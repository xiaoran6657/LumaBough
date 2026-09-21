# 关键设计决策

这是针对公开源码的精选说明，基于既有实现、契约测试和已记录的历史结果重写，
不是原私有 ADR 的逐字复制，也不改变原冻结结论。当前验证身份见 [批次 B](../evidence/BATCH-B.md)。

## 1. 两个可工作的后端之后再提炼共享 RHI

约束：D3D11 的即时上下文和 D3D12 的显式提交/状态不同；
过早抽象会把某个 API 的方便行为伪装成通用契约。
选择：在两种 backend 的资源、提交、读回和失败行为已有实现后，
提炼 public RHI、adapter 与 factory；上层 Renderer/Graph 不链接 native API。

代价：保留 adapter 和底层公共 native runtime，存在少量历史 concrete 路径；
不能只凭同名函数认定两后端完全等价。
验收依据是 [RHI 边界工具](../../tools/validation/check_rhi_boundary.py)、
[CMake composition 工具](../../tools/validation/check_rhi_composition.py)
和 [RHI/图测试](../../tests/rhi/CMakeLists.txt)。
只有新增后端实际暴露无法表达的资源/同步语义时才扩展契约；不为未来 Vulkan 预填虚构能力。

## 2. 句柄身份、CPU 请求与 GPU 使用期限分离

约束：CPU 可以请求新 revision，但 GPU 仍可能使用旧资源；立即销毁或在途删除都会破坏所有权。
选择：以稳定句柄/版本表达身份，revision 在帧点提交，失败保留旧 Ready；
GPU 对象通过完成信号后退休，请求记录由终态释放与容量护栏管理。

代价：需要显式状态、队列、退休列表及 shutdown 契约。
[资产加载测试](../../tests/assets/AsyncAssetLoaderTests.cpp)和
[退休测试](../../tests/rhi/d3d12/DeferredReleaseTests.cpp)分别覆盖不同生命周期。
历史池指标恒定没有覆盖进程记录保留；相关 RSS 缺陷及 2k 修复验证
在 [声明勘正](../evidence/HISTORICAL-CLAIMS.md)披露。
若后续多个 GPU queue 或跨设备改变完成语义，应重新审查退休边界。

## 3. 并行 packet 以稳定结果与有效场景为前提

约束：worker 调度顺序不稳定，渲染排序和资源访问必须可重复。
选择：worker 只读冻结输入，局部构建后按稳定键合并；
[RenderPacketBuilder](../../engine/render/src/RenderPacketBuilder.cpp)
和 [任务系统](../../engine/tasks/src/TaskSystem.cpp)不直接并行调用 RHI。

代价：同步、合并和调度存在开销。历史 METHOD-003 在 packet-bound 负载改善帧时间，
COMBINED-001 在 streaming 场景没有同样收益；不改变成“并行默认总是更快”的说法。
公开 A/B 报告仍需完整原始输入和重算链，目前见 [Ledger](../evidence/CLAIM-LEDGER.csv)。
输入布局、稳定排序或任务调度契约变化时重测等价性；性能推荐随占比和噪声证据修订。
