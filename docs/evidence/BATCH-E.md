# E 批次：候选包、许可隐私终审、独立运行与真实读者

## E1 候选范围与提交身份（进行中）
- 候选源码集合 = PUBLICATION-FILES 的 757 个文件，按 9 组分别提交（本地，未推送）
- 提交身份：xiaoran6657 <xiaoran6657@users.noreply.github.com>；HEAD 3a19a1e884056fcf1ba0f7cf7527bb4be95dd247
- 重新 configure/build 后 EXE 内嵌 sourceCommit=3a19a1e8…；EXE SHA-256 cf9639240d1b2f5f280daf806f7891cd2e014260e74c85a95a2761438914c36c
- 待办：D 验证性重跑（双后端彩排 + Capture）、打包步骤、隐私扫描、E4 独立运行与真实读者、E5 裁定
- 提交后验证性重跑（新 EXE，sourceCommit=b92cda44…）：双后端彩排 6 次全部 PASS
- 新 D3D11 Capture：33,445,445 B，206 事件 / 62 draws，Screenshot=798，RT 导出与 anchor.ppm 平均逐像素差 0.0846
- 新 D3D12 Capture：37,506,362 B，事件列表含 M5.Frame 857 与 Screenshot(677)
- P 适用性检查：当前源码与 P 冻结源码（lb-current-004 快照）差异 10 个文件，全部为文档/清单/.gitignore，无测量代码改动
- 因此 P 的结论对本提交仍适用（无测量路径改动）；P 不重开正式组
- 证据目录：out/e-batch/rehearsal-committed、out/e-batch/capture（仅本地）
- 双后端成片按新 EXE 重录：D3D12 133e92d0…（1,399,205 B）、D3D11 f5ef8a16…（1,495,826 B）
## E2 打包（进行中）
- 打包器 tools/portfolio/build_candidate_package.py：显式输入、fresh 输出、干净工作树、--built-at 与 HEAD 差异只允许 docs/tools
- 首个候选包 out/e-batch/package/lumabough-b92cda4：135 文件 / 10,029,539 B；含 EXE、WinPixEventRuntime.dll、4 个 VC 运行时 DLL、双后端 shaders、烘焙场景、许可与说明
- 包清单 PACKAGE-MANIFEST.json 记录 packagingCommit=8ef79fc4、builtAtCommit=b92cda4、exeSha256=16ebb979…、场景 4ae6eda9…、逐文件 SHA-256；无 PDB
- 发现（E2 要求）：M610_PROJECT_ROOT 是编译期绝对路径，着色器语义哈希与日志都依赖源码树；本机恰好存在源码树，所以同机运行掩盖了这一点
- 修复方案（待执行）：调用点改为优先用 EXE 旁 shaders（运行包携带），缺失时回退源码树；同时把 d3d12 的 HLSL 源一并入包；修复后需重建 + D 验证性重跑 + 重新打包
