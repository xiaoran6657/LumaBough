# 公开性能证据包（去标识、可复算）

状态：**已生成并通过端到端复算**。这是 lb-current-004 冻结数据的去标识副本，用来让公众在不访问任何
私有数据的情况下复算结论；它**不是**新的性能测量，也不代表运行包 v8 的性能。

## 身份

| 项 | 值 |
| --- | --- |
| 生成工具 | `tools/performance/make_public_performance_evidence.py`（公开集合内） |
| 输入 | `out/performance/lb-current-004`（冻结原始数据，只读；不覆盖、不改写） |
| 输出 | `out/e5/perf-evidence/lumabough-performance-evidence-v1/`（56 个文件） |
| 交付 ZIP | `lumabough-performance-evidence-v1.zip`，1,082,886 B，SHA-256 `c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5` |
| 外部校验 | `lumabough-performance-evidence-v1-external.txt`（含 ZIP 自身哈希） |
| 分析器哈希绑定 | 预注册里的 `analyzerSha256` 与包内 `analyzer/summarize_portfolio_experiment.py` 一致 |
| 原始数据保留 | 冻结目录、源码快照、构建日志、截图、Capture 全部留在本地，不进包 |

## 内容与脱敏

- 保留：`PREREGISTRATION.json`（冻结协议）、`formal/summary.json`（组记录）、25 × `formal/<runId>/run.json`
  （含**每次 600 帧的逐帧样本**与统计、判定字段）、25 × `run-notes.json`、公开分析器副本。
- 删除或置空：机器清单绝对路径、EXE 绝对路径、主机名、适配器 LUID；`formal/summary.json` 的 `command`
  数组把绝对路径压成"从 `out/` 起的相对路径"或 `<abs>/文件名`。
- `SANITIZATION.json` 记录每个被处理文件的 `originalSha256` → `publicSha256` 与 `fieldsRemoved`，
  可与冻结记录逐项对账；未被列出的文件与原件逐字节相同。
- 隐私扫描（E3 扫描器三目标之一）：证据包 ZIP **0 项待审**（脱敏后不再含绝对路径）。

## 复算验证（本机实跑）

~~~powershell
cd out/e5/perf-evidence/lumabough-performance-evidence-v1
python -B analyzer/summarize_portfolio_experiment.py --input . --output ../recompute.json
~~~

结果：`validation=PASS_LOCAL`，四个比较与冻结 `ANALYSIS.json` **逐项一致**：

| 比较 | 变化 | 判定 |
| --- | --- | --- |
| packet-serial → packet-parallel | −16.90% | INCONCLUSIVE（候选 cell 漂移 +3.25% 超 3% 噪声阈值） |
| stream-sync → stream-async | +1.71% | REJECTED（CPU 改善不足） |
| stream-async → stream-parallel | −0.03% | REJECTED（CPU 改善不足） |
| stream-sync → stream-parallel | +1.68% | REJECTED（CPU 改善不足） |

即 **1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED**；分析器同时校验了运行顺序、控制变量不漂移、前台门、
GPU 有效样本数与"存储统计 = 逐帧重算"。

## 限制

- 单机、五 cell、每 cell 五次、D3D12、VSync off；不是跨机器或通用加速结论。
- 逐帧样本支持分布与尾部指标复算；构建与运行环境的原始文件只在本地保留。
- 历史 C-M9-002 的身份缺口与 BLOCKED 状态不因本包改变。
