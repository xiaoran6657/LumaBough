// ============================================================================
// MaterialAsset.h — PBR 材质资产的 CPU 载荷与 `.memat` v1 契约
// 里程碑：M4（02 篇 Material 资产与 metallic-roughness PBR）
// 职责：定义 glTF core metallic-roughness 材质烘焙后的运行时载荷：五个因子、
//       五个可选贴图的 AssetId 引用与 alpha mode。M4 只接受 OPAQUE；MASK/BLEND、
//       doubleSided 与材质扩展由 Cooker hard fail（见 MaterialBaker），不静默画错。
//       磁盘 `.memat` 由 INFO（factors + alpha mode）与 TEXR（AssetId + usage）
//       两个 chunk 构成，逐字段 little-endian 序列化，禁止整结构体 memcpy。
// 关联：docs/architecture/README.md「MaterialAsset」
//       tools/asset_cooker/src/MaterialArtifactWriter.cpp（写入端）
//       engine/assets/src/AssetManager.cpp（DecodeMaterialChunks 读取端）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetId.h>

#include <array>
#include <cstdint>

namespace MiniEngine::Assets
{
// M4 材质透明模式：目前只接受 OPAQUE；MASK/BLEND 属于 M11 的透明实验范围。
enum class AlphaMode : std::uint8_t
{
    Opaque = 0
};

// PBR 材质载荷。因子语义遵循 glTF core metallic-roughness：
//   baseColorFactor —— 线性空间 RGBA 乘子（baseColorTexture.rgb 在 sRGB 解码后与之相乘）
//   emissiveFactor  —— 线性空间 RGB 乘子，各分量必须在 [0,1]（Cooker 逐分量校验）
//   metallicFactor / roughnessFactor —— clamp 到 [0,1] 后进入 shader
//   normalScale     —— 只乘 normal map 的 XY 分量后 normalize
//   occlusionStrength —— shader 端 lerp(1, ao, strength)
// 五个贴图位以 AssetId（内容无关身份）引用 `.metex`；未使用的位保持全零
// （AssetId::IsValid() == false），渲染端据此绑定真实 fallback SRV。
struct MaterialAsset final
{
    std::array<float, 4> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> emissiveFactor{0.0F, 0.0F, 0.0F};
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    float normalScale = 1.0F;
    float occlusionStrength = 1.0F;

    AssetId baseColorTexture{};
    AssetId metallicRoughnessTexture{};
    AssetId normalTexture{};
    AssetId occlusionTexture{};
    AssetId emissiveTexture{};

    AlphaMode alphaMode = AlphaMode::Opaque;
};

// `.memat` 的格式版本（通过 BakedHeader.flags 载波；见 BakedFormat.h 的说明）。
inline constexpr std::uint32_t kMaterialFormatVersion = 1;

// 守护契约：Manifest kind 字符串与池类型一一对应（"material" → AssetPool<MaterialAsset>）。
// 磁盘 IO 必须逐字段按 little-endian 显式序列化：padding 不是文件格式，
// 禁止 reinterpret_cast 整个内存结构体。
} // namespace MiniEngine::Assets
