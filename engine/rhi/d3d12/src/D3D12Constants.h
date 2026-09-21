// ============================================================================
// D3D12Constants.h — 四个 root CBV（b0—b3）与两个 pass 私有 b0 的 CPU 侧布局
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；迁移顺序第 1 步的"constant"）
// 职责：把 M4 的常量数据按**逐字节相同**的布局搬到 D3D12，并用 static_assert
//       冻结大小/对齐/偏移。布局相同是 M5-09 parity 的第一道前提——同一份
//       RenderPacket/材质数据在两个后端必须落到同样的字节序列，否则差异会表现为
//       "shader 读到的字段错位"，而这类错误在像素比较里只能看到一个说不清来源的色差。
// D3D11 侧的参照物**不是六个 C++ 类型**（审查三-1 纠正，此前注释写错会误导后人）：
//   - 真正的 C++ 结构体只有**四个**——FrameConstants / ObjectConstants /
//     MaterialConstants 在 `D3D11Renderer.cpp`、PostProcessConstants 在
//     `D3D11HdrTarget.h`；
//   - b3 的 IblConstants 与 skybox 的 b0 在 D3D11 侧**只是 HLSL cbuffer 名**：
//     CPU 侧是就地构造的 XMFLOAT4 / XMFLOAT4X4 直接 memcpy 上传
//     （`D3D11Renderer.cpp` 的 UpdateIblConstants / UpdateSkyboxConstants）。
//   本文件把这两者也提升为具名类型，目的是给 D3D12 的常量上传一个统一入口；
//   字节布局与 D3D11 的**实际上传内容**一致（16B / 64B），已由测试锁定。
// 为什么全部 float4：fxc/dxc 对 float3/float2 混合字段的隐式打包与 CPU struct
//       会错位（M4-03 实测：DebugMode 读到 0）。float4-only 是布局唯一可靠形态，
//       与 D3D11 侧的四个结构体保持同一原则。
// 为什么放在 src/ 而不是 include/：这些是渲染器实现细节，不是公共 API；
//       但测试需要断言布局，因此做成头文件（测试经 src/ include 目录直连，
//       与 D3D12Queue/D3D12PsoFactory 等内部头的口径一致）。
// 与 HLSL 的对应：shaders/d3d12/{PbrForward,ShadowDepth,Skybox,ToneMap}.hlsl 的
//       cbuffer 声明必须与本文件逐字段一致；两侧任一侧增删字段都要同批修改。
//       该一致性由 D3D12CbufferDriftTests **机读校验**（解析 HLSL 源逐字段对账），
//       不靠人眼——审查三-3 指出：只断言 CPU 偏移不足以支撑"与 HLSL 一致"的声称。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（三个结构体 + Ibl/Skybox 的实际上传）
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11HdrTarget.h（PostProcessConstants）
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12RootBindings.h（b0—b3 契约）
// ============================================================================
#pragma once

#include <MiniEngine/World/WorldTypes.h>

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>

namespace MiniEngine::Rhi::D3D12
{
// ---------------------------------------------------------------------------
// b0 FrameConstants：每帧一次，128B。**只有 PbrForward 消费它**——
// `ShadowDepth.hlsl` 只声明 ObjectConstants、**不声明任何 b0**；Skybox 与 ToneMap
// 各自在 b0 位置声明自己的 cbuffer（SkyboxConstants / ToneMapConstants）。
// 三者互不共用，在 D3D12 里由各 pass 互斥地绑到同一个 root CBV 0。
// （审查三-2：此前注释称"PbrForward 与 ShadowDepth 共用同一份"并提到
//  LightWorldViewProjection，与事实相反——该字段属 ObjectConstants。）
//
// 字段语义（与 PbrForward.hlsl 的 cbuffer FrameConstants 逐条对应）：
//   ViewProjection             view * projection（已转置的行主序）
//   CameraPositionAndDebugMode xyz = 相机世界位置，w = debug view 模式
//   DirectionAndIntensity      xyz = DirectionToLight（L），w = LightIntensity
//   LightColorAndPadding       xyz = 线性 RGB，w = 0
//   ShadowMapSizeAndPadding    xy = shadow map 分辨率，zw = 0
// ---------------------------------------------------------------------------
struct alignas(16) FrameConstants final
{
    DirectX::XMFLOAT4X4 viewProjection;           // 64B
    DirectX::XMFLOAT4 cameraPositionAndDebugMode; // 16B
    DirectX::XMFLOAT4 directionAndIntensity;      // 16B
    DirectX::XMFLOAT4 lightColorAndPadding;       // 16B
    DirectX::XMFLOAT4 shadowMapSizeAndPadding;    // 16B
};
static_assert(sizeof(FrameConstants) == 128, "FrameConstants must be 128 bytes (b0 layout is frozen)");
static_assert(alignof(FrameConstants) == 16);
static_assert(offsetof(FrameConstants, cameraPositionAndDebugMode) == 64);
static_assert(offsetof(FrameConstants, shadowMapSizeAndPadding) == 112);

// ---------------------------------------------------------------------------
// b1 ObjectConstants：每 Draw 一次，208B。与 ShadowDepth.hlsl 的 b1 声明逐字节
// 一致（03 篇硬约束：forward 与 shadow 用同一份对象常量，避免两侧各算一套矩阵）。
//
//   World                      world 矩阵（已转置的行主序）
//   NormalMatrix               3×3 inverse-transpose（非均匀缩放下 ≠ World）
//   LightWorldViewProjection   光空间 VP（固定 shadow volume）
//   HandednessAndReceivesShadow x = WorldHandedness（mirrored ? -1 : +1），
//                               y = ReceivesShadow（1/0），zw = 0
// ---------------------------------------------------------------------------
struct alignas(16) ObjectConstants final
{
    DirectX::XMFLOAT4X4 world;                     // 64B
    DirectX::XMFLOAT4X4 normalMatrix;              // 64B
    DirectX::XMFLOAT4X4 lightWorldViewProjection;  // 64B
    DirectX::XMFLOAT4 handednessAndReceivesShadow; // 16B
};
static_assert(sizeof(ObjectConstants) == 208, "ObjectConstants must be 208 bytes (b1 layout is frozen)");
static_assert(alignof(ObjectConstants) == 16);
static_assert(offsetof(ObjectConstants, normalMatrix) == 64);
static_assert(offsetof(ObjectConstants, lightWorldViewProjection) == 128);
static_assert(offsetof(ObjectConstants, handednessAndReceivesShadow) == 192);

// ---------------------------------------------------------------------------
// b2 MaterialConstants：每 Draw 一次，48B（02 篇「Material constants」契约）。
// 不使用 C++ bool，也不写未初始化 padding 进常量缓冲。
//
//   BaseColorFactor               线性 RGBA 乘子
//   EmissiveAndMetallic           rgb = emissiveFactor，a = metallicFactor
//   RoughnessNormalOcclusionFlags x = roughness，y = normalScale，
//                                 z = occlusionStrength，w = flags（恒 0）
// ---------------------------------------------------------------------------
struct alignas(16) MaterialConstants final
{
    DirectX::XMFLOAT4 baseColorFactor;               // 16B
    DirectX::XMFLOAT4 emissiveAndMetallic;           // 16B
    DirectX::XMFLOAT4 roughnessNormalOcclusionFlags; // 16B
};
static_assert(sizeof(MaterialConstants) == 48, "MaterialConstants must be 48 bytes (b2 layout is frozen)");
static_assert(alignof(MaterialConstants) == 16);
static_assert(offsetof(MaterialConstants, emissiveAndMetallic) == 16);
static_assert(offsetof(MaterialConstants, roughnessNormalOcclusionFlags) == 32);

// ---------------------------------------------------------------------------
// b3 IblConstants：environment 变化时更新，16B（float4-only 原则）。
//   PrefilterMipCountAndFlags x = prefiltered mip 数，yzw = 0
// 用途：roughness → LOD 换算的分母基准（prefiltered cube 的 mip 数）。
// ---------------------------------------------------------------------------
struct alignas(16) IblConstants final
{
    DirectX::XMFLOAT4 prefilterMipCountAndFlags;
};
static_assert(sizeof(IblConstants) == 16, "IblConstants must be one 16-byte constant row");
static_assert(alignof(IblConstants) == 16);

// ---------------------------------------------------------------------------
// Skybox 的 b0：去相机平移的 view * projection，64B。
// 与 PbrForward 的 b0 不是同一份数据（skybox 只要一个矩阵），因此独立成类型；
// 两者在 D3D12 里都是"root CBV 0"，由各 pass 在录制时分别绑定各自的缓冲。
// ---------------------------------------------------------------------------
struct alignas(16) SkyboxConstants final
{
    DirectX::XMFLOAT4X4 viewProjectionWithoutTranslation; // 64B
};
static_assert(sizeof(SkyboxConstants) == 64, "SkyboxConstants must be one 4x4 row (64 bytes)");
static_assert(alignof(SkyboxConstants) == 16);

// ---------------------------------------------------------------------------
// ToneMap 的 b0：PostProcessConstants，16B。与 D3D11 侧的
// Rhi::D3D11::PostProcessConstants 同布局（D3D11HdrTarget.h）。
//
//   ExposureEv        相对 EV；shader 按 exp2(ExposureEv) 缩放 HDR radiance
//   DebugHdr          1 = SceneLuminance 假色（证明 HDR 中存在 >1 的亮度）
//   InverseOutputSize 1 / 后备缓冲尺寸（像素）
// ---------------------------------------------------------------------------
struct alignas(16) PostProcessConstants final
{
    float exposureEv = 0.0F;
    std::uint32_t debugHdr = 0U;
    DirectX::XMFLOAT2 inverseOutputSize{0.0F, 0.0F};
};
static_assert(sizeof(PostProcessConstants) == 16, "PostProcessConstants must be exactly one 16-byte constant row");
static_assert(alignof(PostProcessConstants) == 16);
static_assert(offsetof(PostProcessConstants, exposureEv) == 0);
static_assert(offsetof(PostProcessConstants, debugHdr) == 4);
static_assert(offsetof(PostProcessConstants, inverseOutputSize) == 8);

// ---------------------------------------------------------------------------
// 矩阵转换 helper：引擎 row-major row-vector Matrix4 → HLSL mul(vector, matrix)
// 所需的**转置行主序**。实现与 D3D11Renderer.cpp 的同名函数逐表达式一致——
// 两侧必须产生同一字节序列，否则 parity 无从谈起。
// ---------------------------------------------------------------------------
[[nodiscard]] inline DirectX::XMFLOAT4X4 TransposedForHlsl(const World::Matrix4& matrix) noexcept
{
    const auto& engine = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(matrix.values.data());
    DirectX::XMFLOAT4X4 result{};
    DirectX::XMStoreFloat4x4(&result, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&engine)));
    return result;
}

// skybox 专用：把 view 的平移行清零后再乘 projection，得到"去平移"的 VP。
// 立方体顶点即方向向量，相机移动时天空不产生视差（05 篇 Skybox 契约第 1 条）。
[[nodiscard]] inline DirectX::XMFLOAT4X4 ViewProjectionWithoutTranslation(const World::Matrix4& view,
                                                                          const World::Matrix4& projection) noexcept
{
    const auto engineView = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(view.values.data());
    const auto engineProjection = *reinterpret_cast<const DirectX::XMFLOAT4X4*>(projection.values.data());

    DirectX::XMMATRIX viewMatrix = DirectX::XMLoadFloat4x4(&engineView);
    viewMatrix.r[3] = DirectX::XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F); // 去掉相机平移

    const DirectX::XMMATRIX result = DirectX::XMMatrixMultiply(viewMatrix, DirectX::XMLoadFloat4x4(&engineProjection));
    DirectX::XMFLOAT4X4 transposed{};
    DirectX::XMStoreFloat4x4(&transposed, DirectX::XMMatrixTranspose(result));
    return transposed;
}
} // namespace MiniEngine::Rhi::D3D12
