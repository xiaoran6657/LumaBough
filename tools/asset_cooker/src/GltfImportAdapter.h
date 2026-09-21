#pragma once

// G3：glTF 导入适配层（fastgltf v0.9.0 已核对 API）。
// 职责边界（02 篇）：
//   解析 glTF → 拒绝 extensionsRequired / 非 M3 内容 → 展开 accessor
//   （attributes 走 fastgltf tools；indices 因 scalar 语义差异手写解码）→
//   组装 GeometryPrimitiveInput → 交给 G2 BuildRuntimeMesh → 产出引擎自有类型。
//   材质：仅 OPAQUE / 非 doubleSided / TEXCOORD_0（texCoordIndex==0）通过。
//   Node：记录稳定 source-local key 与转换前 local transform（world 篇消费）。
// 引擎自有类型不泄漏 fastgltf；外部 .bin / 图片 URI 的沙箱解析在 G3b 接入
// SourceUri（本版 Data URI / GLB buffer 已就绪）。

#include "GltfGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// 诊断记录：context 描述来源（如 "accessor" / "node/<i>"），message 为可读原因。
struct DiagnosticRecord final
{
    std::string context;
    std::string message;
};

// 导入期诊断收集器：只累积、不中断，是否失败由调用方结合返回值判断。
class DiagnosticSink final
{
  public:
    void Add(std::string context, std::string message)
    {
        records.push_back(DiagnosticRecord{std::move(context), std::move(message)});
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return records.empty();
    }

    std::vector<DiagnosticRecord> records;
};

struct ImportedImage final
{
    enum class SourceKind
    {
        ExternalFile,
        DataUri,
        GlbBufferView
    };

    SourceKind source{};
    std::string normalizedRelativePath; // ExternalFile：写入依赖清单
    std::vector<std::byte> embeddedBytes; // DataUri / GlbBufferView
    std::string mimeHint; // "image/png" | "image/jpeg"
};

// glTF core metallic-roughness 材质的导入结果（02 篇映射表）。
// 各 role 记录 glTF texture → image 下标（Cooker 按 (image, usage) 烘焙 .metex）。
// alphaMode 非 OPAQUE / doubleSided / emissiveStrength 扩展在导入期即拒绝（hard fail）。
struct ImportedMaterial final
{
    std::array<float, 4> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> emissiveFactor{0.0F, 0.0F, 0.0F};
    float metallicFactor{1.0F};
    float roughnessFactor{1.0F};
    float normalScale{1.0F};
    float occlusionStrength{1.0F};
    std::optional<std::size_t> baseColorImageIndex;
    std::optional<std::size_t> metallicRoughnessImageIndex;
    std::optional<std::size_t> normalImageIndex;
    std::optional<std::size_t> occlusionImageIndex;
    std::optional<std::size_t> emissiveImageIndex;
};

struct ImportedPrimitive final
{
    EnginePrimitiveMesh mesh; // G2 转换后（引擎左手系、绕序已交换、tangent 三来源、AABB/sphere）
    std::size_t nodeIndex{};
    std::size_t meshIndex{};
    std::size_t primitiveIndex{};
    std::string stableKey; // "mesh/<meshIndex>/primitive/<primitiveIndex>"（不依赖名称）
    std::optional<std::size_t> gltfMaterialIndex; // glTF materialIndex（`.memat` URI 身份）
    std::optional<ImportedMaterial> material;
};

struct ImportedNode final
{
    std::size_t gltfNodeIndex{};
    std::string name; // 原 name 或确定性 fallback "node/<index>"
    // G3b：parser 以 DecomposeNodeMatrices 解析，matrix 与 TRS 统一落在 TRS，
    // 由 world 篇按 MiniEngine 左手系组合（matrix/TRS 一致性由 golden 锁定）。
    float translation[3]{};
    float rotation[4]{0.0F, 0.0F, 0.0F, 1.0F};
    float scale[3]{1.0F, 1.0F, 1.0F};
    std::vector<std::size_t> childIndices; // glTF node index
    std::optional<std::size_t> meshIndex;
};

struct ImportResult final
{
    std::vector<ImportedNode> nodes;
    std::vector<ImportedPrimitive> primitives;
    std::vector<ImportedImage> images;
    // 经 SourceUri 沙箱解析过的外部依赖（buffer/image），规范化相对 sourceRoot、
    // '/' 分隔，已排序——CookRecipe 写入 Manifest 的依赖清单来源。
    std::vector<std::string> externalDependencyPaths;
};

// source 为绝对路径；sourceRoot 用于 URI 沙箱。失败返回 false 且 diagnostics 非空。
[[nodiscard]] bool ImportGltf(const std::filesystem::path& source,
                              const std::filesystem::path& sourceRoot,
                              ImportResult& result,
                              DiagnosticSink& diagnostics);
}
