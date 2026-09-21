# 历史声明复核

以下是来源记录的审阅摘要，不是 LumaBough 的重新运行结果。
C 批次已导入 METHOD/COMBINED 完整统计样本并重算；历史 EXE 对应的完整源码仍未证明，因此公开 Ledger 不继承历史 PASS。详见[性能报告](../portfolio/PERFORMANCE.md)。
本地摘要可读（LOCAL）也不等于完整证据已经匿名可验证（VERIFIED）。

| ID | 历史观察 | 公开版处置 |
|---|---|---|
| C-M9-001 | 三个 M7 场景双后端语义对照，固定帧像素 MAE 0.1630/255，31/31 检查 | 只在相同场景/参数范围引用；需导入经审查报告、输入和 raw，或本库复验 |
| C-M9-002 | METHOD-003：m7-cpu-scale，serial w1 对 parallel per-worker w8/c256；cpuFrameMs 65.7946→54.9751 ms，−16.44%，历史判定 ACCEPTED | 限 packet-bound 场景；该百分比是历史帧级统计，不是所有场景加速 |
| C-M9-003 | Vulkan 未实施 | Extended / N/A，不进入支持后端或已实现能力 |
| C-M9-004 | 四组 Debug/validation 运行，历史 8/8 检查通过 | 历史受测配置有效，不等于本仓库当前运行复验 |
| C-M9-005 | 旧 10,000 帧 d3d12 soak 中 residentBytes 为 124,981,248 B 恒定 | 收窄为瞬态池口径，撤回“0 泄漏”；修复链详见下文 |
| C-M9-006 | revision 资产原子提交，10 类故障保留旧 Ready，原记录 27 项契约测试 | 历史测试数量不能当作当前测试总数；须导入具体证据或当前重跑 |
| C-M9-007 | 最终 portfolio-capture 的 PIX / RenderDoc 尚未采集 | 保持 BLOCKED；旧场景 capture 不能替代最终 anchor |

## C-M9-005 的必要勘正

旧 soak 的 21,978 次热重载请求、16 次零尺寸挂起和池内存恒定，
不能推出进程整体不增长。后续 SOAK-RSS-001 的 10k 运行报告工作集
173.8→672.8 MiB（+499.0 MiB），进程无增长假设为 REJECTED，池口径仍恒定。
该 topic 的仪表 smoke 比预注册载荷早 15 秒；原记录披露此顺序偏离，
10k 正式 run 本身晚于载荷。公开案例也要保留这项披露。

SOAK-RSS-002 归因到热重载 churn；LOAD-RECORDS-001 修复请求记录保留，
**2000 帧**热重载组工作集增量由 +89.4 MiB 降至 +2.6 MiB，
recordsReleased=1056（等于 reloadCommits），evicted=0。
这是有条件的修复验证，**没有修复后 10k 复验的含义**。
LiveRequestCount 指在途请求，不是终态保留记录数量；不能用旧 65 条在途数证明保留。

新 C-M9-005 仅描述历史池指标及上述限制。要宣称当前稳定性，须在本仓库最终运行版本
明确采样口径、稳定窗口和时长后重新验收；不得把短运行修复验证改写成长期无泄漏保证。

## 性能反例必须随主案例披露

COMBINED-001 的 streaming 三组比较全部 REJECTED：
组合相对基线 cpuFrameMs +3.12%、p95 +6.57%。该场景 packet 段约 0.3%，
所以 METHOD-003 的收益不能外推到它。公开性能报告保留此负结果和适用范围。
M8 已跳过；提交记账成本作为独立后续课题，不成为本轮新增 Vulkan 的借口。

## 缺失证据如何补齐

按 Manifest 中的历史 artifact 标识提取完整协议内 run、预注册、比较器版本、报告、
必要代码快照/差异与输入身份。保留失效/排除 run 说明，脱敏后重算 hash 与统计。
只有原报告、私有 commit 或这个摘要时，不称公众可复现。
