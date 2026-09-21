# 批次 B：目录、依赖与公共入口验收

2026-09-20 完成当前本地 entry 范围。Debug、Release 全量构建通过；
两种配置各运行 1003 项 CTest，均为 0 失败。尚未提交、推送、建立远端或公开。

## 范围和目录

所有者明确确认 673 个文件的候选范围；清单自身另列 selfExcludedPath。
636 个来自 A 阶段候选的保留/迁移，37 个为本批新增文档、图、许可及工具。
[文件清单](PUBLICATION-FILES.json) 与 [路径映射](PATH-MAP.json) 绑定实际 SHA-256，
其中映射记录 107 项，47 项移到本地 out。范围刷新遇到任何新增文件即失败。

tools 按 dev/assets/evidence/performance/validation/capture/legacy/portfolio 分组；
C++ 工具项目保留原 target 与位置。Python 契约测试集中到 tests/tools/contracts。
历史基线、旧 V1 portfolio 脚本、退休样例驱动器及缓存保留在 out 的本地存档。
M7SchemaJsonEvidence 改用脱敏 schema fixture，不把它当实测机器环境。

仅移除依赖私有性能账本的 M7ExperimentViews 归档一致性检查；
新增 PortfolioEntry、PortfolioImportContracts。运行契约测试未删除。
旧 M6 验收驱动器中的两个纯汇总函数抽取到 m6_summary.py，原 6 项汇总回归保留。

## 入口与来源

根 README、DEVELOPMENT、文档/工具/测试入口、路线图和变更记录已重写。
三张架构图提供 JSON 图源、生成脚本、SVG/PNG 和导出 hash；
已查看图像布局及深色背景下的可读性。三个决策说明链接当前源码。

第三方来源、许可文本、模型署名与所有 recipe 完成登记：
15 个第三方代码/资产载荷与所列上游字节一致；simdjson 为 Apache-2.0，
MikkTSpace 为 Zlib。SimpleSparseAccessor 按较严格的 CC-BY-4.0 署名。
详情见 [许可复核](LICENSING-REVIEW.md) 与 [完整声明](../../THIRD-PARTY-NOTICES.md)。

公开文件模式复查的剩余命中已逐项阅读，均为合成测试、标准安装位置、
排除规则、扫描规则本身或历史原件相对 ID；逐行绑定保存在 PATH-MAP。
未发现凭据模式或私人源库 URL 命中；模式扫描不能替代最终发布复核。

## 构建与运行

实测工具链：Windows x64，VS 2026 / MSVC 19.51，CMake 4.3.1-msvc1，
Windows SDK 10.0.26100.0，Python 3.14；GPU 为 AMD Radeon RX 9070。
Pillow 12.1.0 与 NumPy 2.4.2 安装在本仓库 out/python，requirements-dev.txt 固定版本。
开发 shell 已实际验证，PowerShell 5.1 语法检查通过。

| 检查 | 结果 |
|---|---|
| Debug / Release 全量构建 | 两者通过 |
| Debug / Release CTest | 各 1003 项，0 失败 |
| Python 工具单测 | 42 项通过 |
| 导入与公开链接安全契约 | 14 项通过 |
| 当前源码 RHI boundary | PASS |
| boundary 检查器自测 | 33 个负例、9 个正例通过 |
| CMake File API composition | 4 个生成配置通过；不等于四种配置均构建 |
| D3D11 / D3D12 smoke | 各 9 帧，nativeWarningErrors=0，alive/retiring=0 |
| 固定 PBR 场景 | 两后端各 300 帧、1280×720，warningErrors=0 |
| entry 文件与文档链接 | PASS；不验证远程 URL 匿名访问 |

复现命令见 [开发指南](../DEVELOPMENT.md)；本地完整命令和日志在 out/m9/batch-b。
两后端 graphHash 均为 0x435B344F438E02DA，commandHash 均为
a74983f5042807a3e52bbe0e788eebe5a95c672f4f4ab1d3394c0b1b47c25ebf。
这次是 720p 入口验证，不替代后续 1080p Demo anchor。

[README 预览身份](RENDER-PREVIEW.json) 记录程序、实际 C++/HLSL 文件快照、
输入/着色器身份、原始输出及 PNG hash。PPM 转 PNG 未改变 RGB 像素；
两后端像素差异只作描述性记录，没有据此新增 parity PASS 声明。

D3D12 退出日志有 Live Object census：发生在 backend 基础设施析构前；
代码按具体对象身份核对，实测 unexpectedNativeResources=0。
因此本次结论是验证/退休门通过，不能写成“所有日志无警告”或“已证明进程无泄漏”。

## 本次修复与边界

配置时修复 SDK DXC 定位、无 HEAD 的构建身份与 Python 依赖缺口。
全量回归找到并修复一处迁移后导入，以及 Windows PowerShell 5.1 从 Python
继承不同 PSModulePath 时的 Utility 模块发现问题；证据归档器同时支持未提交仓库。
保留最初失败日志及后续通过日志，没有通过删除失败测试取得结果。

runtimeCommit 与 publicationCommit 仍为 null；程序输出 uncommitted。
当前逐文件快照能绑定本地结果，最终发布必须提交后重建并绑定真实提交。
未执行新的长时 soak、性能 A/B、真实强制 TDR 或 GUI Capture，
也未制作运行包、动态视频和最终 overlay。Claim Ledger 的历史证据缺口仍保持 BLOCKED。

下一批 C：最小 Demo / rehearsal 与选中性能案例重算。静态定位、架构和结论已由文档承担，
视频只展示需要连续时间过程理解的行为。下一批由所有者另行指派。

## 本地日志身份

以下 SHA-256 绑定本地原始日志；日志可能含机器路径，不进入公开文件集合。

| 日志标签 | SHA-256 |
|---|---|
| configure-fixed | 816b4aefd34db31dbf5c56e291f2a232da1206eba7a4f7f06bcfb18ebff4fba0 |
| build-debug | 7764e0bafe027b7b8520ec06553a6a868365f7aeb3422757dd7c4ebb444def20 |
| build-release | 7f3fb97e6682bd42b3f7d80a3b1c0a901f75acdf5ef804789c48784dd83053c2 |
| ctest-debug-final | d836f44376b5cf33d98e78ba3479044a9ec646369b296583512d89cb2a5377cf |
| ctest-release-final | eea0a82b740af66683c2e1dc5262bcd629663b9e287ca73a55a12747136ea9ff |
| python-tools-complete | 5b7d526dba7d3e9df856120ccd8eb762838032a32d84cca5d63f4ad7cb71abde |
| rhi-boundary-current | 6369634886864516f4fde5eba9b092ff401e5aca838b8ffcda0637b3cb7dc8e5 |
| rhi-boundary | 4b5bc691095993a6d7b3a484291f987c039d43967f4587d8c995fb2ee4cf5620 |
| composition | 239255a9ea15add2e3592f48b7760e0fe7f551568624985dd0cb7cea1fd6d35f |
| powershell | f9e9d6e684eee47fd4557275c2bd2b2280242a26bf95960bb2b1ee45ac5a2c72 |
| cook | f8c037de3c11b872734c3bbaf331e74e052112f26311c5ec5e13d027fc64efab |
| smoke-d3d11 | 045ae7709c8459936d62313d35c0fd87cfff95b7cb95d3e3aab4db32a5f150fd |
| smoke-d3d12 | dcaec4a9c74321c29ce1d452a2e36afd217661871bb8a662da615f2877c9b40a |
| pbr-d3d11 | a0c8d883c1c730375654a45e081750acf08c6444bd215e820ecf8fa16b1b6bf3 |
| pbr-d3d12 | 717d07b27131ba507cebdc26507a2ac5805f011922499f829b3fb3a5ceea659a |
