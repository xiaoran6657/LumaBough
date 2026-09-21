# 变更记录

## 2026-09-21 · E 批次候选包与隐私终审

新增候选运行包打包器（显式输入、确定性 ZIP）、隐私/内容扫描器（三目标、逐项批准基线）、候选门
`validate_publication.py --stage candidate`（包字节 + 构建绑定 + 隐私门 + E4 记录）与 E4 操作手册。
修复跨机硬伤：生成的着色器包头原先内嵌构建机绝对路径，改为按 EXE 目录解析并随包携带字节码，
构建树改名后的隔离测试通过。第二台机器（Windows 11 / RTX 3090 Ti）双后端 1202 帧与 Demo 事件路径全部通过，
跨机哈希一致；真实读者由所有者决定延后，公开承诺随之缩减。未创建 remote、未推送。见
[候选裁定](evidence/CANDIDATE-REVIEW.md) · [批次记录](evidence/BATCH-E.md) · [F 提案草案](evidence/F-PROPOSAL.md)。

## 2026-09-20 · D 批次 anchor Capture 与视频准备

新增连续 Demo 的可读性控制（状态停留帧数、可配置的临时 extent、VSync 跟随）与视频/Capture 协议 recipe；tour 运行的 GPU capture 改为绑定 anchor 帧。双后端 anchor Capture 已采集并做无 GUI 结构核对；视频仍因缺少录屏工具 BLOCKED。P 保持 BLOCKED，本轮不宣称当前加速。见 [D 记录](evidence/BATCH-D.md)。

## 2026-09-20 · 公开工程批次 B

- 将脚本按用途目录化，集中工具契约测试，保留 C++ target 和模块测试布局。
- 私有账本视图检查留源库，历史结果退出测试输入；保留纯汇总函数及其回归。
- 修复 DXC 发现、无提交构建身份、私人归档默认路径与文档引用。
- 补充公共 README、开发指南、架构图、关键决策、资产与第三方来源。
- 实际构建、测试与剩余限制见 [批次 B](evidence/BATCH-B.md)，不继承旧测试数量。

## 2026-09-20 · 公开工程批次 A

初始化独立本地 Git 仓库，选择 MIT，建立公开清单和 V2 证据框架。
来源 MiniEngine 保留私有历史；[导入记录](evidence/IMPORT-REVIEW.md)是当时快照。

## 2026-09-20：C 批次本地验证

新增连续 Demo 锚点、三轮双后端彩排、25 runs 数值重算；保留 streaming 负结果。修正历史源码身份表述，性能完整复现仍 BLOCKED，未发布。

## 2026-09-20 · 当前源码新性能实验

新增固定区组采集器、统计复算器与12项契约测试；两次独立源码/Release 构建和预演通过，正式组分别在第18/22次因前台覆盖不足 INVALID。M7 benchmark 新增预热前前台就绪等待及超时证据，未放宽计时门槛。新性能结论仍 BLOCKED，见[记录](evidence/NEW-PERFORMANCE.md)。

## 2026-09-20 · 后续串行任务修订

补充 P/D/E/F 的前置条件、执行步骤、交付与阻断规则，加入非特定平台/模型的 agent 启动提示。所有工作继续在 LumaBough 串行进行。修正作品集入口的 C 批次过时状态；本次只改文档，未启动采样、Capture、打包或发布。
