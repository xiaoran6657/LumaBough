"""E2：候选运行包打包器（可复用、输入显式、默认拒绝）。
只带"运行必需 + 许可与说明"：不复制开发工具、SDK、资产源树、PDB、Capture 或视频。"
"""
import argparse,hashlib,json,re,shutil,subprocess,sys,zipfile
from pathlib import Path
CRT=['msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll']
# 与 --crt-dir 下的实际 Redist 文件一致（打包时逐字节复制并记录 SHA-256）。
CRT_VERSION='14.51.36247.0'
from validate_publication import build_sensitive
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
    ap.add_argument('--zip',type=Path,default=None,help='同时产出确定性交付 ZIP')
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
        # 与候选门共用同一个入口 build_sensitive()：只有影响 EXE/场景的改动才要求重建。
        bad=[c for c in changed if build_sensitive(c)]
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
    lic={'licenses/LICENSE':'LICENSE'}
    for k,v in lic.items(): plan[k]=R/v
    plan['licenses/WinPixEventRuntime-license.txt']=a.pix_src/'license.txt'
    plan['licenses/WinPixEventRuntime-ThirdPartyNotices.txt']=a.pix_src/'ThirdPartyNotices.txt'
    exe_sha=sha(exe); scene_sha=sha(a.scene/'manifest.json')
    # E5：包内许可说明按运行包重新整理——源码树的相对链接在 licenses/ 下会失效，改写为纯文本路径。
    def local_edition(source, title):
        text=(R/source).read_text(encoding='utf-8')
        def rewrite(match):
            label,target=match.group(1),match.group(2)
            if target.startswith(('http://','https://','mailto:')):return match.group(0)
            return label+'（源码仓库路径：'+target+'）'
        text=re.sub(r'\[([^\]]*)\]\(([^)]+)\)',rewrite,text)
        header=('# '+title+'（运行包副本）','',
                '本文件随运行包分发；源码仓库中的相对链接在包内无效，已改写为纯文本路径，外部链接保持原样。','')
        return chr(10).join(header)+text
    readme=['LumaBough candidate runtime package','','commit '+commit,'EXE sha256 '+exe_sha,
            'scene manifest sha256 '+scene_sha,'','See RUN.md, SUPPORT-MATRIX.md and PACKAGE-MANIFEST.json.']
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
    (out/'licenses/THIRD-PARTY-NOTICES.md').write_text(local_edition('THIRD-PARTY-NOTICES.md','第三方 notices'),encoding='utf-8')
    (out/'licenses/ASSET-LICENSES.md').write_text(local_edition('assets/LICENSES.md','资产许可'),encoding='utf-8')
    redist=['# 随包可再分发的第三方运行时','',
            '| 文件 | 版本 | SHA-256 |','|---|---|---|']
    for d in CRT: redist.append('| '+d+' | '+CRT_VERSION+' | '+sha(a.crt_dir/d)+' |')
    redist+=['| WinPixEventRuntime.dll | 随 PIX NuGet 包 | '+sha(a.exe_dir/'WinPixEventRuntime.dll')+' |','',
             '来源与条款：','',
             '- 4 个 VC 运行时 DLL 取自 Visual Studio Build Tools 的 VC Redist 目录（Microsoft.VC145.CRT），',
             '  按应用本地（app-local）方式随包分发；版本与 Redist 原件逐字节一致，未做修改。',
             '  适用条款见 Microsoft 的可再分发代码说明：https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution',
             '- WinPixEventRuntime.dll 来自 PIX NuGet 包，许可与第三方 notices 随包提供（同目录两个 WinPixEventRuntime-* 文件）。',
             '- 本项目自有代码使用 MIT（见同目录 LICENSE）；MIT 只覆盖自有部分，不覆盖上述第三方二进制。']
    (out/'licenses/REDISTRIBUTABLES.md').write_text(chr(10).join(redist)+chr(10),encoding='utf-8')
    (out/'README.md').write_text(chr(10).join(readme)+chr(10),encoding='utf-8'); (out/'RUN.md').write_text(chr(10).join(run)+chr(10),encoding='utf-8')
    (out/'SUPPORT-MATRIX.md').write_text(chr(10).join(sup)+chr(10),encoding='utf-8')
    files={f.relative_to(out).as_posix():{'size':f.stat().st_size,'sha256':sha(f)} for f in sorted(out.rglob('*')) if f.is_file()}
    man={'schemaVersion':1,'kind':'LumaBough candidate runtime package','packagingCommit':commit,'builtAtCommit':built,'exeSha256':exe_sha}
    man['sceneManifestSha256']=scene_sha; man['files']=files
    man['excluded']=['DXC/VS/SDK','asset sources','PDB','captures','videos','Tracy','scene reports (baker absolute paths)']
    man['selfExcluded']=['PACKAGE-MANIFEST.json','SHA256SUMS.txt']  # 自引用不可行，明确标注而非静默省略
    (out/'PACKAGE-MANIFEST.json').write_text(json.dumps(man,ensure_ascii=False,indent=2)+chr(10),encoding='utf-8')
    sums=chr(10).join(v['sha256']+'  '+k for k,v in files.items())+chr(10)
    (out/'SHA256SUMS.txt').write_text(sums,encoding='utf-8')
    if a.zip:
        # 确定性交付 ZIP：排序 + 固定时间戳，条目名与包内相对路径一致（扫描/清单同一套键）。
        names=sorted(p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file())
        with zipfile.ZipFile(a.zip,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            for name in names:
                info=zipfile.ZipInfo(name,date_time=(2026,1,1,0,0,0))
                info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=0o644<<16
                z.writestr(info,(out/name).read_bytes())
        print(json.dumps({'zip':str(a.zip),'zipBytes':Path(a.zip).stat().st_size,'zipSha256':sha(a.zip)},ensure_ascii=False))
    print(json.dumps({'files':len(files),'bytes':sum(v['size'] for v in files.values()),'commit':commit,'exeSha256':exe_sha},ensure_ascii=False))
    return 0
if __name__=='__main__': sys.exit(main())
