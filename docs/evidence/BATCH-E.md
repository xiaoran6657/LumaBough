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
