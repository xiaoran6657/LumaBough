# 公开证据入口

B 的目录、许可、依赖和本地构建验证见 [批次 B](BATCH-B.md)。C 与当前性能尝试见下方记录；后续按 [P → D → E → F](../tasks/README.md) 串行推进。

- [A 批导入历史记录](IMPORT-REVIEW.md)
- [来源与身份](PROVENANCE.md)
- [逐文件允许集合](PUBLICATION-FILES.json)
- [目录迁移映射](PATH-MAP.json)
- [依赖处理](DEPENDENCY-GAPS.md)
- [许可复核](LICENSING-REVIEW.md)
- [范围与产物](EVIDENCE-MANIFEST.json)
- [声明 Ledger](CLAIM-LEDGER.csv)
- [历史声明勘正](HISTORICAL-CLAIMS.md)
- [产物索引](ARTIFACT-INDEX.md)

entry 只证明当前本地清单、链接与验证范围。candidate、published 未实现，最终运行提交、分发包、公开 URL 与发布授权仍须另行完成。

## C 批次

[批次记录](BATCH-C.md) · [Demo](../portfolio/DEMO.md) · [历史性能重算](../portfolio/PERFORMANCE.md)

## 当前源码新性能实验

[尝试与结论记录](NEW-PERFORMANCE.md) · [逐运行审计](NEW-PERFORMANCE-ATTEMPTS.json)。
001/002/003 因前台覆盖不足 INVALID；lb-current-004 取得完整有效组（25/25），结论为 1 个 INCONCLUSIVE
（packet 并行中位 -16.9%，但漂移与尾部 hitch 未过）＋ 3 个 REJECTED（streaming 三配置），**没有 ACCEPTED**。
历史 C-M9-002 与历史性能摘要仍 BLOCKED。

## D 批次（最终 anchor Capture / 动态视频）

[批次记录](BATCH-D.md)：双后端 anchor Capture 已采集并做无 GUI 结构核对；视频因本机没有可用录屏工具 BLOCKED。Capture、原片与 PDB 均留在本地，公开前单独审查。
- D 阶段收口（2026-09-21）：双后端 anchor Capture 的 CLI＋GUI 复核通过，双后端成片已产出；证据见 [批次记录](BATCH-D.md)，大文件留本地。

## E 批次（候选包、隐私终审、独立运行）

[批次记录](BATCH-E.md) · [隐私与内容终审](PRIVACY-REVIEW.md) · [逐项处置基线](PRIVACY-DISPOSITIONS.json)。
[候选裁定](CANDIDATE-REVIEW.md) · [F 发布提案草案](F-PROPOSAL.md)。E1/E2 完成（候选包 v7：157 文件 /
10,092,318 B，交付 ZIP 3,464,797 B，包内 manifest 与 SHA256SUMS）；E3 通过（49 键全部批准，扫描器退出 0）；
E4 第二台机器全部通过（跨机哈希一致），真实读者由所有者决定延后 → 公开时必须缩减承诺；
E5 候选门 `--stage candidate` 已实现并通过；未创建 remote、未推送。
