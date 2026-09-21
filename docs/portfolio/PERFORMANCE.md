# 历史性能重算与适用边界

25 次运行的 15,000 个逐帧样本已重新计算分布，且与原统计一致。固定历史比较器复现原裁定。**这是历史数值复核，不是本仓库当前构建的性能重测，也尚不能称完整历史构建复现。**

| 案例 / 比较 | cpuFrameMs 前 → 后（ms） | 变化 | 历史规则裁定 |
|---|---:|---:|---|
| METHOD-003：serial w1 → parallel per-worker w8/c256 | 65.7946 → 54.9751 | −16.444% | ACCEPTED |
| COMBINED-001：sync serial → async serial | 33.5980 → 34.1570 | +1.664% | REJECTED，p95 退化 |
| COMBINED-001：async serial → async parallel | 34.1570 → 34.6466 | +1.433% | REJECTED，未超过噪声下限 |
| COMBINED-001：sync serial → async parallel | 33.5980 → 34.6466 | +3.121% | REJECTED，p95 +6.567% |

主案例限定 m7-cpu-scale / D3D12 / packet 成本较高的配置；负例限定 m7-streaming。不能把 −16.44% 外推为通用引擎加速。每组保留第一批 3 次和追加批 2 次，未选取最好运行；每次 warmup=120、measure=600、seed=6657、1920×1080。METHOD 共 10 次，COMBINED 共 15 次。旧 driver 的 RP-001/002 标签保留，实验归属以 run.experimentId 和协议清单为准。

## 重算入口与统计口径

~~~powershell
python tools/portfolio/recompute_performance.py --output out/performance-recomputed
~~~

需要 Python 和 Windows PowerShell 5.1。默认读取[全部数值输入](../evidence/performance/TRANSFORM-MANIFEST.json)，不需要访问私有仓库。工具先由 samples 重建每个 distribution 的 median、MAD、线性插值 p95/p99/maximum 和 hitch 计数，再调用固定历史规则。运行之间取各 run median 的 median；噪声下限 max(3%, 2×基线 run 间 relativeMAD)，受保护指标阈值 5%，保留原精度下限规则。allocationCount 被声明 unavailable，不能将占位零解释为零分配。GPU 延迟查询有未读回样本，重算保留原始零占位口径，不与另一次仅过滤有效样本的统计混用。

protocol.json 从历史比较输出和完整 run 集合重建，未冒充原始预注册文档；私有预注册提交只作来源索引，不能替代公开原件。

固定比较器取自两个实验共用版本，原文件 SHA-256 为 6edd5e3c01ed35b62bb1dd0e109f7d98d8321f1354e5733d29ca404e23689b2e；公开副本只增加本机 PowerShell 模块加载、修改一处来源注释，未改统计算法。当前比较器后续新增规则不偷偷覆盖历史裁定。

历史顺序索引的 cell 键与 run 目录名不匹配，且未汇总 batch2 的 driver，因此历史漂移可能退回 runIndex。补充核算按两个 driver 的真实 startedUtc 排序，METHOD 基线 −0.102%、候选 +2.737%；COMBINED 三个 cell 为 −1.140%、−2.702%、−1.672%，均未越过 3%。这次发现不改变结论，但不能把旧顺序算法说成正确。

[机器配置](../evidence/performance/machine.json)保留 CPU/GPU/驱动/OS/内存/电源计划；原 manifest 的桌面尺寸是 2194×1234，运行尺寸为 1920×1080，两者不是同一字段。原编译器版本/温度信息为空，保持缺失。主机名、adapter LUID 和绝对路径已脱敏；[变换清单](../evidence/performance/TRANSFORM-MANIFEST.json)绑定原文件与公开文件哈希。截图、RGBA 和诊断日志不参与数值重算，本批未导入；无关 METHOD-002 rejudge 明确排除。

## 源码身份为什么仍阻断

所有 25 个 raw 记录同一 EXE SHA-256 c39e76f9d46f18993421fbaed30203a7630ebbd8c9ca24d910b2cc96ee6e567c，以及配置时写入的 sourceCommit=0c7a1a3b9c6f442148ed703fb4fa9282537b6d67。但在实验前的提交链中，dbef1fb 已修改 RenderPacketBuilder.cpp、TaskSystem.h 和 TaskSystem.cpp；CMake 的该字段只在配置时更新。此前把预注册提交 2429dc1 当作实际运行源码提交也没有证据支持。

因此实际 sourceCommit 保持 null，单列 embeddedSourceCommit、预注册和报告身份；不选一个看似合理的提交代填。当前尚缺匹配历史 EXE 的完整源码快照/可验证构建绑定，C-M9-002 保持 BLOCKED / LOCAL。数值复算可用，并不补足源码身份。下一步推荐在 LumaBough 冻结一次明确源码与二进制的新实验，产生独立实验 ID；旧结果继续保留为历史复核，不能用新数据倒填旧实验。若以后找到匹配的历史构建清单或源码校验和，也可单独闭合旧身份。

[机器可读摘要](../evidence/PERFORMANCE-SUMMARY.json)是数据入口。旧 10k soak 只支持池指标口径，不能证明零泄漏；修复后仅有 2k 证据的限制见[勘正](../evidence/HISTORICAL-CLAIMS.md)。C 批次未把它升级为新稳定性结论。

## 当前源码新实验状态

已另行实施两组独立实验，均因正式采样的前台覆盖不足而 INVALID，未生成新的加速结论。源码与新构建已冻结，失败数据完整保留；见[新实验记录](../evidence/NEW-PERFORMANCE.md)。本文历史数值及其身份阻断保持原样。
