// ============================================================================
// MeshArtifactWriter.cpp — .memesh（Mesh Baked artifact）写入的实现
// 里程碑：M3-04（审计 3.1 抽取；审计 3.3 防御加固）
// 职责：按 wire 偏移显式写出 header(kind=Mesh, chunkCount=2)、VERT/INDX 描述符
//       与 16 对齐数据区；顶点/索引数量超 u32 时失败，输出字节确定性。
// 关联：tools/asset_cooker/src/MeshArtifactWriter.h（wire 布局）
//       engine/assets/src/AssetManager.cpp（DecodeMeshChunks 读取端）
// ============================================================================

#include "MeshArtifactWriter.h"

#include "BakedWriter.h"

#include <MiniEngine/Assets/BakedFormat.h>
#include <MiniEngine/Assets/PbrVertex.h>

#include <cstring>
#include <limits>

namespace MiniEngine::Tools
{
bool BuildMeshArtifact(const Sha256Digest& buildKey, const EnginePrimitiveMesh& mesh, std::vector<std::byte>& out,
                       std::string& error)
{
    using namespace MiniEngine::Assets;
    out.clear();
    error.clear();
    // 与引擎解码端对称的防御：descriptor 的 elementCount 是 u32，超界必须显式失败
    //（读侧会按 u32 拒绝），而不是静默截断。recipes 的 primitive budget 更小，
    // 此处是 writer 自身的 fail-closed 保证。
    if (mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        error = "mesh vertex count exceeds u32";
        return false;
    }
    if (mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        error = "mesh index count exceeds u32";
        return false;
    }

    constexpr std::size_t kChunkTableStart = static_cast<std::size_t>(kBakedHeaderSize);     // 64
    constexpr std::size_t kVertDescriptorOffset = kChunkTableStart;                          // 64
    constexpr std::size_t kIndxDescriptorOffset = kChunkTableStart + kChunkDescriptorSize;   // 96
    constexpr std::size_t kDataStart = kIndxDescriptorOffset + kChunkDescriptorSize;         // 128

    const std::size_t vertexByteCount = mesh.vertices.size() * sizeof(PbrVertex);
    const std::size_t indexByteCount = mesh.indices.size() * sizeof(std::uint32_t);
    const std::size_t vertexDataOffset = kDataStart;
    const std::size_t indexDataOffset = BakedWriter::AlignUp(kDataStart + vertexByteCount, kChunkAlignment);
    const std::size_t artifactSize = indexDataOffset + indexByteCount;

    out.assign(artifactSize, std::byte{0});
    // M4 v2：flags 载荷 kMeshFormatVersion（读取端输出 recook diagnostic 的依据）。
    BakedWriter::WriteBakedHeader(out, BakedAssetKind::Mesh, buildKey, 2,
                                  static_cast<std::uint64_t>(artifactSize), kMeshFormatVersion);
    BakedWriter::WriteChunkDescriptor(out, kVertDescriptorOffset, "VERT",
                                      static_cast<std::uint64_t>(vertexDataOffset),
                                      static_cast<std::uint64_t>(vertexByteCount),
                                      static_cast<std::uint32_t>(mesh.vertices.size()), sizeof(PbrVertex));
    BakedWriter::WriteChunkDescriptor(out, kIndxDescriptorOffset, "INDX",
                                      static_cast<std::uint64_t>(indexDataOffset),
                                      static_cast<std::uint64_t>(indexByteCount),
                                      static_cast<std::uint32_t>(mesh.indices.size()), sizeof(std::uint32_t));

    if (!mesh.vertices.empty())
    {
        std::memcpy(out.data() + vertexDataOffset, mesh.vertices.data(), vertexByteCount);
    }
    if (!mesh.indices.empty())
    {
        std::memcpy(out.data() + indexDataOffset, mesh.indices.data(), indexByteCount);
    }
    return true;
}
} // namespace MiniEngine::Tools
