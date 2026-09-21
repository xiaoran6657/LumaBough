# 路线图

Core 固定为 D3D11 + D3D12；Vulkan 不在范围。唯一工作仓库为 LumaBough。
后续按 **P → D → E → F** 串行实施：同一时刻只有一个实施阶段，不设并行 agent、独立性能仓库或额外开发仓库。
本次只修订计划；阶段文档不是执行、提交、推送或公开授权。新 agent 从[交接入口](tasks/README.md)开始。

| 阶段 | 范围 | 当前状态 / 任务入口 |
|---|---|---|
| A | 独立导入、公开边界、声明勘正、MIT | 已实施：[历史记录](evidence/IMPORT-REVIEW.md) |
| B | 目录、依赖、公开入口与架构、构建回归 | 已完成：[验证记录](evidence/BATCH-B.md) |
| C | Demo / rehearsal；历史性能重算 | Demo/数值通过，历史源码绑定阻断：[记录](evidence/BATCH-C.md) |
| P | 焦点诊断、补充性能实验、独立结论 | 已完成：有效组（lb-current-004，25/25）= 1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED；001/002/003 INVALID；静态报告写回等 F：[记录](evidence/NEW-PERFORMANCE.md) · [任务](tasks/P-performance.md) |
| D | 最终 anchor Capture；聚焦动态内容的视频 | Capture 完成、已完成（Capture 复核 + 双后端成片）：[记录](evidence/BATCH-D.md) · [任务](tasks/D-capture-video.md) |
| E | 候选包、许可隐私终审、独立运行与真实读者审阅 | 候选通过（范围缩减：读者延后）：[裁定](evidence/CANDIDATE-REVIEW.md) · [记录](evidence/BATCH-E.md) · [任务](tasks/E-candidate-review.md) |
| F | 授权发布、匿名访问与下载核验 | 执行中：先按授权建私有远端并发布 `v0.1.0-preview`（预发布），转 public 仍需单独授权：[提案](evidence/F-PROPOSAL.md) · [任务](tasks/F-publication.md) |

P 收口可以是有效的正结果、负结果或不确定结果；INVALID 不算有效比较。
本轮 P 已给出有效组的负/不确定结论：没有 ACCEPTED 比较，因此**仍不宣称当前加速**；
历史 C-M9-002 与历史性能摘要保持 BLOCKED。
如始终不能取得有效组，必须由所有者明确同意以“不宣称当前加速、保留阻断”的缩减范围进入 D；不得由 agent 自动豁免。
历史 C-M9-002 的身份缺口不因新实验消失，也不要求伪造历史身份才能发布；E/F 须核对最终对外声明与保留限制一致。

视频只展示需要连续过程理解的内容。定位、目标、做了什么/没做什么、架构总图、性能统计、收获和入口放静态文档及图片。
当前 Demo 仅证明程序化 RHI 尺寸/恢复和 shader 候选处理，不冒称文件监听热更新、自由相机或新 shader 效果。

各阶段结束更新任务状态、证据、逐文件候选清单和本页；不覆盖历史失败记录。
后续修改测量路径、Demo、shader、构建或运行资源，先评估哪些已完成证据失效，再串行复验受影响阶段。
提交与图/命令记账成本等进一步优化是独立研究，不在本计划顺带实施。
