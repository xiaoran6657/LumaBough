// ============================================================================
// RenderPacket.h — 当前帧渲染快照：World → 渲染端的唯一数据通道
// 里程碑：M4（01 篇架构边界与渲染顺序；手抄清单第 1 条）
// 职责：定义一帧内不可变的 CPU 渲染契约：相机/光照数据、主视锥与光视锥
//       culling 后的绘制列表、守恒统计。RenderPacket 是帧快照，不是通用
//       RHI——它不包含 ID3D11*/ComPtr/bind slot/owning pointer，也不持有
//       跨帧有效的 World/Entity 指针；Asset payload 只按 Handle 借用。
// 关联：docs/architecture/README.md（当前帧数据边界）
//       engine/world/src/RenderQueueBuilder.cpp（唯一构建方）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（消费方，02 篇起接入）
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/World/DirectionalLight.h>
#include <MiniEngine/World/Frustum.h>

namespace MiniEngine::Assets
{
// 前置声明：句柄只需类型区分，不需要完整定义（M4-02 引入 MaterialAsset 后落地）。
struct MaterialAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::World
{
// 单个不透明 Draw 的完整 CPU 侧描述。
//
// 与 M3 的 World::RenderItem 相比字段更丰富（normal matrix、世界包围球、
// AssetId 排序键、shadow 标记）。M3 已拥有 RenderItem 这个名字，因此本 M4
// 快照刻意使用 RenderDraw，两个类型在增量迁移期共存；迁移完成后在独立
// 重构中统一命名（01 篇"当前帧数据边界"）。
struct RenderDraw final
{
    // Mesh 与材质的进程内句柄：渲染端经 Handle+revision 自行解析 payload 并
    // 上传 GPU（D3D11AssetCache），extraction 阶段只借用、不拥有。
    Assets::AssetHandle<Assets::MeshAsset> mesh;
    Assets::AssetHandle<Assets::MaterialAsset> material; // M4-02 起填充

    // 规范身份（AssetId 字节序）：稳定排序键与诊断用。
    // 排序绝不能用 Handle 的 slot/generation——reload 会改变槽位分配顺序，
    // 破坏截图可复现性；AssetId 与内容/顺序无关，跨 reload 稳定。
    // material 在 M4-02 前保持无效值，排序自然退化到 meshId → entityIndex。
    Assets::AssetId materialId{};
    Assets::AssetId meshId{};

    // 世界矩阵与 normal matrix（3x3 inverse-transpose，供切线空间法线变换；
    // 非均匀缩放下与 world 不同，这正是它单独存在的原因）。
    // Object constant buffer 还需写入 WorldHandedness = mirrored ? -1 : +1，
    // 供 normal mapping 修正切线手性；PBR 与 Shadow shader 的 b1 布局逐字节一致。
    Matrix4 world{};
    Matrix4 normal{};

    // 世界空间保守包围球（本地球经 TransformSphereConservative 变换）。
    Sphere worldBounds{};

    // 输入 RenderItem 在 BuildRenderItems() 确定性顺序中的位置：作为排序的
    // 最终并列键，保证同 id 物体的顺序也确定。
    std::uint32_t entityIndex = 0;

    // world 3x3 行列式为负（镜像/负缩放）：渲染端据此选择相反正面绕序。
    bool mirrored = false;
    // 阴影参与标记。默认 true：M4 固定场景所有物体默认投影/受影；
    // 改为默认 false 会让 shadow pass 静默失效且统计口径错位（M4-02 起由组件显式携带）。
    bool castsShadow = true;
    bool receivesShadow = true;
};

// 单次 BuildRenderPacket 的 culling / queue 统计（06 篇「统计」清单，12 项）。
//
// 守恒不变量（由 BuildRenderPacket 保证，测试逐条断言）：
//   candidateObjects = mainVisible + mainCulled
//   shadowCandidates = shadowVisible + shadowCulled
//   mainVisible      = mainInside + mainIntersecting
// 其中 mainInside/mainIntersecting 是"仍绘制"的细分，不改变守恒式——
// intersect 一律保留（06 篇：宁可多画，绝不漏剔除）。
//
// 统计全部由 CPU 决策产生，不依赖 GPU query；`D3D11_QUERY_PIPELINE_STATISTICS`
// 只作 baseline capture 的交叉证据，不逐帧阻塞读取（06 篇「统计」）。
struct CullingStats final
{
    std::uint32_t candidateObjects{};   // 输入的 RenderItem 数（含 mesh 信息缺失者）
    std::uint32_t mainVisible{};        // 主视锥非 Outside（进入 mainOpaque）
    std::uint32_t mainCulled{};         // 主视锥 Outside
    std::uint32_t mainInside{};         // 六面全部 Inside
    std::uint32_t mainIntersecting{};   // 与某平面相交（保守保留）
    std::uint32_t shadowCandidates{};   // castsShadow=true 的物体数
    std::uint32_t shadowVisible{};      // 光视锥非 Outside（进入 shadowCasters）
    std::uint32_t shadowCulled{};       // 光视锥 Outside
    std::uint32_t opaqueDrawCalls{};    // mainOpaque.size()
    std::uint32_t shadowDrawCalls{};    // shadowCasters.size()
    std::uint32_t trianglesSubmitted{}; // 本帧提交的主队列三角形总数（indexCount/3）
    double cullingCpuMicroseconds{};    // 两次 culling 分类的 CPU 耗时
    // M7-01 新增（附加字段，不改既有行为）：两个稳定排序的 CPU 耗时。M7 的 raw 指标
    // 需要把 cull 与 sort 分开记账；chunk 合并耗时在 M7-05 的 Builder 中才存在。
    double sortCpuMicroseconds{};
    double queueBuildCpuMicroseconds{}; // 整个 BuildRenderPacket（含排序）的耗时
};

// 一帧的不可变渲染快照：由 BuildRenderPacket 在帧边界一次性构建，
// 之后渲染端只读消费，Draw 循环中不得修改 World（01 篇固定帧序列）。
struct RenderPacket final
{
    // 相机数据（row-major row-vector、LH、D3D 深度 0..1 约定）。
    // viewProjection = view * projection，已可直接喂给 Frustum 提取。
    Matrix4 view{};
    Matrix4 projection{};
    Matrix4 viewProjection{};
    // 相机世界位置（PBR 高光与 IBL 方向需要）。
    Float3 cameraWorldPosition{};
    // 当前帧的方向光与固定 shadow volume。
    DirectionalLight directionalLight{};

    // 主视锥 culling 后的不透明绘制列表（已按 AssetId 字节稳定排序）。
    std::vector<RenderDraw> mainOpaque;
    // 光视锥 culling 后的 shadow caster 列表（独立于主视锥，已稳定排序）。
    std::vector<RenderDraw> shadowCasters;

    // 06 篇 culling / queue 统计（守恒不变量见 CullingStats 注释）。
    // 渲染端不消费统计（只消费两个队列），它是 culling 验收与 A/B 的唯一数据源。
    CullingStats stats{};
};
} // namespace MiniEngine::World
