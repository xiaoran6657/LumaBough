"""E2：候选运行包打包器（可复用、输入显式、默认拒绝）。
只带"运行必需 + 许可与说明"：不复制开发工具、SDK、资产源树、PDB、Capture 或视频。"
"""
import argparse,hashlib,json,shutil,subprocess,sys
from pathlib import Path
CRT=['msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll']
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p): return json.loads(Path(p).read_text(encoding='utf-8'))
def linkish(p): return Path(p).is_symlink() or (hasattr(Path(p),'is_junction') and Path(p).is_junction())
def copy(src,dst):
    assert not linkish(src), f'link in input: {src}'
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(src,dst); return dst
def main():
    R=Path(__file__).resolve().parents[2]
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--exe-dir',type=Path,default=R/'out/build/windows-msvc-debug/samples/rhi_sandbox/Release')
    ap.add_argument('--scene',type=Path,default=R/'out/demo/scene')
    ap.add_argument('--crt-dir',type=Path,default=Path('C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT'))
    ap.add_argument('--pix-src',type=Path,default=R/'out/build/windows-msvc-debug/_deps/miniengine_pix-src')
    ap.add_argument('--built-at',default=None,help='构建该二进制的提交（默认=当前 HEAD）')
    a=ap.parse_args()
    out=a.output.resolve()
    if out.exists(): raise SystemExit('output must be fresh: '+str(out))
    commit=subprocess.run(['git','-C',str(R),'rev-parse','HEAD'],capture_output=True,text=True,check=True).stdout.strip()
    dirty=subprocess.run(['git','-C',str(R),'status','--porcelain'],capture_output=True,text=True).stdout.strip()
    if dirty: raise SystemExit('working tree must be clean: '+dirty[:200])
    exe=a.exe_dir/'MiniEngineSandbox.exe'
    for p in [exe,a.exe_dir/'WinPixEventRuntime.dll',a.crt_dir]: _=[x for x in [p]][0]
    for p in [exe,a.exe_dir/'WinPixEventRuntime.dll',a.crt_dir,a.scene,a.pix_src/'license.txt']:
        if not Path(p).exists(): raise SystemExit('missing input: '+str(p))
    built=a.built_at or commit
    if built!=commit:
        changed=subprocess.run(['git','-C',str(R),'diff','--name-only',built+'..'+commit],capture_output=True,text=True,check=True).stdout.split()
        # 只有 docs/、tools/、tests/ 允许在构建之后继续改动：它们不进 EXE。engine/samples/shaders/assets 一旦改动必须重建。
        bad=[c for c in changed if not c.startswith(('docs/','tools/','tests/'))]
        if bad: raise SystemExit('runtime code changed since --built-at; rebuild required: '+', '.join(bad))
    plan={}
    plan['runtime/MiniEngineSandbox.exe']=exe
    plan['runtime/WinPixEventRuntime.dll']=a.exe_dir/'WinPixEventRuntime.dll'
    for d in CRT: plan['runtime/'+d]=a.crt_dir/d
    sh=a.exe_dir/'shaders'
    for f in sorted(sh.rglob('*')):
        if f.is_file() and f.suffix.lower()!='.pdb':
            plan['runtime/'+f.relative_to(a.exe_dir).as_posix()]=f
    for f in sorted(a.scene.rglob('*')):
        # E3：scene/reports/*.asset.json 记录烘焙机的绝对源路径，不随包交付（运行不需要它）。
        if f.is_file() and 'reports' not in f.relative_to(a.scene).parts:
            plan['scene/'+f.relative_to(a.scene).as_posix()]=f
    # d3d12 的 HLSL 源：运行包需要它才能算着色器语义哈希（EXE 旁的 shaders/d3d12 只有编译产物）。
    for f in sorted((R / 'shaders/d3d12').rglob('*')):
        if f.is_file() and f.suffix.lower() in {'.hlsl', '.hlsli'}:
            plan['runtime/shaders/d3d12/' + f.name] = f
    lic={'licenses/LICENSE':'LICENSE','licenses/THIRD-PARTY-NOTICES.md':'THIRD-PARTY-NOTICES.md','licenses/ASSET-LICENSES.md':'assets/LICENSES.md'}
    for k,v in lic.items(): plan[k]=R/v
    plan['licenses/WinPixEventRuntime-license.txt']=a.pix_src/'license.txt'
    plan['licenses/WinPixEventRuntime-ThirdPartyNotices.txt']=a.pix_src/'ThirdPartyNotices.txt'
    exe_sha=sha(exe); scene_sha=sha(a.scene/'manifest.json')
    readme=('LumaBough candidate runtime package\\n\\ncommit '+commit+'\\nEXE sha256 '+exe_sha+'\\nscene manifest sha256 '+scene_sha+'\\n\\nSee RUN.md, SUPPORT-MATRIX.md and PACKAGE-MANIFEST.json.\\n')
    run=['# 运行说明','1) 解压到任意目录（不要放进源码树）','2) 在本目录下执行：']
    run+=['   runtime\\\\MiniEngineSandbox.exe --rhi=d3d12 --scene=m4-visual-baseline --manifest=scene/manifest.json --migration-level=9 --frames=1202 --width=1920 --height=1080 --output=out-run']
    run+=['3) 期望：stdout 出现 status PASS 与 graphHash；产物写在 --output 指定目录','4) 切换后端把 --rhi=d3d12 换成 d3d11']
    run+=['注意：包内不带 DXC/VS/SDK/资产源树/PDB；D3D11 用随包 HLSL 在运行时编译。']
    sup=['# 真实支持矩阵（按实测填写，不做通用承诺）','','| 项 | 实测 |','|---|---|']
    sup+=['| 操作系统 | Windows 10/11 x64（UCRT） |','| 后端 | D3D12（本机实测）、D3D11（本机实测） |']
    sup+=['| GPU | AMD Radeon RX 9070（driver 32.0.31035.1003）本机实测 |','| 场景 | m4-visual-baseline（M6 Demo / 连续事件） |']
    sup+=['| 不支持 | m7-* 性能场景（源码树相对路径）、Tracy、Capture 工具链 |','| 运行时 | 随包 app-local VC 运行时 4 个 DLL + WinPixEventRuntime.dll |']
    sup+=['','本包只代表上表实测范围；历史 C-M9-002 BLOCKED 与当前 P 结果（无 ACCEPTED）不构成收益声明。']
    for dst,src in plan.items(): copy(Path(src), out/dst)
    (out/'README.md').write_text(readme,encoding='utf-8'); (out/'RUN.md').write_text(chr(10).join(run)+chr(10),encoding='utf-8')
    (out/'SUPPORT-MATRIX.md').write_text(chr(10).join(sup)+chr(10),encoding='utf-8')
    files={f.relative_to(out).as_posix():{'size':f.stat().st_size,'sha256':sha(f)} for f in sorted(out.rglob('*')) if f.is_file()}
    man={'schemaVersion':1,'kind':'LumaBough candidate runtime package','packagingCommit':commit,'builtAtCommit':built,'exeSha256':exe_sha}
    man['sceneManifestSha256']=scene_sha; man['files']=files
    man['excluded']=['DXC/VS/SDK','asset sources','PDB','captures','videos','Tracy','scene reports (baker absolute paths)']
    man['selfExcluded']=['PACKAGE-MANIFEST.json','SHA256SUMS.txt']  # 自引用不可行，明确标注而非静默省略
    (out/'PACKAGE-MANIFEST.json').write_text(json.dumps(man,ensure_ascii=False,indent=2)+chr(10),encoding='utf-8')
    sums=chr(10).join(v['sha256']+'  '+k for k,v in files.items())+chr(10)
    (out/'SHA256SUMS.txt').write_text(sums,encoding='utf-8')
    print(json.dumps({'files':len(files),'bytes':sum(v['size'] for v in files.values()),'commit':commit,'exeSha256':exe_sha},ensure_ascii=False))
    return 0
if __name__=='__main__': sys.exit(main())
