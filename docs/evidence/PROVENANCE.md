# 来源与公开边界

LumaBough 从所有者的私有 MiniEngine 工程独立导入。初始手动复制 669 个文件：
663 个源库 tracked 文件与 6 个 Python 缓存，逐字节匹配；没有复制旧 Git 历史。
来源 HEAD 为 9012c54f0dc3af1ab269c4721740503f9a7beb9a；当时源工作区另有 M9 规划修订，
因此提交号不代表全部源工作区，逐文件 sourceSha256 才是实际导入身份。

## 批次 B

所有者明确确认 673 个文件的本地候选范围，其中 636 个来自 A 阶段候选的保留或迁移，
37 个为 B 阶段新增公开文档、图、许可及工具。完整范围见 [文件清单](PUBLICATION-FILES.json)。
旧基线、缓存与退休样例驱动器移至本地 out，不属于该范围。
[路径映射](PATH-MAP.json) 保留原/新身份；纯 M6 汇总函数从旧验收驱动器中抽出并保留契约测试。

本批重排工具和 Python 测试，修正 CMake 接线、DXC 定位及无 HEAD 的版本记录；
源码中的私人文档注释改指公开架构。没有改渲染算法或冻结场景数据。
实际构建、测试、图像及限制见 [B 记录](BATCH-B.md)。
源仓库保持原状态，没有回写或双向同步。

## 身份

- sourceCommit：私有导入或历史运行的代码提交。
- evidenceRecordCommit：原报告提交，不能作为受测版本。
- runtimeCommit：本仓库实际受测代码提交，尚无首个提交，因此为 null；程序输出 uncommitted。
- publicationCommit：最终发布版本，目前 null。

本次实际 C++/HLSL 文件快照和程序 SHA-256 记录在 [预览身份](RENDER-PREVIEW.json)。
它能绑定本地验证，不能冒充最终提交后的构建。最终发布仍须绑定真实 runtimeCommit。
私有 SHA 不拼接到新仓库 GitHub URL；历史摘要的 hash 不冒充原报告 hash。

公开清单不含自身 hash，整体身份留在 out/m9/batch-b 的本地回执。
当前 publicationStatus 仍为 BLOCKED：候选包、最终 Demo 证据、匿名可达性和发布授权尚未完成。
后续只能在确认的文件范围内刷新哈希；新增文件必须重新审阅，不能自动将工作树整体批准。
