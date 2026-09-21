# LumaBough 仓库指南

后续串行工作先读 [任务交接入口](docs/tasks/README.md) 和当前阶段任务书。只在 LumaBough 实施，不并行开发，不要求指定 agent 平台、模型或私有 skill。

再读 [C 批次记录](docs/evidence/BATCH-C.md)、[批次 B](docs/evidence/BATCH-B.md)、[来源说明](docs/evidence/PROVENANCE.md)、
[依赖缺口](docs/evidence/DEPENDENCY-GAPS.md)和[许可预审](docs/evidence/LICENSING-REVIEW.md)。
按用户实际指派的批次工作；文档中的后续计划不等于执行授权。

这是独立公开工程的本地候选，内部 namespace、targets 和可执行文件仍用 MiniEngine。
Core 只含 D3D11 / D3D12；Vulkan 未实施。历史源库验收不能替代本仓库复验。
所有者选择 MIT；第三方文件保留原许可。

engine 放可复用实现，samples 放入口，shaders 放 HLSL，tools 放离线工具，tests 放测试。
C++20、四空格、Allman braces、UTF-8。面向所有者的 docs 用简体中文。
文档改动运行相关文档检查；运行实现改动运行适当测试。不能默默移除失败测试。

公开集合由 docs/evidence/PUBLICATION-FILES.json 逐文件管理，.gitignore 只是辅助。
新增或修改内容必须重新分类和更新 hash。pending / excluded 不能提交到公开源码集合。
不要自动批准扫描报告中的条目。不要复制旧 Git 历史、内部工作流或私人记录。
许可、隐私和依赖门未通过前不能公开。commit / push / 发布以用户本轮授权为准。

最新实际状态见 [新性能实验记录](docs/evidence/NEW-PERFORMANCE.md)：001/002/003 均 INVALID，
lb-current-004 取得有效组（1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED），**不宣称当前加速**；
禁止拼接或覆盖旧正式组。C 历史身份缺口保持原样。
（本段为当前结论；文件中其它位置若出现"未启动第三组"等表述，属当时快照，见
[批次记录](docs/evidence/BATCH-E.md) 与 [候选裁定](docs/evidence/CANDIDATE-REVIEW.md)。）
