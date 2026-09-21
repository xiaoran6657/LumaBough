"""Negative fixtures exercise the production benchmark verifier without a GPU."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('evidence', ROOT/'tools/legacy/M5BenchmarkEvidence.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)

def fixture(backend):
    report = dict(result='BASELINE',buildType='Release',backend=backend.upper(),compiler='fixture',resolution=[1280,720],warmupFrames=120,measuredFrames=600,windowForeground=True,debugLayer=False,gpuValidation=False,dredActive=False,captureAttached=False,captureActive=False,vsync=False,gpu=dict(adapter='fixture GPU',driver='1.2.3.4'),assetManifestSha256='a'*64,environmentArtifactSha256='b'*64,visibleSequenceHash='0x1234',iblProfile='baseline',exposureEv=0,counts=dict(mainVisibleMedian=30,drawsMedian=60,trianglesMedian=25660),gpuSamples=dict(valid=600,missing=0,failed=0))
    report['metricsMs'] = {name:dict(median=1,p95=2,p99=3) for name in evidence.METRICS}
    if backend == 'd3d12':
        report['counts'].update({name:1 for name in evidence.HIGH_WATER})
    else:
        report['gpuSamples'].update(valid=719,measuredValid=600,disjoint=0,ringSkippedFrames=0)
    return report

class NativeEvidenceTests(unittest.TestCase):
    def test_valid_both_schemas(self):
        for backend in ('d3d11','d3d12'):
            evidence.validate(fixture(backend),backend,'a'*64)

    def test_reject_each_diagnostic_flag_and_vsync(self):
        for field in ('debugLayer','gpuValidation','dredActive','captureAttached','captureActive','vsync'):
            with self.subTest(field=field):
                report=fixture('d3d12');report[field]=True
                with self.assertRaises(ValueError): evidence.validate(report,'d3d12','a'*64)

    def test_missing_diagnostic_is_not_false(self):
        report=fixture('d3d12');del report['captureActive']
        with self.assertRaises(ValueError): evidence.validate(report,'d3d12','a'*64)

    def test_reject_unknown_driver_and_backend(self):
        for key in ('driver','backend'):
            report=fixture('d3d12')
            if key == 'driver': report['gpu']['driver']='unknown'
            else: report['backend']='D3D11'
            with self.assertRaises(ValueError): evidence.validate(report,'d3d12','a'*64)

    def test_reject_bad_quantiles(self):
        for value in (-1,float('nan'),float('inf'),4):
            report=fixture('d3d12');report['metricsMs']['gpuFrame']['median']=value
            with self.assertRaises(ValueError): evidence.validate(report,'d3d12','a'*64)

    def test_measured_samples_not_lifetime_total(self):
        report=fixture('d3d11');report['gpuSamples']['measuredValid']=599
        with self.assertRaises(ValueError): evidence.validate(report,'d3d11','a'*64)

    def test_highwater_required_only_for_d12(self):
        evidence.validate(fixture('d3d11'),'d3d11','a'*64)
        report=fixture('d3d12');del report['counts']['psosHighWater']
        with self.assertRaises(ValueError): evidence.validate(report,'d3d12','a'*64)

    def test_cross_run_identity_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            for backend in ('d3d11','d3d12'):
                (Path(directory)/f'{backend}-1.json').write_text(json.dumps(fixture(backend)))
            evidence.validate_directory(directory,'a'*64,runs=1)
            for field in ('windowForeground','trianglesMedian'):
                report=fixture('d3d12')
                if field == 'windowForeground': report[field]=False
                else: report['counts'][field]+=1
                (Path(directory)/'d3d12-1.json').write_text(json.dumps(report))
                with self.assertRaises(ValueError): evidence.validate_directory(directory,'a'*64,runs=1)

if __name__ == '__main__': unittest.main()
