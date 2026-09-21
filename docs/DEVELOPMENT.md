# 开发、构建与验证

从仓库根执行。当前 CMake Presets 要求 Windows x64、VS 2026（Visual Studio 18）、
MSVC v145、Windows SDK、CMake 4.2+，脚本需要 Python 3。
Python 图像测试使用 Pillow 与 NumPy，版本固定在 requirements-dev.txt。先建立本地隔离环境：

~~~powershell
python -m venv out/python
out/python/Scripts/python.exe -m pip install -r requirements-dev.txt
~~~
源码中不包含 SDK、驱动、编译器或依赖的预编译二进制。

## 工具链和双配置

~~~powershell
. ./tools/dev/Enter-MiniEngineDevShell.ps1
Get-Command cmake
cmake --version
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --parallel 6
ctest --preset windows-msvc-debug --output-on-failure
cmake --build --preset windows-msvc-debug --config Release --parallel 6
ctest --preset windows-msvc-debug -C Release --output-on-failure
~~~

Debug 和 Release 共用 multi-config 构建树。
不要使用不存在的 windows-msvc-release preset。
测试继续走 preset，保留 TEMP/TMP 指向本仓库 out/tmp，避免某些 Windows 测试使用非 ASCII 临时路径。
PowerShell 5.1 可运行现有 .ps1；公共命令不要求另装 pwsh。
无需因构建安装 clang-format；它只服务格式化。

独立的 windows-msvc-profile 与 windows-msvc-profile-tracy 构建树服务性能采集；
它们仍是 VS multi-config 树，build preset 选择 Release。Tracy 只在后者开启。
普通 Debug/Release 回归与 profile 性能结果不可混用。

CMake 从所选 SDK 的注册表路径或 PATH 寻找 dxc.exe，缺失时配置明确失败；
也可显式传 -DMINIENGINE_DXC_EXECUTABLE=<SDK中的dxc.exe>。
VS CMake 可通过开发 shell 进入，避免 PATH 先命中旧版 Python cmake shim。

## 最小运行与固定 PBR 场景

无需烘焙的 smoke：

~~~powershell
out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --rhi=d3d11 --smoke-level=6 --frames=9 --debug --output=out/smoke/d3d11
out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --rhi=d3d12 --smoke-level=6 --frames=9 --debug --output=out/smoke/d3d12
~~~

固定 PBR 场景只准备所需 recipe，不把性能 recipe 当 cooker 输入：

~~~powershell
$env:MGE_GLTF_VALIDATOR = (Get-Command gltf_validator.exe -ErrorAction Stop).Source
New-Item -ItemType Directory -Force out/demo/recipes
Copy-Item assets/recipes/m4-visual-baseline.asset.json out/demo/recipes/
out/build/windows-msvc-debug/tools/asset_cooker/Release/MiniEngineAssetCooker.exe cook --source-root assets/source --recipe-root out/demo/recipes --output out/demo/scene --profile windows-d3d11 --validator $env:MGE_GLTF_VALIDATOR
out/build/windows-msvc-debug/samples/rhi_sandbox/Release/MiniEngineSandbox.exe --rhi=d3d12 --scene=m4-visual-baseline --manifest=out/demo/scene/manifest.json --migration-level=9 --fixed-frame=300 --width=1280 --height=720 --debug --headless --output=out/demo/d3d12
~~~

固定场景烘焙需要 [glTF Validator 2.0.0-dev.3.10](https://github.com/KhronosGroup/glTF-Validator/releases/tag/2.0.0-dev.3.10)。
将解压后的 gltf_validator.exe 加入当前 PATH，或把 MGE_GLTF_VALIDATOR 设为其完整路径。
本批使用机器上已有版本，没有把其二进制复制入仓库。实测记录见 [批次 B](evidence/BATCH-B.md)。
输出目录中画面、metadata、诊断日志是本次运行产物；手动复制到公共材料前检查身份与隐私。
不存在“仅换 GitHub 链接就继承原运行”的流程。

## 测试与诊断

[tests/README](../tests/README.md) 区分纯 CPU、GPU、工具和文档检查。
完整命令的实际实例数以 CTest 输出为准。DRED 真实 TDR 属显式运行项目，
默认测试中的跳过不表示已经重新执行设备移除验收。

~~~powershell
ctest --preset windows-msvc-debug --show-only=json-v1
ctest --preset windows-msvc-debug -R 'Graph|AssetLoad|Task|RhiBoundary|M7Comparison' --output-on-failure
out/python/Scripts/python.exe -B -m unittest discover -s tests/tools -p "test_*.py"
python tools/validation/check_rhi_boundary.py
python tools/portfolio/validate_publication.py --stage entry
~~~

依赖图核对使用 CMake File API：配置前创建
out/build/windows-msvc-debug/.cmake/api/v1/query/codemodel-v2 空文件，再配置并执行：

~~~powershell
python tools/validation/check_rhi_composition.py --build out/build/windows-msvc-debug --output out/architecture/composition.json
~~~

工具自测已集中到 tests/tools/contracts；每个工具目录的职责见 [tools/README](../tools/README.md)。
历史复算工具位于 tools/legacy，需要自己的输入，不是当前一键验收流程。

## 性能证据和外部工具

新性能采集先生成自己的机器 manifest 到 out/performance/environment.json；
tests/fixtures/performance/environment.schema-example.json 仅为脱敏 schema fixture，
不能冒充当前采集环境。所有 raw、日志、PDB、Capture 留 out/artifacts，
公开前单独审查。证据归档器必须显式指定 destination-root，不能默认写任何私人目录。

RenderDoc、PIX GUI、Tracy GUI 和 glTF Validator 是可选工作流工具；
使用它们不等于允许把安装目录随包分发。普通源码构建无需安装这些 GUI。
CMake 获取的 WinPixEventRuntime 是单独的构建/运行依赖。

## 已知边界

当前分发包、Demo overlay 和最终 anchor capture 尚未完成。
源码尚无首个提交时，编译身份记 uncommitted，实际验收绑定逐文件快照；
最终发布仍必须绑定真实 runtimeCommit。绝对源码路径可能进入 PDB / shader debug data，
所以本地构建通过并不等于调试产物已适合公开。
