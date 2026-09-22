# T1 发布后文档收口与性能复算

日期：2026-09-22。基线 HEAD：1108c7b082eccf6189afe3f0be3c01ceeddfee17。
范围：当前导航、静态性能说明、FAQ、阶段状态与逐文件清单；不改引擎、运行包、视频或冻结实验。

## 修改与边界

同步 README、作品集/证据入口、产物索引和 P/D/E/F 状态；补 FAQ 的目标、贡献、故障案例、复现与限制。
004 有效组不等于优化被接受：1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED。
历史 C-M9-002 保持 BLOCKED；C-M9-007 改指 D 记录，明确本地 Capture 完成与原始 Capture 未公开的区别。
旧 B/C 取证正文和早期证据清单保留，不改历史运行身份或审批时间。

## 公开输入与本轮实际复算

匿名下载 Release 的 lumabough-performance-evidence-v1.zip（无认证头、无 Cookie）。
Python HTTPS 首次遇到 TLS EOF，改用系统 Invoke-WebRequest 成功；未关闭证书校验或修改全局代理。
下载 1,082,886 B，SHA-256 c33c5b39d05b77b5587acdfe85b9131e377aeb48828c61ebe21acc0c10489ff5，与发布记录一致。
新目录解压 56 文件，使用包内分析器执行：

~~~powershell
python -B analyzer/summarize_portfolio_experiment.py --input . --output ../recompute.json
~~~

退出 0，validation=PASS_LOCAL，runCount=25，measuredSamples=15000。
与本地冻结 ANALYSIS.json 的 validation、runCount、measuredSamples、comparisons、runs、experimentIds 逐项相等；
包括四项比较的完整数值、漂移、受保护指标、判定与逐运行统计。脱敏 raw 文件哈希自然不同，不要求与原始 raw 字节相同。
本轮没有新 GPU 采样，也没有把 004 结论改绑到运行包 v8。

## 门禁

首次 entry：PASS（774 受审文件）；首次扫描新增两个唯一处置键，已列 proposed，等待所有者明确确认。
所有者随后明确批准上述两项保留，按实际时间登记；候选门复跑退出 0，775 受审文件、原包字节、隐私门和 E4 均通过。
暂存集合隐私扫描 150 项命中，0 未裁定、0 待批准；62 个唯一处置键。匿名下载性能 ZIP 另行扫描为 0 命中。
候选门固定输出 publication=BLOCKED 仅表示它不验证远端发布，不改变既有 F 事实。

## 证据位置

本地 out/t1-docs-2026-09-22 保存下载原件、解压输入、recompute.json、文件提案与扫描结果。
下载及复算入口见[公开性能证据包](PERFORMANCE-EVIDENCE-PACK.md)，包身份见[发布记录](PUBLICATION-RECORD.md)。
真实读者走查仍延后，不用所有者本人检查或 Agent 自查替代。
