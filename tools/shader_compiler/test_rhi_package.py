"""真实编译失败、语义漂移和逻辑映射负例；旧 manifest/fixture 必须原样保留。"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

# 测试导入生成器时不向源码目录写 pycache；产物只进入显式 work 目录。
sys.dont_write_bytecode=True
import tempfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    args=parser.parse_args()
    args.work.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('generator',args.root/'tools/shader_compiler/generate_rhi_package.py')
    generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix='package-negative-',dir=args.work) as directory:
        work=Path(directory);repo=work/'repo';out=work/'output'
        shutil.copytree(args.root/'shaders',repo/'shaders')
        command=[str(args.compiler.resolve()),'--package','--repo',str(repo),'--out',str(out)]
        def compile_package(ok):
            result=subprocess.run(command,capture_output=True,text=True,timeout=60)
            if (result.returncode==0)!=ok:raise AssertionError(result.stdout+result.stderr)
            return result
        compile_package(True)
        manifest=out/'manifest.json';original=manifest.read_bytes()
        artifact_hashes={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file()}
        source=repo/'shaders/d3d12/ToneMap.hlsl';text=source.read_text(encoding='utf-8-sig')
        source.write_text(text+'\n#error M604_FORCED_SECOND_VARIANT_FAILURE\n',encoding='utf-8')
        failed=compile_package(False)
        assert 'M604_FORCED_SECOND_VARIANT_FAILURE' in failed.stderr
        assert manifest.read_bytes()==original
        source.write_text(text.replace('exp2(ExposureEv)','exp2(ExposureEv + 1.0)'),encoding='utf-8')
        failed=compile_package(False)
        assert 'semantic' in failed.stderr
        assert manifest.read_bytes()==original
        for relative,sha in artifact_hashes.items():assert hashlib.sha256((out/relative).read_bytes()).hexdigest()==sha
        contract=work/'bindings.json';baseline=json.loads((args.root/'tools/shader_compiler/m6-binding-contract.json').read_text(encoding='utf-8'))
        contract.write_text(json.dumps(baseline),encoding='utf-8')
        good=generator.build(manifest,contract);assert len(good['packages'])==9
        header=out/'fixture.h';header.write_text(generator.header(good),encoding='utf-8');old_header=header.read_bytes()
        def rejected():
            try:generator.build(manifest,contract)
            except ValueError:return
            raise AssertionError('malformed package was accepted')
        bad=json.loads(json.dumps(baseline));bad['bindings']=[b for b in bad['bindings'] if b['name']!='HdrScene']
        contract.write_text(json.dumps(bad),encoding='utf-8');rejected()
        result=subprocess.run(['python',str(args.root/'tools/shader_compiler/generate_rhi_package.py'),'--manifest',str(manifest),'--bindings',str(contract),'--header',str(header),'--summary',str(out/'logical.json')],capture_output=True,text=True,timeout=15)
        assert result.returncode!=0 and header.read_bytes()==old_header
        bad=json.loads(json.dumps(baseline));next(b for b in bad['bindings'] if b['name']=='HdrScene')['register']=99
        contract.write_text(json.dumps(bad),encoding='utf-8');rejected()
        bad=json.loads(json.dumps(baseline));hdr=next(b for b in bad['bindings'] if b['name']=='HdrScene');cb=next(b for b in bad['bindings'] if b['name']=='ToneMapConstants');hdr['set']=cb['set'];hdr['binding']=cb['binding']
        contract.write_text(json.dumps(bad),encoding='utf-8');rejected()
        contract.write_text(json.dumps(baseline),encoding='utf-8')
        # 全局冲突即使分属两个 shader，也必须拒绝；alias 则明确共用 canonical 元数据。
        bad=json.loads(json.dumps(baseline));next(b for b in bad['bindings'] if b['name']=='SkyboxConstants')['binding']=9
        contract.write_text(json.dumps(bad),encoding='utf-8');rejected()
        bad=json.loads(json.dumps(baseline));next(b for b in bad['bindings'] if b['name']=='IblSampler')['aliases']=['HdrScene']
        contract.write_text(json.dumps(bad),encoding='utf-8');rejected()
        contract.write_text(json.dumps(baseline),encoding='utf-8')
        alias_manifest=json.loads(original)
        for v in next(a for a in alias_manifest['assets'] if a['assetId']=='ToneMapPSMain')['variants'].values():
            sampler=dict(next(b for b in v['reflection']['bindings'] if b['name']=='LinearClampSampler'));sampler['name']='IblSampler';v['reflection']['bindings'].append(sampler)
        manifest.write_text(json.dumps(alias_manifest),encoding='utf-8')
        alias_package=generator.build(manifest,contract)
        assert [a['variants'] for a in alias_package['packages']]==[a['variants'] for a in good['packages']]
        for v in next(a for a in alias_manifest['assets'] if a['assetId']=='ToneMapPSMain')['variants'].values():
            next(b for b in v['reflection']['bindings'] if b['name']=='IblSampler')['comparisonSampler']=True
        manifest.write_text(json.dumps(alias_manifest),encoding='utf-8');rejected()
        manifest.write_bytes(original)
        raw=json.loads(original);bytecode=out/raw['assets'][0]['variants']['d3d11']['bytecode']['path'];bytecode.write_bytes(bytecode.read_bytes()+b'corrupt');rejected()
        print('M604PackageNegatives PASS: second-variant-compile, semantic-drift, unmapped-binding, native-register-drift, logical-collision, cross-pass-collision, canonical-alias, alias-metadata, bytecode-integrity, old-publication-preserved')
if __name__=='__main__':main()
