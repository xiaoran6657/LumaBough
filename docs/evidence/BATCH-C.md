# C 批次：Demo 与历史性能复算

日期：2026-09-20。范围：本地 LumaBough；未提交、未推送、未发布。

## 结果

- Demo：PASS / LOCAL。两后端各三次可见窗口连续彩排，每次 1202 帧。静态锚点一致、事件序列通过、前后像素相同、资源退休 gate 通过。
- 性能数值：PASS。完整 25 runs / 15,000 samples 重算，主案例和全部负例复现；真实时间顺序漂移未改变结论。
- 历史源码绑定：BLOCKED。原始 EXE 的实际源码快照未证明，C-M9-002 不升级 PASS。C 批次整体仍有此验收缺口。
- Debug / Release 沙箱重新构建通过；两配置各 1002 项 CTest 功能回归通过，随后 PortfolioEntry 各 1 项通过，合计各 1003 项。740 文件的本地公开候选清单已应用并通过入口校验；不等于发布许可。
- 工具测试：顶层 42 项、contracts 目录 30 项通过。系统 Python 缺 Pillow 的首次失败已保留日志，随后使用项目 out/python 虚拟环境成功。
- C++ 只改 M6SceneRunner 的锚点/事件取证，不更改 engine 算法。recipe 删除不存在的 overlay、seed/fixedDelta 控制承诺；动态能力据实现收窄。

## 证据与接续

[Demo 操作与边界](../portfolio/DEMO.md) · [彩排身份](DEMO-REHEARSAL.json) ·
[性能报告](../portfolio/PERFORMANCE.md) · [重算摘要](PERFORMANCE-SUMMARY.json)

本地 raw/日志与失败尝试位于 out/m9/batch-c。公开材料仅使用明确挑选、脱敏并绑定 hash 的统计输入和摘要，大日志/PPM/EXE 不进入公开清单。B 批次 RENDER-PREVIEW 的旧源码/运行身份保持原样，没有替换成 C 源码。

本批提案已应用，PortfolioEntry 已通过。后续 agent 可接续 D 的正式 Capture/动态视频。D 不自动解除历史性能的源码身份阻断；E/F 的包、许可、匿名访问和发布授权仍独立验收。推荐的新性能实验需要完整冻结协议、当前源码与 EXE 哈希，另记实验 ID；不得冒充历史 16.44% 的重测闭合。
