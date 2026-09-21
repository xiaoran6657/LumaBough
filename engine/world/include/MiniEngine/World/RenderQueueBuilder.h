// ============================================================================
// RenderQueueBuilder.h — 从 RenderItem 快照构建 M4 RenderPacket（culling + 排序）
// 里程碑：M4（01 篇架构边界与渲染顺序）
// 职责：把 World::BuildRenderItems() 的确定性 RenderItem 快照升级为当前帧
//       RenderPacket：主视锥与光视锥两次 bounding-sphere culling、按 AssetId
//       字节的稳定排序、守恒统计，以及 light view-projection / normal matrix /
//       本地包围球三个纯数学辅助。全部为 CPU 数据，不暴露 D3D / 平台类型。
// 关联：docs/architecture/README.md（RenderPacket 边界）
//       engine/world/src/Frustum.cpp（平面提取与球分类）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetHandle.h>
#include <MiniEngine/Assets/AssetId.h>
#include <MiniEngine/World/DirectionalLight.h>
#include <MiniEngine/World/Frustum.h>
#include <MiniEngine/World/RenderPacket.h>
#include <MiniEngine/World/WorldTypes.h>

#include <functional>
#include <optional>
#include <span>

namespace MiniEngine::Assets
{
struct MeshAsset;
} // namespace MiniEngine::Assets

namespace MiniEngine::World
{
// 相机输入：World 层不依赖 RHI 的 D3D11Camera，由调用方（Sandbox）提取数据传入。
// view / projection 遵循引擎 row-major row-vector、LH、D3D 深度 0..1 约定。
struct RenderPacketCamera final
{
    Matrix4 view{};
    Matrix4 projection{};
    Float3 worldPosition{};
};

// 一次 mesh 查询的结果：排序身份 + 本地空间包围球 + 索引数。
// 身份必须来自 AssetId（内容寻址、跨 reload 稳定），不得使用 Handle 的 slot/generation
// ——重载会改变槽位分配顺序，破坏截图可复现性（01 篇 render queue 契约）。
// indexCount 只用于 06 篇的 trianglesSubmitted 统计（culling A/B 的 GPU work 代理量），
// 不参与剔除判定；为 0 时该物体不贡献三角形数（header-only 占位 payload 的情形）。
struct MeshDrawInfo final
{
    Assets::AssetId id{};
    Sphere localBounds{};
    std::uint32_t indexCount{};
};

// culling 开关（06 篇「CLI」）：main 与 shadow **必须可独立控制**——
// 关掉主视锥剔除但仍剔除屏幕外 caster，会让"shadow queue 独立"这条契约无法验证；
// 反之亦然。合用一个布尔会让 A/B 的开关含义模糊。
struct CullingOptions final
{
    bool mainCulling = true;
    bool shadowCulling = true;
};

// Handle → (AssetId, 本地包围球) 的只读查询。
// 生产路径由 Sandbox 用 AssetManager 实现（payload 反查 + 包围球缓存）；
// 单元测试注入固定映射。返回 nullopt 表示句柄失效或 payload 未就绪。
using MeshInfoLookup = std::function<std::optional<MeshDrawInfo>(Assets::AssetHandle<Assets::MeshAsset>)>;

// Handle → 稳定材质身份的只读查询。返回 nullopt 表示材质句柄失效或材质尚未就绪；
// 此时 RenderDraw::material 仍保留句柄，但 materialId 保持无效，排序仍可确定。
using MaterialInfoLookup = std::function<std::optional<Assets::AssetId>(Assets::AssetHandle<Assets::MaterialAsset>)>;

// 构建固定光视锥（DirectionalLight 的正交 shadow volume）的 view-projection。
// row-major row-vector、LH、D3D 深度 0..1；结果直接可喂给
// Frustum::FromRowVectorDirect3D 与未来的 ShadowDepth pass。
// 失败：光方向与 up 平行、含非有限分量或退化盒子时抛 std::runtime_error。
[[nodiscard]] Matrix4 BuildLightViewProjection(const DirectionalLight& light);

// 从 MeshAsset 顶点字节流现算本地空间包围球（AABB 半对角线保守球）。
// 只读 position（各 stride 的前 12 字节），不依赖具体顶点格式版本。
// 返回：顶点非空且字节流完整时为包围球；空网格 / stride 不足 / 数据缺失为 nullopt。
[[nodiscard]] std::optional<Sphere> ComputeMeshLocalBounds(const Assets::MeshAsset& mesh);

// 从 world 矩阵推导 normal matrix：3x3 inverse-transpose（row-vector 约定下
// 法线变换为 n' = n * (M3x3^-1)^T）。奇异矩阵（不可逆 3x3）抛 std::runtime_error。
[[nodiscard]] Matrix4 NormalMatrixFromWorld(const Matrix4& world);

// 构建当前帧 RenderPacket：两次视锥 culling + 稳定排序 + 守恒统计。
//
// items 必须来自 World::BuildRenderItems()（已按 Entity index 确定性排序），
// 本函数用输入位置作为 RenderDraw::entityIndex 的稳定并列键。
// meshInfo 返回 nullopt（句柄失效/payload 未就绪）时该物体保守地跳过 culling
// （恒可见），不因查询缺失而静默丢物体。
//
// 参数：
//   items        —— 本帧 RenderItem 快照
//   camera       —— 主相机数据（view/projection/eye）
//   light        —— 固定 DirectionalLight（含固定 shadow volume）
//   meshInfo     —— mesh 身份与包围球查询（不可为空 function）
//   options      —— main/shadow culling 独立开关（A/B timing 与正确性对照用）
// 返回：可直接交给渲染端消费的当前帧快照（含 06 篇 12 项统计）。
// 失败：矩阵/球含非有限值、奇异 normal matrix 或退化视锥时抛 std::runtime_error。
[[nodiscard]] RenderPacket BuildRenderPacket(const std::span<const RenderItem> items, const RenderPacketCamera& camera,
                                             const DirectionalLight& light, const MeshInfoLookup& meshInfo,
                                             const CullingOptions& options,
                                             const MaterialInfoLookup& materialInfo = {});

// 主/影队列的稳定排序键（M4 契约：materialId → meshId → mirrored → entityIndex；
// shadow 队列跳过 materialId）。公开给 M7-05 的并行 Builder 复用——串行与并行
// 必须使用同一比较器，输出才可能逐字节一致（M7-A14/A15）。
[[nodiscard]] bool StableOpaqueLess(const RenderDraw& left, const RenderDraw& right);
[[nodiscard]] bool StableShadowLess(const RenderDraw& left, const RenderDraw& right);
} // namespace MiniEngine::World
