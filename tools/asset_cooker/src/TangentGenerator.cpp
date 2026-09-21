// ============================================================================
// TangentGenerator.cpp — MikkTSpace 离线切线生成的实现
// 里程碑：M4-02
// 职责：以 face-corner 数据驱动 vendored MikkTSpace（genTangSpaceDefault），
//       把输出切线写入展开顶点流，再按完整 48 字节顶点做"首次出现顺序"的
//       确定性去重。MikkTSpace 只在本层出现，不泄漏到 G2/adapter 的类型里。
// 关联：tools/asset_cooker/src/TangentGenerator.h（契约）
//       tools/asset_cooker/deps/mikktspace/mikktspace.h（C 接口）
// ============================================================================

#include "TangentGenerator.h"

#include <MiniEngine/Assets/PbrVertex.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>

#include "mikktspace.h"

namespace MiniEngine::Tools
{
using MiniEngine::Assets::PbrVertex;

namespace
{
// MikkTSpace 回调的共享数据：corner 顶点流 + 去重输出。
// 约定：mesh.vertices 已是 face-corner 展开（G2 的 Generated 路径保证）。
struct MikkUserData final
{
    EnginePrimitiveMesh* mesh{};
    bool failed{};
    std::string error;
};

// face = cornerIndex / 3；corner 顶点流与展开后的 indices 一一对应。
int MikkGetNumFaces(const SMikkTSpaceContext* context)
{
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    return static_cast<int>(data->mesh->vertices.size() / 3U);
}

int MikkGetNumVerticesOfFace(const SMikkTSpaceContext*, const int)
{
    return 3;
}

int MikkGetVertexIndex(const SMikkTSpaceContext*, const int face, const int vert)
{
    return face * 3 + vert;
}

void MikkGetPosition(const SMikkTSpaceContext* context, float* outPosition, const int face, const int vert)
{
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const PbrVertex& vertex = data->mesh->vertices[static_cast<std::size_t>(MikkGetVertexIndex(context, face, vert))];
    outPosition[0] = vertex.position[0];
    outPosition[1] = vertex.position[1];
    outPosition[2] = vertex.position[2];
}

void MikkGetNormal(const SMikkTSpaceContext* context, float* outNormal, const int face, const int vert)
{
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const PbrVertex& vertex = data->mesh->vertices[static_cast<std::size_t>(MikkGetVertexIndex(context, face, vert))];
    outNormal[0] = vertex.normal[0];
    outNormal[1] = vertex.normal[1];
    outNormal[2] = vertex.normal[2];
}

void MikkGetTexCoord(const SMikkTSpaceContext* context, float* outTexCoord, const int face, const int vert)
{
    const auto* data = static_cast<const MikkUserData*>(context->m_pUserData);
    const PbrVertex& vertex = data->mesh->vertices[static_cast<std::size_t>(MikkGetVertexIndex(context, face, vert))];
    outTexCoord[0] = vertex.uv0[0];
    outTexCoord[1] = vertex.uv0[1];
}

// MikkTSpace 的切线输出：xyz 单位切线 + sign（bitangent 手性）。
void MikkSetTSpaceBasic(const SMikkTSpaceContext* context, const float* tangent, const float sign, const int face,
                        const int vert)
{
    auto* data = static_cast<MikkUserData*>(context->m_pUserData);
    PbrVertex& vertex = data->mesh->vertices[static_cast<std::size_t>(MikkGetVertexIndex(context, face, vert))];
    vertex.tangent[0] = tangent[0];
    vertex.tangent[1] = tangent[1];
    vertex.tangent[2] = tangent[2];
    vertex.tangent[3] = sign;
}

// 把 48 字节顶点作为去重键（位级比较：任何字段不同即不同顶点——seam 两侧的
// UV/切线差异因此得以保留）。
using VertexKey = std::array<unsigned char, sizeof(PbrVertex)>;

VertexKey MakeKey(const PbrVertex& vertex)
{
    VertexKey key{};
    std::memcpy(key.data(), &vertex, sizeof(PbrVertex));
    return key;
}
} // namespace

bool GenerateTangents(EnginePrimitiveMesh& mesh, std::string& error)
{
    error.clear();

    // 前置校验：Generated 路径必须是展开后的 corner 流（G2 契约）。
    if (mesh.tangentSource != TangentSource::Generated)
    {
        error = "GenerateTangents requires TangentSource::Generated";
        return false;
    }
    if (mesh.vertices.empty() || mesh.vertices.size() != mesh.indices.size() || mesh.vertices.size() % 3 != 0)
    {
        error = "Generated-tangent mesh must be face-corner expanded (vertex count == index count, multiple of 3)";
        return false;
    }
    for (const PbrVertex& vertex : mesh.vertices)
    {
        const float normalLengthSquared =
            vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2];
        if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 0.0F)
        {
            error = "corner vertex has zero or non-finite normal (MikkTSpace precondition)";
            return false;
        }
    }

    // 去重前先留一份原数据：MikkTSpace 失败时保证 mesh 保持调用前状态。
    const EnginePrimitiveMesh original = mesh;

    SMikkTSpaceInterface interface{};
    interface.m_getNumFaces = MikkGetNumFaces;
    interface.m_getNumVerticesOfFace = MikkGetNumVerticesOfFace;
    interface.m_getPosition = MikkGetPosition;
    interface.m_getNormal = MikkGetNormal;
    interface.m_getTexCoord = MikkGetTexCoord;
    interface.m_setTSpaceBasic = MikkSetTSpaceBasic;

    MikkUserData userData;
    userData.mesh = &mesh;
    SMikkTSpaceContext context{};
    context.m_pInterface = &interface;
    context.m_pUserData = &userData;

    // genTangSpaceDefault：默认角阈值（180 度），由 MikkTSpace 内部处理退化面。
    if (genTangSpaceDefault(&context) == 0)
    {
        error = "MikkTSpace failed to generate tangents";
        mesh = original;
        return false;
    }

    // tangent 手性必须恰好是 -1/+1（02 篇 PbrVertex 契约；MikkTSpace 的 sign 保证
    // 输出 ±1，这里 fail-closed 防御上游行为变化）。
    for (const PbrVertex& vertex : mesh.vertices)
    {
        if (vertex.tangent[3] != -1.0F && vertex.tangent[3] != 1.0F)
        {
            error = "MikkTSpace produced a tangent handedness other than -1/+1";
            mesh = original;
            return false;
        }
    }

    // 确定性去重：按完整 48 字节首次出现顺序保留唯一顶点，indices 重映射。
    // 首次出现顺序只依赖输入流的确定性，与运行环境无关。
    std::vector<PbrVertex> dedupedVertices;
    dedupedVertices.reserve(mesh.vertices.size());
    std::vector<std::uint32_t> dedupedIndices;
    dedupedIndices.reserve(mesh.indices.size());
    std::map<VertexKey, std::uint32_t> firstSeen;
    for (const std::uint32_t oldIndex : mesh.indices)
    {
        const VertexKey key = MakeKey(mesh.vertices[oldIndex]);
        const auto found = firstSeen.find(key);
        if (found != firstSeen.end())
        {
            dedupedIndices.push_back(found->second);
            continue;
        }
        const auto newIndex = static_cast<std::uint32_t>(dedupedVertices.size());
        dedupedVertices.push_back(mesh.vertices[oldIndex]);
        firstSeen.emplace(key, newIndex);
        dedupedIndices.push_back(newIndex);
    }

    mesh.vertices = std::move(dedupedVertices);
    mesh.indices = std::move(dedupedIndices);
    return true;
}
} // namespace MiniEngine::Tools
