# 公开证据入口

本次新增展示以[软阴影验收](SHADOW-LAB-REVIEW.md)为准，原生 D3D12 离屏支持域及新运行包独立复验。精选图与参数见[实验页](../showcase/shadow-lab/README.md)。下文的 Sandbox 候选门和旧性能结论保留原记录，不用于替代本批验收。

获准范围的 P → D → E → F 已完成，当前发布状态以[发布记录](PUBLICATION-RECORD.md)为准。
[产物索引与下载](ARTIFACT-INDEX.md)提供运行包、双后端视频、性能证据包和校验表。
当前受测范围见[候选裁定](CANDIDATE-REVIEW.md)，原七条[声明 Ledger](CLAIM-LEDGER.csv)保留历史限制，不承担全部当前交付状态。

## 性能与复现

[性能报告](../portfolio/PERFORMANCE.md) · [新实验完整记录](NEW-PERFORMANCE.md) · [公开逐帧输入与分析器](PERFORMANCE-EVIDENCE-PACK.md)。

001/002/003 INVALID；004 有效组 25/25，1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED。
数值下降不能冒称接受的优化，历史 C-M9-002 的源码身份仍 BLOCKED。
性能实验与发布包 v8 的 EXE 不同，不能互换性能结论。

## Demo Capture 与运行包

[D 记录](BATCH-D.md)：双后端 anchor CLI/GUI 核对、成片均已完成；视频随 Release 公开，原始 Capture/PDB 留本地。
[E 记录](BATCH-E.md)、[E4 六项运行](E4-RESULTS.json)、[隐私终审](PRIVACY-REVIEW.md)与[处置记录](PRIVACY-DISPOSITIONS.json)支持候选 v8。
[F 记录](PUBLICATION-RECORD.md)包含匿名下载及新解压运行；真实读者走查由所有者决定延后。

entry 检查逐文件集合与文档链接；candidate 已实现，额外检查包字节、构建绑定、隐私与 E4。
工具的 publication=BLOCKED 是本地 schema 边界，不是远端仍私有；没有 published 阶段，不能把工具输出当作远端发布验证。
发布前后审批顺序按 F 原记录保留，本轮文档维护不倒填审批。

## 来源与早期记录

[来源](PROVENANCE.md) · [公开清单](PUBLICATION-FILES.json) · [目录映射](PATH-MAP.json) ·
[依赖](DEPENDENCY-GAPS.md) · [许可](LICENSING-REVIEW.md) · [历史勘正](HISTORICAL-CLAIMS.md)。

[A 导入](IMPORT-REVIEW.md)、[B 构建](BATCH-B.md)、[C 数值复核](BATCH-C.md)及
[早期证据清单](EVIDENCE-MANIFEST.json)保留各自取证时点，不冒充最终构建。
[FAQ](../portfolio/FAQ.md)解释目标、贡献和已知限制；[任务入口](../tasks/README.md)说明后续维护边界。
