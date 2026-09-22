# 项目 FAQ

## 为什么做这个项目 MiniEngine 与 LumaBough 是什么关系

目标是在 Windows / C++20 工程里理解并验证渲染后端、Render Graph、资源生命周期与多线程数据流。
MiniEngine 是私有开发工程；LumaBough 是按逐文件范围筛选的独立公开作品集，内部程序名仍保留 MiniEngine。
不承诺商业通用引擎或完整编辑器。来源边界见[来源说明](../evidence/PROVENANCE.md)，模块见[架构](../architecture/README.md)。

## 本人 AI 与第三方分别承担什么

所有者负责项目目标、范围与取舍决策、验收安排、人工核对和公开授权；
开发过程中使用 AI Agent 辅助实现、测试、文档整理与审查。
因此不能把仓库描述为全部代码独立手写，也不能仅据 Git 作者字段推断逐文件人工贡献比例。
具体模块应结合提交、问题记录和现场讲解说明，不在这里声称未经核实的工时或占比。

自有工程的设计与集成不等于自行实现第三方依赖或创作全部模型纹理。
第三方库、测试资产、许可和署名见[第三方声明](../../THIRD-PARTY-NOTICES.md)及[资产许可](../../assets/LICENSES.md)。
自有部分采用 MIT，第三方保留各自条件。

## 当前能展示什么 不能展示什么

Core 限 Windows x64、D3D11 / D3D12。公开源码包含 RenderPacket、Render Graph、资源生命周期、
资产烘焙与 revision 加载等工程机制；运行包仅覆盖声明的固定场景与双后端路径。
Vulkan、完整编辑器、通用资产导入、设备丢失自动恢复不在当前承诺中。
源码导览与实测结论分别见[架构](../architecture/README.md)和[候选支持矩阵](../evidence/CANDIDATE-REVIEW.md)。

## 视频里的动态行为是真的哪一种

视频来自双后端连续运行，展示程序化 RHI 尺寸变更/恢复、shader 候选接受与拒绝。
不能由此推导操作系统最小化、文件监听热更新、自由相机或同进程切换后端。
静态 anchor 与录制流程、确切边界见[Demo](DEMO.md)及[D 记录](../evidence/BATCH-D.md)。
原始 GPU Capture/PDB 只在本地核对，公众不能从现有附件下载回放。

## 解决过什么真实问题 如何验证

候选运行包曾因着色器包头带入构建机绝对路径而不具备独立运行条件。
修复为按 EXE 目录解析并携带字节码；构建树改名隔离验证与第二台机器的双后端运行支持该修复，
见[变更记录](../CHANGELOG.md)、[候选裁定](../evidence/CANDIDATE-REVIEW.md)与[E4](../evidence/E4-RESULTS.json)。

性能采样也经历前三组 INVALID：前台覆盖不足或就绪超时。
后续诊断可观测的启动链与焦点条件，再用新实验 ID 和完整区组得到 004；未删失败 run 或拼接成功部分。
001/002 当时没有夺取者记录，不能倒推归因；证据和限制见[新实验记录](../evidence/NEW-PERFORMANCE.md)。

## 性能为什么有下降数字却不说优化成功

004 的 25 次运行是完整有效组，但四项比较为 1 INCONCLUSIVE + 3 REJECTED，无 ACCEPTED。
packet 中位 CPU 帧时间下降约 16.90%，候选时间漂移与尾部 hitch 未过门，不能接受为加速结论；
streaming 三项未检出可接受的 CPU 改善。历史约 16.44% 来自另一实验及判定器，实际源码身份仍未证明。
两者不混用，也不外推到所有场景或发布运行包。完整表与条件见[性能报告](PERFORMANCE.md)。

## 怎样下载运行或复算 需要哪些环境

[产物索引](../evidence/ARTIFACT-INDEX.md)提供 Release 下载。先按外部校验表验证 ZIP，解压后按 RUN.md 启动双后端；
运行包不要求 Visual Studio、SDK、DXC 或 Python，仍须受支持的 Windows x64 与图形设备。
源码构建另有工具链与依赖要求，见[开发指南](../DEVELOPMENT.md)。

性能复算只需 Python 与[公开证据包](../evidence/PERFORMANCE-EVIDENCE-PACK.md)，不需要 GPU 重测。
它包含 25 × 600 逐帧样本、协议、组记录和固定分析器；保留失败及负结果边界，不代表完整环境复现。

## 还有什么没有验证 下一步是什么

第二台机器与匿名下载后运行已完成，但真实读者走查仍按所有者决定延后，不能声称已证实易用性或普遍兼容。
当前公开材料没有解决历史性能源码身份，也没有提供原始 Capture 的匿名回放。
后续可补真人反馈或独立新实验；是否增加功能、原始附件或新 Release 由所有者决定。
新性能结论必须绑定新协议、源码和完整运行，不覆盖现有失败记录；见[路线图](../ROADMAP.md)。
