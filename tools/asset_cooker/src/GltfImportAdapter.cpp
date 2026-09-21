// ============================================================================
// GltfImportAdapter.cpp — fastgltf 导入适配层（G3）的实现
// 里程碑：M3（02 篇 G3/G3b）
// 职责：解析 glTF 并转换为引擎自有类型：accessor 展开（attributes 走 fastgltf
//       tools，indices 手写解码）、节点层级与 TRS 收集、材质过滤（OPAQUE /
//       非 doubleSided / TEXCOORD_0）、外部 .bin 与图片经 SourceUri 沙箱读入。
//       fastgltf 类型不跨出本文件。
// 关联：tools/asset_cooker/src/GltfImportAdapter.h（职责边界）
//       fastgltf v0.9.0（spnda/fastgltf，API 已核对）
// ============================================================================

#include "GltfImportAdapter.h"

#include "SourceUri.h"
#include "TangentGenerator.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace MiniEngine::Tools
{
namespace
{
bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::byte>& out)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
    {
        return false;
    }
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0)
    {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(end));
    if (!out.empty())
    {
        stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return stream.good();
    }
    return true;
}

[[nodiscard]] bool AppendDiagnostic(DiagnosticSink& diagnostics, const std::string& context,
                                    const std::string& message)
{
    diagnostics.Add(context, message);
    return false;
}

// G3b：外部 URI → SourceUri 沙箱 → 读文件。sourceDirectory 是 glTF 所在目录，
// sourceRoot 是资产根；成功后把规范化相对路径加入外部依赖清单（确定性）。
bool ResolveExternalUri(const fastgltf::URI& uri,
                        const std::filesystem::path& sourceDirectory,
                        const std::filesystem::path& sourceRoot,
                        const std::string& context,
                        std::vector<std::byte>& bytes,
                        std::string& normalizedPath,
                        DiagnosticSink& diagnostics)
{
    // fastgltf::URI 已做 percent-decode；取整体 string 后交沙箱按 7 步验证。
    const std::string uriText(uri.string());

    ResolvedSourceUri resolved;
    UriRejectReason reason{};
    std::string sandboxError;
    if (!ResolveSourceUri(uriText, sourceDirectory, sourceRoot, resolved, reason, sandboxError))
    {
        return AppendDiagnostic(diagnostics, context,
                                "URI rejected (" + std::string(UriRejectReasonName(reason)) + "): " + uriText + ": " +
                                    sandboxError);
    }
    if (resolved.isDataUri)
    {
        return AppendDiagnostic(diagnostics, context, "data URI should not reach external resolver: " + uriText);
    }
    if (!ReadFileBytes(resolved.absolutePath, bytes))
    {
        return AppendDiagnostic(diagnostics, context, "failed to read external file: " + resolved.absolutePath.string());
    }
    normalizedPath = resolved.normalizedRelativePath;
    return true;
}

std::string_view MimeToString(const fastgltf::MimeType mime) noexcept
{
    switch (mime)
    {
        case fastgltf::MimeType::PNG:
            return "image/png";
        case fastgltf::MimeType::JPEG:
            return "image/jpeg";
        default:
            return "";
    }
}

// 展平 buffer 字节：Data URI（sources::Array）、GLB（sources::ByteView）、
// sources::Vector 直接拷贝；sources::URI（外部 .bin）经 SourceUri 沙箱加载。
// DefaultBufferDataAdapter 遇 URI 会 assert，因此本函数在进入 accessor 前
// 就把所有 buffer 变成内存字节。
bool CollectBufferBytes(const fastgltf::Asset& asset,
                        std::vector<std::vector<std::byte>>& buffers,
                        DiagnosticSink& diagnostics,
                        const std::filesystem::path& sourceDirectory,
                        const std::filesystem::path& sourceRoot,
                        std::vector<std::string>& externalDependencyPaths)
{
    buffers.resize(asset.buffers.size());
    for (std::size_t index = 0; index < asset.buffers.size(); ++index)
    {
        const fastgltf::DataSource& data = asset.buffers[index].data;
        const std::string context = "buffer/" + std::to_string(index);
        const bool handled = std::visit(
            [&](const auto& source) -> bool
            {
                using T = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<T, fastgltf::sources::Array> ||
                              std::is_same_v<T, fastgltf::sources::ByteView> ||
                              std::is_same_v<T, fastgltf::sources::Vector>)
                {
                    buffers[index].assign(source.bytes.begin(), source.bytes.end());
                    return true;
                }
                else if constexpr (std::is_same_v<T, fastgltf::sources::URI>)
                {
                    std::string normalizedPath;
                    return ResolveExternalUri(source.uri, sourceDirectory, sourceRoot, context, buffers[index],
                                              normalizedPath, diagnostics) &&
                           (externalDependencyPaths.push_back(normalizedPath), true);
                }
                else
                {
                    return AppendDiagnostic(diagnostics, context, "unsupported buffer source");
                }
            },
            data);
        if (!handled)
        {
            return false;
        }
    }
    return true;
}

// 手写标量 accessor 解码（indices）：覆盖 UNSIGNED_BYTE/SHORT/INT；索引不允许 normalized。
std::optional<std::uint32_t> ReadIndexScalar(const fastgltf::Accessor& accessor,
                                             const std::vector<std::byte>& buffer,
                                             const fastgltf::BufferView& bufferView,
                                             const std::size_t element)
{
    if (!accessor.bufferViewIndex.has_value() || accessor.type != fastgltf::AccessorType::Scalar)
    {
        return std::nullopt;
    }

    const std::size_t offset = accessor.byteOffset + bufferView.byteOffset;
    switch (accessor.componentType)
    {
        case fastgltf::ComponentType::UnsignedByte:
        {
            const auto* value = reinterpret_cast<const std::uint8_t*>(buffer.data() + offset + element);
            return static_cast<std::uint32_t>(*value);
        }
        case fastgltf::ComponentType::UnsignedShort:
        {
            const auto* value = reinterpret_cast<const std::uint16_t*>(buffer.data() + offset + element * 2);
            return static_cast<std::uint32_t>(*value);
        }
        case fastgltf::ComponentType::UnsignedInt:
        {
            const auto* value = reinterpret_cast<const std::uint32_t*>(buffer.data() + offset + element * 4);
            return *value;
        }
        default:
            return std::nullopt;
    }
}

bool ReadIndices(const fastgltf::Asset& asset,
                 const fastgltf::Accessor& accessor,
                 const std::vector<std::vector<std::byte>>& buffers,
                 std::vector<std::uint32_t>& indices,
                 DiagnosticSink& diagnostics,
                 const std::string& context)
{
    if (!accessor.bufferViewIndex.has_value())
    {
        return AppendDiagnostic(diagnostics, context, "indices accessor has no bufferView");
    }
    const fastgltf::BufferView& view = asset.bufferViews[*accessor.bufferViewIndex];
    const std::vector<std::byte>& buffer = buffers[view.bufferIndex];

    indices.reserve(accessor.count);
    for (std::size_t element = 0; element < accessor.count; ++element)
    {
        const auto value = ReadIndexScalar(accessor, buffer, view, element);
        if (!value.has_value())
        {
            return AppendDiagnostic(diagnostics, context, "unsupported index component type");
        }
        indices.push_back(*value);
    }
    return true;
}

// 关键：fastgltf 的 DefaultBufferDataAdapter 从 asset.buffers[..].data 取数，
// 而外部 URI buffer 我们已自行加载进沙箱字节向量（asset.buffers 仍是 URI）。
// 必须提供自定义 adapter：operator()(asset, bufferViewIndex) 从沙箱字节返回 span，
// 否则 iterateAccessor 会命中 Default 的 assert。
struct SandboxedBufferAdapter final
{
    const std::vector<std::vector<std::byte>>* buffers{};
    const fastgltf::Asset* asset{};

    [[nodiscard]] std::span<const std::byte> operator()(const fastgltf::Asset&, const std::size_t bufferViewIndex) const
    {
        const fastgltf::BufferView& view = asset->bufferViews[bufferViewIndex];
        const std::vector<std::byte>& bytes = (*buffers)[view.bufferIndex];
        return std::span<const std::byte>{bytes}.subspan(view.byteOffset, view.byteLength);
    }
};

template <typename ElementType, typename Sink>
void ReadAttribute(const fastgltf::Asset& asset, const fastgltf::Accessor& accessor, Sink&& append,
                   const SandboxedBufferAdapter& adapter)
{
    fastgltf::iterateAccessor<ElementType>(asset, accessor, std::forward<Sink>(append), adapter);
}

// fastgltf math::vec 用 operator[] 访问分量（v0.9.0）。
[[nodiscard]] GltfVec3 ToGltfVec3(const fastgltf::math::fvec3& value)
{
    return GltfVec3{value[0], value[1], value[2]};
}

[[nodiscard]] GltfVec2 ToGltfVec2(const fastgltf::math::fvec2& value)
{
    return GltfVec2{value[0], value[1]};
}

[[nodiscard]] GltfTangent ToGltfTangent(const fastgltf::math::fvec4& value)
{
    return GltfTangent{value[0], value[1], value[2], value[3]};
}

// textureInfo → image 下标（texture→source 链；M4 材质只记录 image 引用，
// Cooker 按 (image, usage) 烘焙 .metex）。无 image source 时为 nullopt。
[[nodiscard]] std::optional<std::size_t> ResolveImageIndex(const fastgltf::TextureInfo& info,
                                                           const fastgltf::Asset& asset)
{
    if (info.textureIndex >= asset.textures.size())
    {
        return std::nullopt;
    }
    return asset.textures[info.textureIndex].imageIndex;
}

bool ResolveImage(const fastgltf::Asset& asset,
                  const std::size_t imageIndex,
                  const std::filesystem::path& sourceDirectory,
                  const std::filesystem::path& sourceRoot,
                  ImportedImage& image,
                  std::vector<std::string>& externalDependencyPaths,
                  DiagnosticSink& diagnostics)
{
    const fastgltf::Image& gltfImage = asset.images[imageIndex];
    const std::string context = "image/" + std::to_string(imageIndex);
    return std::visit(
        [&](const auto& source) -> bool
        {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, fastgltf::sources::URI>)
            {
                image.source = ImportedImage::SourceKind::ExternalFile;
                image.mimeHint = std::string(MimeToString(source.mimeType));
                return ResolveExternalUri(source.uri, sourceDirectory, sourceRoot, context, image.embeddedBytes,
                                          image.normalizedRelativePath, diagnostics) &&
                       (externalDependencyPaths.push_back(image.normalizedRelativePath), true);
            }
            else if constexpr (std::is_same_v<T, fastgltf::sources::Array>)
            {
                // Data URI 图片：fastgltf 已解码为内嵌字节。
                image.source = ImportedImage::SourceKind::DataUri;
                image.mimeHint = std::string(MimeToString(source.mimeType));
                image.embeddedBytes.assign(source.bytes.begin(), source.bytes.end());
                return true;
            }
            else if constexpr (std::is_same_v<T, fastgltf::sources::BufferView>)
            {
                // GLB 内嵌 bufferView 图片：从 buffers 字节拷贝。
                image.source = ImportedImage::SourceKind::GlbBufferView;
                image.mimeHint = std::string(MimeToString(source.mimeType));
                return true;
            }
            else
            {
                return AppendDiagnostic(diagnostics, context, "unsupported image source");
            }
        },
        gltfImage.data);
}

} // namespace

bool ImportGltf(const std::filesystem::path& source, const std::filesystem::path& sourceRoot,
                ImportResult& result, DiagnosticSink& diagnostics)
{
    result = ImportResult{};

    std::vector<std::byte> bytes;
    if (!ReadFileBytes(source, bytes))
    {
        return AppendDiagnostic(diagnostics, source.string(), "failed to read glTF source");
    }

    // 字节构造器是 protected；v0.9.0 用静态工厂 FromBytes（返回 Expected）。
    auto dataBuffer = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (dataBuffer.error() != fastgltf::Error::None)
    {
        return AppendDiagnostic(diagnostics, source.string(), "failed to wrap glTF bytes");
    }

    fastgltf::Parser parser;
    // DecomposeNodeMatrices：matrix 与 TRS 统一为 TRS（matrix/TRS 一致性 golden）。
    // 不设 LoadExternalBuffers/LoadExternalImages：外部资源必须经 SourceUri 沙箱。
    auto parsed = parser.loadGltf(dataBuffer.get(), source.parent_path(),
                                  fastgltf::Options::DecomposeNodeMatrices);
    if (parsed.error() != fastgltf::Error::None)
    {
        return AppendDiagnostic(diagnostics, source.string(),
                                "fastgltf parse failed: " + std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    const fastgltf::Asset& asset = parsed.get();

    // extensionsRequired 必须为空（02 篇 M3 支持矩阵）。
    if (!asset.extensionsRequired.empty())
    {
        const std::string requiredText(asset.extensionsRequired.front().c_str());
        return AppendDiagnostic(diagnostics, "asset", "unsupported required extension: " + requiredText);
    }

    const std::filesystem::path sourceDirectory = source.parent_path();
    std::vector<std::string>& externalPaths = result.externalDependencyPaths;

    // 收集 buffer 字节（Data URI / GLB / 外部文件）。
    std::vector<std::vector<std::byte>> buffers;
    if (!CollectBufferBytes(asset, buffers, diagnostics, sourceDirectory, sourceRoot, externalPaths))
    {
        return false;
    }
    const SandboxedBufferAdapter bufferAdapter{&buffers, &asset};

    // images：外部 URI 记依赖、Data URI/GLB 记字节（WIC 解码在 texture bake 篇）。
    for (std::size_t imageIndex = 0; imageIndex < asset.images.size(); ++imageIndex)
    {
        ImportedImage image;
        if (!ResolveImage(asset, imageIndex, sourceDirectory, sourceRoot, image, externalPaths, diagnostics))
        {
            return false;
        }
        result.images.push_back(std::move(image));
    }

    // 选定场景 root nodes：defaultScene 优先；无 scene 时所有 node 视为 root。
    std::vector<std::size_t> rootNodes;
    if (asset.defaultScene.has_value() && *asset.defaultScene < asset.scenes.size())
    {
        rootNodes.assign(asset.scenes[*asset.defaultScene].nodeIndices.begin(),
                         asset.scenes[*asset.defaultScene].nodeIndices.end());
    }
    else
    {
        rootNodes.resize(asset.nodes.size());
        for (std::size_t index = 0; index < asset.nodes.size(); ++index)
        {
            rootNodes[index] = index;
        }
    }

    // 从 roots 遍历 nodes（visited 防环；node key 一律 index）。
    std::vector<std::uint8_t> visited(asset.nodes.size(), 0);
    std::vector<std::size_t> pending = rootNodes;
    while (!pending.empty())
    {
        const std::size_t nodeIndex = pending.back();
        pending.pop_back();
        if (visited[nodeIndex])
        {
            continue;
        }
        visited[nodeIndex] = 1;

        const fastgltf::Node& node = asset.nodes[nodeIndex];
        ImportedNode importedNode;
        importedNode.gltfNodeIndex = nodeIndex;
        importedNode.name = node.name.empty() ? std::string("node/" + std::to_string(nodeIndex))
                                              : std::string(node.name.c_str());
        importedNode.meshIndex = node.meshIndex;
        importedNode.childIndices.assign(node.children.begin(), node.children.end());
        if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform))
        {
            importedNode.translation[0] = trs->translation[0];
            importedNode.translation[1] = trs->translation[1];
            importedNode.translation[2] = trs->translation[2];
            importedNode.rotation[0] = trs->rotation[0];
            importedNode.rotation[1] = trs->rotation[1];
            importedNode.rotation[2] = trs->rotation[2];
            importedNode.rotation[3] = trs->rotation[3];
            importedNode.scale[0] = trs->scale[0];
            importedNode.scale[1] = trs->scale[1];
            importedNode.scale[2] = trs->scale[2];
        }
        else
        {
            // DecomposeNodeMatrices 已开启，matrix 不应再出现；出现即解析器行为变化。
            return AppendDiagnostic(diagnostics, "node/" + std::to_string(nodeIndex),
                                    "unexpected non-TRS node transform (DecomposeNodeMatrices ineffective)");
        }
        result.nodes.push_back(std::move(importedNode));

        for (const std::size_t child : node.children)
        {
            if (child < asset.nodes.size() && !visited[child])
            {
                pending.push_back(child);
            }
        }

        if (!node.meshIndex.has_value() || *node.meshIndex >= asset.meshes.size())
        {
            continue;
        }
        const fastgltf::Mesh& mesh = asset.meshes[*node.meshIndex];
        for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            const fastgltf::Primitive& primitive = mesh.primitives[primitiveIndex];
            const std::string context =
                "node/" + std::to_string(nodeIndex) + " mesh/" + std::to_string(*node.meshIndex) +
                " primitive/" + std::to_string(primitiveIndex);

            if (primitive.type != fastgltf::PrimitiveType::Triangles)
            {
                return AppendDiagnostic(diagnostics, context, "primitive is not TRIANGLES");
            }
            if (primitive.dracoCompression != nullptr)
            {
                return AppendDiagnostic(diagnostics, context, "Draco compression is not supported");
            }

            const auto* positionAttribute = primitive.findAttribute("POSITION");
            if (positionAttribute == primitive.attributes.end())
            {
                return AppendDiagnostic(diagnostics, context, "missing POSITION attribute");
            }
            const fastgltf::Accessor& positionAccessor = asset.accessors[positionAttribute->accessorIndex];
            if (positionAccessor.type != fastgltf::AccessorType::Vec3)
            {
                return AppendDiagnostic(diagnostics, context, "POSITION must be Vec3");
            }

            const std::size_t vertexCount = positionAccessor.count;
            GeometryPrimitiveInput input;
            input.positions.reserve(vertexCount);
            ReadAttribute<fastgltf::math::fvec3>(
                asset, positionAccessor,
                [&](const fastgltf::math::fvec3& value) { input.positions.push_back(ToGltfVec3(value)); },
                bufferAdapter);

            const auto* normalAttribute = primitive.findAttribute("NORMAL");
            if (normalAttribute != primitive.attributes.end())
            {
                const fastgltf::Accessor& normalAccessor = asset.accessors[normalAttribute->accessorIndex];
                if (normalAccessor.count != vertexCount || normalAccessor.type != fastgltf::AccessorType::Vec3)
                {
                    return AppendDiagnostic(diagnostics, context, "NORMAL must be Vec3 with POSITION count");
                }
                input.normals.reserve(vertexCount);
                ReadAttribute<fastgltf::math::fvec3>(
                    asset, normalAccessor,
                    [&](const fastgltf::math::fvec3& value) { input.normals.push_back(ToGltfVec3(value)); },
                    bufferAdapter);
            }

            const auto* uvAttribute = primitive.findAttribute("TEXCOORD_0");
            if (uvAttribute != primitive.attributes.end())
            {
                const fastgltf::Accessor& uvAccessor = asset.accessors[uvAttribute->accessorIndex];
                if (uvAccessor.count != vertexCount || uvAccessor.type != fastgltf::AccessorType::Vec2)
                {
                    return AppendDiagnostic(diagnostics, context, "TEXCOORD_0 must be Vec2 with POSITION count");
                }
                input.uv0.reserve(vertexCount);
                ReadAttribute<fastgltf::math::fvec2>(
                    asset, uvAccessor,
                    [&](const fastgltf::math::fvec2& value) { input.uv0.push_back(ToGltfVec2(value)); },
                    bufferAdapter);
            }

            // TANGENT（可选）：glTF 自带切线，G2 按 RH→LH 反射规则转换（x/w 取反）。
            const auto* tangentAttribute = primitive.findAttribute("TANGENT");
            if (tangentAttribute != primitive.attributes.end())
            {
                const fastgltf::Accessor& tangentAccessor = asset.accessors[tangentAttribute->accessorIndex];
                if (tangentAccessor.count != vertexCount || tangentAccessor.type != fastgltf::AccessorType::Vec4)
                {
                    return AppendDiagnostic(diagnostics, context, "TANGENT must be Vec4 with POSITION count");
                }
                input.tangents.reserve(vertexCount);
                ReadAttribute<fastgltf::math::fvec4>(
                    asset, tangentAccessor,
                    [&](const fastgltf::math::fvec4& value) { input.tangents.push_back(ToGltfTangent(value)); },
                    bufferAdapter);
            }

            if (primitive.indicesAccessor.has_value())
            {
                if (!ReadIndices(asset, asset.accessors[*primitive.indicesAccessor], buffers, input.indices,
                                 diagnostics, context))
                {
                    return false;
                }
            }

            ImportedPrimitive imported;
            imported.nodeIndex = nodeIndex;
            imported.meshIndex = *node.meshIndex;
            imported.primitiveIndex = primitiveIndex;
            imported.stableKey = "mesh/" + std::to_string(*node.meshIndex) + "/primitive/" +
                                 std::to_string(primitiveIndex);
            if (primitive.materialIndex.has_value() && *primitive.materialIndex < asset.materials.size())
            {
                const fastgltf::Material& material = asset.materials[*primitive.materialIndex];
                // M4 材质 hard fail 三连：非 OPAQUE、doubleSided、emissive-strength
                // 扩展（02 篇：拒绝而非静默烘焙成越界 core factor）。
                if (material.alphaMode != fastgltf::AlphaMode::Opaque)
                {
                    return AppendDiagnostic(diagnostics, context, "alphaMode must be OPAQUE");
                }
                if (material.doubleSided)
                {
                    return AppendDiagnostic(diagnostics, context, "doubleSided=true is not supported");
                }
                if (material.emissiveStrength != 1.0F)
                {
                    return AppendDiagnostic(diagnostics, context,
                                            "KHR_materials_emissive_strength is not supported in M4");
                }
                ImportedMaterial importedMaterial;
                for (int component = 0; component < 4; ++component)
                {
                    importedMaterial.baseColorFactor[component] = material.pbrData.baseColorFactor[component];
                }
                for (int component = 0; component < 3; ++component)
                {
                    importedMaterial.emissiveFactor[component] = material.emissiveFactor[component];
                }
                importedMaterial.metallicFactor = material.pbrData.metallicFactor;
                importedMaterial.roughnessFactor = material.pbrData.roughnessFactor;
                // 五个 role 的 texture→image 解析；texCoordIndex 必须 0（M4 只绑 TEXCOORD_0）。
                if (const auto& texture = material.pbrData.baseColorTexture)
                {
                    if (texture->texCoordIndex != 0)
                    {
                        return AppendDiagnostic(diagnostics, context,
                                                "baseColorTexture must use TEXCOORD_0 (texCoordIndex==0)");
                    }
                    importedMaterial.baseColorImageIndex = ResolveImageIndex(*texture, asset);
                }
                if (const auto& texture = material.pbrData.metallicRoughnessTexture)
                {
                    if (texture->texCoordIndex != 0)
                    {
                        return AppendDiagnostic(diagnostics, context,
                                                "metallicRoughnessTexture must use TEXCOORD_0");
                    }
                    importedMaterial.metallicRoughnessImageIndex = ResolveImageIndex(*texture, asset);
                }
                if (material.normalTexture.has_value())
                {
                    if (material.normalTexture->texCoordIndex != 0)
                    {
                        return AppendDiagnostic(diagnostics, context, "normalTexture must use TEXCOORD_0");
                    }
                    importedMaterial.normalImageIndex = ResolveImageIndex(*material.normalTexture, asset);
                    importedMaterial.normalScale = material.normalTexture->scale;
                }
                if (material.occlusionTexture.has_value())
                {
                    if (material.occlusionTexture->texCoordIndex != 0)
                    {
                        return AppendDiagnostic(diagnostics, context, "occlusionTexture must use TEXCOORD_0");
                    }
                    importedMaterial.occlusionImageIndex = ResolveImageIndex(*material.occlusionTexture, asset);
                    importedMaterial.occlusionStrength = material.occlusionTexture->strength;
                }
                if (const auto& texture = material.emissiveTexture)
                {
                    if (texture->texCoordIndex != 0)
                    {
                        return AppendDiagnostic(diagnostics, context, "emissiveTexture must use TEXCOORD_0");
                    }
                    importedMaterial.emissiveImageIndex = ResolveImageIndex(*texture, asset);
                }
                imported.gltfMaterialIndex = *primitive.materialIndex;
                imported.material = std::move(importedMaterial);
            }

            // wantsNormalMap 来自材质的 normalTexture：tangent 三来源决策是
            // 几何与材质的交叉决策（02 篇「Tangent 规则」）。
            const bool wantsNormalMap = imported.material.has_value() && imported.material->normalImageIndex.has_value();
            std::string geometryError;
            const GeometryError code = BuildRuntimeMesh(input, wantsNormalMap, imported.mesh, geometryError);
            if (code != GeometryError::Ok)
            {
                return AppendDiagnostic(diagnostics, context,
                                        std::string("geometry rejected: ") + GeometryErrorName(code) + ": " +
                                            geometryError);
            }
            // Generated 路径：MikkTSpace 在 RH→LH 转换之后运行（02 篇推荐顺序，
            // 避免手工修正 tangent handedness），随后按完整顶点确定性去重。
            if (imported.mesh.tangentSource == TangentSource::Generated)
            {
                std::string tangentError;
                if (!GenerateTangents(imported.mesh, tangentError))
                {
                    return AppendDiagnostic(diagnostics, context, "tangent generation failed: " + tangentError);
                }
            }
            result.primitives.push_back(std::move(imported));
        }
    }

    // 外部依赖排序去重：Manifest 依赖清单的确定性基础。
    std::sort(externalPaths.begin(), externalPaths.end());
    externalPaths.erase(std::unique(externalPaths.begin(), externalPaths.end()), externalPaths.end());
    return true;
}
} // namespace MiniEngine::Tools
