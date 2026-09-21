# 产物索引

| artifact ID | 本地候选公共路径 | 类型 / 状态 |
|---|---|---|
| historical-claims-review | [HISTORICAL-CLAIMS.md](HISTORICAL-CLAIMS.md) | 来源摘要与勘正；LOCAL，不能代替原始证据 |
| batch-b-entry-review | [BATCH-B.md](BATCH-B.md) | 本轮目录、构建与测试摘要；LOCAL |
| batch-b-render-preview | [RENDER-PREVIEW.json](RENDER-PREVIEW.json) | 真实固定帧与受测代码快照；LOCAL |

各记录的字节数与 SHA-256 见 [EVIDENCE-MANIFEST.json](EVIDENCE-MANIFEST.json)。
逐文件代码来源见 [PUBLICATION-FILES.json](PUBLICATION-FILES.json)。
旧 raw topic 当前没有公共 URL；本仓库未配置远端，也未上传任何附件。

公开复现还缺：parity / validation / soak 原报告及完整输入，
METHOD-003 完整 A/B raw 和必要源码身份，COMBINED-001 负结果，
SOAK-RSS-001/002 与 LOAD-RECORDS-001 修正链，资产故障契约证据。
最终 Demo anchor 的 PIX/RenderDoc 属新运行，必须另采，不能改写成旧 artifact。
所有缺口关联 Claim Ledger，未补齐前保持 BLOCKED。

## C 批次本地证据

[连续彩排](DEMO-REHEARSAL.json) · [性能摘要](PERFORMANCE-SUMMARY.json) · [完整统计输入变换](performance/TRANSFORM-MANIFEST.json)
