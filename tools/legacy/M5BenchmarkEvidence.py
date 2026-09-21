"""M5-11: validate native benchmark evidence before permitting comparisons."""
import argparse
import json
import math
from pathlib import Path

METRICS = 'cpuFrame fixedUpdate worldExtract frustumCull renderQueueSort renderSubmit presentCpu gpuFrame gpuShadow gpuOpaquePbr gpuSkybox gpuToneMap'.split()
IDENTITY = 'gpu.adapter gpu.driver assetManifestSha256 environmentArtifactSha256 visibleSequenceHash resolution iblProfile exposureEv windowForeground counts.mainVisibleMedian counts.drawsMedian counts.trianglesMedian'.split()
HIGH_WATER = 'descriptorHighWater psosHighWater barriersPerFrameHighWater constantRingHighWater uploadRingHighWater'.split()

def value(report, path):
    current = report
    for part in path.split('.'):
        if not isinstance(current, dict) or part not in current:
            raise ValueError(f'missing field: {path}')
        current = current[part]
    if current is None or current == '':
        raise ValueError(f'empty field: {path}')
    return current

def validate(report, backend, manifest_hash, warmup=120, frames=600):
    for field in IDENTITY + ['compiler']:
        value(report, field)
    for field in ('debugLayer', 'gpuValidation', 'dredActive', 'captureAttached', 'captureActive', 'vsync'):
        if value(report, field) is not False:
            raise ValueError(f'{field} must explicitly be false')
    if value(report, 'windowForeground') is not True:
        raise ValueError('windowForeground must explicitly be true')
    for field, expected in {'result': 'BASELINE', 'buildType': 'Release', 'backend': backend.upper(), 'resolution': [1280,720], 'warmupFrames': warmup, 'measuredFrames': frames}.items():
        if value(report, field) != expected:
            raise ValueError(f'{field} mismatch')
    if 'unknown' in value(report, 'gpu.driver').lower():
        raise ValueError('unknown GPU driver')
    for field in ('assetManifestSha256', 'environmentArtifactSha256'):
        digest = value(report, field)
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest.lower()):
            raise ValueError(f'invalid digest: {field}')
    if value(report, 'assetManifestSha256').lower() != manifest_hash.lower():
        raise ValueError('manifest hash mismatch')
    measured = 'gpuSamples.measuredValid' if backend == 'd3d11' else 'gpuSamples.valid'
    if value(report, measured) != frames or value(report, 'gpuSamples.valid') < frames:
        raise ValueError('incomplete measured GPU samples')
    for field in ('gpuSamples.missing', 'gpuSamples.failed'):
        if value(report, field) != 0:
            raise ValueError(f'{field} is nonzero')
    # D3D12 timestamps have no D3D11_QUERY_TIMESTAMP_DISJOINT mechanism.
    if backend == 'd3d11':
        for field in ('gpuSamples.disjoint', 'gpuSamples.ringSkippedFrames'):
            if value(report, field) != 0:
                raise ValueError(f'{field} is nonzero')
    else:
        for field in HIGH_WATER:
            v = value(report, 'counts.' + field)
            if type(v) is not int or v < 0:
                raise ValueError(f'invalid high water: {field}')
    for metric in METRICS:
        numbers = [value(report, f'metricsMs.{metric}.{q}') for q in ('median','p95','p99')]
        if any(type(n) not in (int,float) or not math.isfinite(n) or n < 0 for n in numbers) or numbers != sorted(numbers):
            raise ValueError(f'invalid quantiles: {metric}')

def validate_directory(directory, manifest_hash, runs=3, warmup=120, frames=600, backends=('d3d11','d3d12')):
    records = []
    for backend in backends:
        for run in range(1,runs+1):
            path = Path(directory) / f'{backend}-{run}.json'
            report = json.loads(path.read_text(encoding='utf-8-sig'))
            validate(report, backend, manifest_hash, warmup, frames)
            records.append({'backend':backend, 'run':run, 'path':str(path), 'report':report})
    first = records[0]['report']
    for record in records:
        for field in IDENTITY:
            if value(record['report'],field) != value(first,field):
                raise ValueError(f"{record['backend']}-{record['run']}: identity mismatch: {field}")
    return {'status':'FACTS_ONLY', 'warmupFrames':warmup, 'measuredFrames':frames, 'runs':runs, 'records':records}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--backend', choices=('d3d11','d3d12'), action='append')
    parser.add_argument('--summary', default='summary.json')
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--warmup',type=int,default=120)
    parser.add_argument('--frames',type=int,default=600)
    args = parser.parse_args()
    try:
        summary = validate_directory(args.directory,args.manifest_sha256,args.runs,args.warmup,args.frames,args.backend or ('d3d11','d3d12'))
        (args.directory/args.summary).write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
        print('FACTS_ONLY: all native reports validated')
    except (ValueError, OSError, TypeError) as error:
        parser.exit(3, f'BLOCKED: {error}\n')
