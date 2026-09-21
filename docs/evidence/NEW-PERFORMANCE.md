# 新性能实验：四次尝试、有效组与独立结论

日期：2026-09-21。范围：LumaBough 本地当前源码。
**当前状态：lb-current-004 取得完整有效组（25/25 通过），结论为 1 个 INCONCLUSIVE + 3 个 REJECTED，没有 ACCEPTED。**
历史声明 C-M9-002 与历史 PERFORMANCE-SUMMARY 仍 **BLOCKED**，本结果不复原、不倒填、不代表历史身份修复。
未提交、未推送、未公开。

## 实际结果

| 尝试 | 预演 | 正式运行 | 失败条件 | 整组裁定 |
|---|---|---|---|---|
| lb-current-001 | 五配置的 debug-layer 与计时预演均通过 | 17 次通过，第 18 次 stream-sync-r4 失败 | 前台 0/600 帧（0%） | INVALID |
| lb-current-002 | 五配置的 debug-layer 与计时预演均通过 | 21 次通过，第 22 次 packet-serial-r5 失败 | 前台 509/600 帧（84.8333%），低于 90% | INVALID |
| lb-current-003 | 五配置的 debug-layer 与计时预演均通过 | 8 次通过，第 9 次 stream-sync-r2 失败 | 采样前前台就绪 TIMEOUT（等满 60 秒未拿到连续 1 秒前台），进程退出码 2 | INVALID |
| lb-current-004 | 五配置的 debug-layer 与计时预演均通过 | **25 次全部通过** | 无 | **有效组** |

三次失败运行（001/002/003）都完成了 600 帧或停在采样前，GPU 有效查询 597/600，validationMessages=0，
最终 liveResources/aliveObjects/retiringObjects=0；这些观测不能扩张为跨进程零泄漏结论。
前三次的失焦都没有记录到夺取者窗口或进程，因此**不能归因**给用户操作、通知、agent 或某个系统程序。

预注册规定不自动重试、不替换失败 run、不拼接部分区组；任何一次正式失败使整组 INVALID。
没有对任何不完整集合计算加速百分比，没有把任何负结果删掉。
[机器可读审计](NEW-PERFORMANCE-ATTEMPTS.json)记录每次尝试、运行及原始文件哈希。

## lb-current-003（2026-09-21，整组 INVALID）

准备：新实验 ID `E-LB-PACKET-003` / `E-LB-STREAMING-003`、新预注册
（SHA-256 `beeacda2ed631657ee38e644e7c63437fb707cacc785f87435f33f7dcb316aa3`）、
按逐文件清单冻结源码、复用已校验依赖源码后全新构建、重新烘焙；采集器实验 ID 绑定与
第一次注册的缺陷（experimentIds 仍写 002）已作废重注册，未复用任何运行。

过程：10 次预演全部通过；正式采样前 8 次全部 `foregroundRatio=1.0`、`gpuFrameSamples=597`；
第 9 次 `09-stream-sync-r2` 的 `foreground-readiness.json` 为
`{"status":"TIMEOUT","waitMs":60008,...}` ——**采样开始前始终没有拿到连续 1 秒前台**，进程退出码 2。
按预注册（`retries=0`、失败即停、不替换不拼接）整组 INVALID；分析器确认拒绝
（`formal group or preregistration failed`），没有生成任何比较或加速百分比。
8 次成功运行的数据保留在本地，不补成新实验。逐项记录见
out/performance/lb-current-003/INVALID-RESULT.json。

失败模式三次都不一样：001 是测量中完全失去前台（比例 0）；002 是测量中部分失去（0.848333）；
003 是从未获得前台（就绪 TIMEOUT）。三次都没有记录到夺取者窗口或进程，
因此**不能归因**给用户操作、通知、agent 或某个系统程序；只能确认"会话状态与前台可用性不稳定"。

## 独立结论（当前源码，lb-current-004 完整有效组）

预注册 SHA-256 `45778477c770a542df9517a809e15dea50dceb07d47a305dc55ec66b9322ecdb`；
EXE `325e9e43cb2e2d6e3ff3aaa1176d434b55a94909cccfc5d22610c2496d2144ad`；
25 次运行全部 PASS（每 cell 5 次、五轮完整区组、1920×1080、VSync off、warmup 120 + measure 600、
前台比例 1.0、GPU 有效查询 597/600、validationMessages=0）。
主指标：每 run 的 `cpuFrameMs` median → 取五个 run median 的 median；噪声阈值 max(3%, 2×基线 MAD)。

| 比较 | 基线 → 候选（ms） | 变化 | 漂移 | 受保护指标 | 裁定 |
|---|---|---|---|---|---|
| packet-serial → packet-parallel（50,000 代理，w8/c256） | 70.158 → 58.299 | **-16.90%** | 候选 **+3.25%** 超出 3% 噪声阈值 | p95 -18.68% 通过；GPU -0.40% 通过；residentBytes 不变；**尾部 hitch 未通过**（候选 3.37% > 允许 1.03%） | **INCONCLUSIVE**（先判漂移） |
| stream-sync → stream-async（500 代理） | 35.168 → 35.769 | +1.71% | 均在阈值内 | 全部通过 | REJECTED（CPU 改善不足） |
| stream-async → stream-parallel | 35.769 → 35.759 | -0.03% | 均在阈值内 | 全部通过 | REJECTED（CPU 改善不足） |
| stream-sync → stream-parallel | 35.168 → 35.759 | +1.68% | 均在阈值内 | 全部通过 | REJECTED（CPU 改善不足） |

去标识复算材料：[NEW-PERFORMANCE-RUNS.json](NEW-PERFORMANCE-RUNS.json)（25 次运行的逐 run 分布、CPU 阶段分解、
受保护指标、逐文件哈希与四条比较的判定；不含机器路径、用户名与原始逐帧数组）。
逐帧中位数本身的复核需要本地原始文件，这一限制写在材料里。

**结论（独立、只对本机本构建）**：
1. 50,000 代理的 packet 构建：并行的中位 CPU 帧时间比串行低约 16.9%，
   但按预注册的判定顺序，候选 cell 的时间漂移（+3.25%）超过噪声阈值，先判 **INCONCLUSIVE**；
   同时尾部 hitch 受保护指标也未通过（3.37% vs 上限 1.03%）。因此**不能声明可接受的加速**。
2. 500 代理的 streaming 三配置：两两比较全部 **REJECTED**，即异步/并行上传在本设置下没有可分辨的
   端到端 CPU 收益（变化在 -0.03% ~ +1.71%）。
3. 没有任何比较得到 ACCEPTED；没有把 INCONCLUSIVE 当作正结果，也没有把它改名为有效比较。
4. 本结果只绑定本次冻结源码、构建与环境（单一机器、每配置 5 次、无温度传感器记录），
   不是跨机器结论，也不代表通用加速。历史 C-M9-002 的身份缺口与 BLOCKED 状态不因此改变。

## 采样条件：前台可获取性（对本结果适用性的前提）

有效组是在"驱动进程本身由前台可见控制台启动"的条件下取得的；此前三次失败与两次准备中断都发生在
其它启动/会话状态（无人值守会话、agent IDE 占据前台、以及 `DETACHED_PROCESS`/隐藏窗口启动链）。
因此：**任何代码/构建/启动方式改动后想复用本结果，必须先核对前台可获取性条件是否相同**，
并重新走新实验 ID、新预注册与完整区组；不得把本结果直接套到新的执行路径上。

## 前台夺取者与启动链（第一次拿到具体证据，并据此取得有效组）

1. **前台被占**：003 第 9 次失败后立即查询前台归属 = `LumaBough - CodeBuddy CN`
   （agent 所在 IDE 的进程，会话 1）。M7 采样进程按设计"不强抢焦点"，只等连续 1 秒前台，
   被占满 60 秒就 TIMEOUT。这解释了 003 第 9 次与 004 前两次预演的失败形态。
2. **启动链也会决定成败**：把 IDE 最小化后，直接用 shell 跑一次短 benchmark 可以拿到 100% 前台，
   但用 `DETACHED_PROCESS + CREATE_NO_WINDOW` 启动驱动时，孙进程的采样窗口仍然拿不到前台（连续三次 TIMEOUT）；
   去掉 `DETACHED_PROCESS` 也不行。**只有由前台可见控制台启动驱动**时，25 次采样才全部拿到前台。
3. 因此本轮的有效组是在"IDE 最小化 + 驱动由前台可见控制台启动"的组合条件下取得的；
   这只解释**可观测**的这几次，001/002 的夺取者当时没有记录，仍然不能归因。
4. 该条件属于运行环境，不是对测量口径的修改：0.9 门槛、warmup/measure 帧数、cell 定义、
   区组顺序、受保护指标都没有改，也没有强抢焦点、暂停计时或剔除失焦帧。

## 2026-09-21 焦点诊断（补充，不是性能结论）

诊断只回答"前台能不能保持、什么时候丢、被谁夺走"，不产生任何性能数字或加速结论。

**会话与环境事实**：`query user` 显示 console 会话处于 Active 且 IDLE TIME 为 none（非锁屏、非断开）；
屏保关闭（`ScreenSaveActive=0`）。这与 001/002 记录的"无人值守、无活动桌面、GetForegroundWindow 命中率 0"
不是同一种会话状态——**焦点条件与会话状态强相关**。

**前台保持探针**（本地探针窗口，50 ms 采样，120 秒）：2375/2375 采样保持前台（100%），
0 次丢失，1 次变化（获取瞬间），`SetForegroundWindow` 被允许，没有记录到夺取者。
结果：out/performance/lb-focus-diagnosis-2026-09-21/focus-probe-120s.json。

**真实负载前台门**：三次 M7 benchmark（`--foreground-gate=on`，1920×1080、warmup 120、measure 600）
`foregroundRatio` 均为 600/600 = 1.0，`validationMessages=0`，`gpuFrameSamples=597`。
三次是诊断运行（experiment-id `E-LB-FOCUS-DIAGNOSIS`），**不是正式组，不作为性能结论**，
也不进入任何比较或中位数统计。

**仍然没有解决的部分**：
- 001/002 两次失败的夺取者当时没有记录（窗口/进程/PID 均未落盘），现在无法回溯归因；
  本报告不能把它归因于用户操作、通知或某个系统程序。
- 历史失败率约 1/18 与 1/22（单次约 5%）；三次诊断通过不能证明 25 次连续通过。

**继续做正式组的前提**：
1. 所有者在运行窗口内不使用这台机器，并关闭或最小化其它可见窗口（当前会话里有三个 IDE 窗口与资源管理器）；
2. 运行期间不并发构建、录制或采集；
3. 第三组须新实验 ID、新预注册、全新构建与完整区组；单次失败仍使整组 INVALID。

**准备状态**：M7 需要的烘焙场景按哈希恢复（`out/m4-09/scene/manifest.json`
SHA-256 `4ae6eda980221395a80e3bb03374d051a07b8cd197f8a5c0b55097f427ba6260`，与 002 冻结副本逐字节一致）；
机器清单已采集（SHA-256 `d064057ecd4368d6e142ed309a50cd4c317b57c5a287b24536c656677cced738`）。

## 源码与构建身份

两次分别保存独立源码 ZIP、逐文件 SOURCE-SNAPSHOT、全新 Release 构建、EXE/DLL/shader 与烘焙资源身份。
实际编译器为 MSVC 19.51.36252.0，VS2026 v145，Tracy OFF；运行使用 D3D12、非 WARP、无 debug-layer/GBV/Capture。
机器为 Ryzen 5 5600G / Radeon RX 9070。详细环境保留在本地，公开摘要只保留文件哈希。

第一次 fresh build 遇到 fastgltf 的 simdjson.h 零字节下载；重新下载同一 v3.12.3 文件并校验后完成构建。
失败构建与修复记录保留，没有更换依赖版本。第二次只复用逐文件校验过的依赖源码，工程对象和 EXE 全部重新编译。
第二次源码快照基于旧 740 文件允许清单，加上本次显式审阅的实验工具/测试与 M7SceneRunner 修改；
它不是最终交付文档的快照。后续公开清单更新不倒改这两个快照。

本仓库尚无 HEAD，sourceCommit/runtimeCommit/publicationCommit 均保持 null；
源码 ZIP 和逐文件哈希是本次实际绑定方式，不能把内嵌 uncommitted 当成提交身份。
历史 C-M9-002 与历史 PERFORMANCE-SUMMARY 保持 BLOCKED，不因这些新运行而改变。

> 状态更新（2026-09-21，E 批次）：本节以下内容描述 001–003 时期的状态，属**历史快照**。
> 当前结论是 lb-current-004 的有效组（25/25）：1 INCONCLUSIVE + 3 REJECTED，**无 ACCEPTED**；
> 仓库此后已建立本地 HEAD 与提交身份，第三组未启动，采样路径未改动。
> 另：公开复算输入已补齐——[公开性能证据包](PERFORMANCE-EVIDENCE-PACK.md) 提供去标识的逐帧样本
> （25 × 600 帧）、冻结协议、组记录与公开分析器，并已实测复算出同一结论（1 INCONCLUSIVE + 3 REJECTED）。
> 逐 run 汇总见 [NEW-PERFORMANCE-RUNS.json](NEW-PERFORMANCE-RUNS.json)；冻结原始数据仍只留本地。

## 采样前检查修复

只修改 samples/rhi_sandbox/M7SceneRunner.cpp 的 benchmark 前置检查：
当 foreground gate 开启时，在预热前等待连续前台 1 秒，最长 60 秒。
等待期间处理窗口消息；关闭或超时明确失败，写 foreground-readiness.json。
不强抢焦点，不暂停计时帧，不剔除离开前台的帧，不降低原 90% 门槛。
foreground gate 关闭时不走新增等待路径。

真实负例使用隐藏窗口验证：约 60 秒后 TIMEOUT，未生成计时 run.json。
真实正例为第二次全部五配置计时预演及已完成的正式运行。
此修复防止“从未就绪却开始计时”，不能保证之后不被切走焦点。

## 冻结方法

两个独立实验分别比较：
- E-LB-PACKET-001/002：50,000 代理，serial w1 与 per-worker parallel w8/c256。
- E-LB-STREAMING-001/002：500 代理，同步串行、异步串行、异步并行。

每配置五次独立进程，五轮随机完整区组；seed=6657，顺序随机种子=665720260920。
每次 1920×1080、VSync off、warmup=120、measure=600；GPU 有效样本最低 594/600。
主指标是五个 run median 的 median；噪声为 max(3%, 2×基线 run median 的 relativeMAD)。
先检查有效性，再检查按真实 sequence 计算的前后漂移，然后检查 CPU p95、有效 GPU、池 residentBytes 和尾部退化。
GPU 未读回的零占位不当作有效零耗时；allocation 未观测不解释成零分配。
具体固定规则见各本地 PREREGISTRATION.json；两组都在数值比较之前即因有效性失败而终止。

## 验证与本地接续

新 Release 构建和烘焙通过；每次五项 debug-layer 正确性与五项计时预演通过。
工具测试 tests/tools 42 项、tests/tools/contracts 42 项通过（后者含新增 12 项统计契约）。
本次未重跑全部 CTest，不能把 C 批次的 1003 项旧结果说成本次新结果。

原始源码、机器信息、构建日志、图像、二进制与正式数据全部留在：
- out/performance/lb-current-001
- out/performance/lb-current-002

每个目录保存 tools-snapshot、PREREGISTRATION、BUILD-RECORD、BUILD-INPUTS、pilot、formal 与 INVALID-RESULT。
第二次另有 readiness-negative 和 audit_attempts.py；本地 bootstrap.py 记录实际构建方式。
这些文件未进入公开允许集合。公开摘要不包含用户名、主机名或绝对机器路径。

采集入口是 tools/performance/run_portfolio_experiment.py（pilot/register/run），
分析入口是 tools/performance/summarize_portfolio_experiment.py（--input/--output）。
当前采集器绑定 002，两个目录均已封存，不可重用其 formal 路径重跑。
工具要求已经准备好 SOURCE-SNAPSHOT、source.zip、环境、构建与依赖记录；它不替代构建准备脚本。

下一步必须先解决持续前台条件：在独立本地终端由所有者手动启动，使用不弹出进度通知的运行方式，
先做焦点保持诊断并记录前台切换时间/目标 PID（仅本地保存），确认后再决定是否开第三组。
这只是降低干扰、获取根因的建议，尚未证明能修复问题。
第三组须新实验 ID、新预注册和完整区组；若改采样代码须重新冻结、构建、预演。
两组已有数据不能补成新实验。尚未启动第三组，也未改变桌面通知、电源或锁屏设置。
