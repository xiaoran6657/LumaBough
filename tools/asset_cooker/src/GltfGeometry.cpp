// ============================================================================
// GltfGeometry.cpp — glTF 几何转换核心（G2，v2）的实现
// 里程碑：M3（02 篇 G2）→ M4-02 升级 PbrVertex / tangent 三来源
// 职责：实现 GltfGeometry.h 的 v2 行为合同。与 M3 的差异：顶点布局换 48 字节
//       PbrVertex；新增 tangent 三来源决策与 face-corner 展开路径；
//       glTF TANGENT 的 RH→LH 反射（x 取反 + w 取反）在此一次完成。
// 关联：tools/asset_cooker/src/GltfGeometry.h（行为合同与规则出处）
//       tools/asset_cooker/src/TangentGenerator.cpp（Generated 路径的 MikkTSpace）
// ============================================================================

#include "GltfGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MiniEngine::Tools
{
namespace
{
bool AllFinite3(const std::vector<GltfVec3>& values)
{
    return std::all_of(values.begin(), values.end(),
                       [](const GltfVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); });
}

bool AllFinite2(const std::vector<GltfVec2>& values)
{
    return std::all_of(values.begin(), values.end(), [](const GltfVec2& v) { return std::isfinite(v.u) && std::isfinite(v.v); });
}

bool AllFiniteTangent(const std::vector<GltfTangent>& values)
{
    return std::all_of(values.begin(), values.end(), [](const GltfTangent& t) {
        return std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) && std::isfinite(t.w);
    });
}

GltfVec3 Cross(const GltfVec3& a, const GltfVec3& b)
{
    return GltfVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

GltfVec3 Sub(const GltfVec3& a, const GltfVec3& b)
{
    return GltfVec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

GltfVec3 Normalized(const GltfVec3& value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!(length > 0.0F) || !std::isfinite(length))
    {
        return GltfVec3{0.0F, 0.0F, 0.0F};
    }
    return GltfVec3{value.x / length, value.y / length, value.z / length};
}

void UpdateAabb(EnginePrimitiveMesh& mesh, const GltfVec3& position)
{
    mesh.aabbMin[0] = std::fmin(mesh.aabbMin[0], position.x);
    mesh.aabbMin[1] = std::fmin(mesh.aabbMin[1], position.y);
    mesh.aabbMin[2] = std::fmin(mesh.aabbMin[2], position.z);
    mesh.aabbMax[0] = std::fmax(mesh.aabbMax[0], position.x);
    mesh.aabbMax[1] = std::fmax(mesh.aabbMax[1], position.y);
    mesh.aabbMax[2] = std::fmax(mesh.aabbMax[2], position.z);
}

// 稳定 fallback tangent：选一个与 normal 夹角不退化的固定参考向量，投影到
// 切平面后 normalize。参考向量的选取只依赖 normal 的主轴，因此同一顶点在任何
// 环境下产出同一 tangent（无 normal map 时 bitangent 不参与光照，w 恒为 +1）。
GltfVec3 FallbackTangent(const GltfVec3& normal)
{
    const GltfVec3 reference =
        std::abs(normal.y) < 0.999F ? GltfVec3{0.0F, 1.0F, 0.0F} : GltfVec3{1.0F, 0.0F, 0.0F};
    const float dot = reference.x * normal.x + reference.y * normal.y + reference.z * normal.z;
    return Normalized(GltfVec3{reference.x - normal.x * dot, reference.y - normal.y * dot,
                               reference.z - normal.z * dot});
}
} // namespace

const char* TangentSourceName(const TangentSource source) noexcept
{
    switch (source)
    {
        case TangentSource::Provided:
            return "Provided";
        case TangentSource::Fallback:
            return "Fallback";
        case TangentSource::Generated:
            return "Generated";
    }
    return "Unknown";
}

const char* GeometryErrorName(const GeometryError code) noexcept
{
    switch (code)
    {
        case GeometryError::Ok:
            return "Ok";
        case GeometryError::EmptyPositions:
            return "EmptyPositions";
        case GeometryError::CountMismatch:
            return "CountMismatch";
        case GeometryError::NonTriangleIndexCount:
            return "NonTriangleIndexCount";
        case GeometryError::IndexOutOfRange:
            return "IndexOutOfRange";
        case GeometryError::NonFiniteValue:
            return "NonFiniteValue";
        case GeometryError::MissingUvForNormalMap:
            return "MissingUvForNormalMap";
    }
    return "Unknown";
}

GeometryError BuildRuntimeMesh(const GeometryPrimitiveInput& input, const bool wantsNormalMap, EnginePrimitiveMesh& out,
                               std::string& error)
{
    error.clear();

    // 全量验证先行：任何失败都不写 out（与 AssetManager 事务同一哲学）。
    if (input.positions.empty())
    {
        error = "primitive has no POSITION data";
        return GeometryError::EmptyPositions;
    }
    if (!AllFinite3(input.positions))
    {
        error = "POSITION contains non-finite values";
        return GeometryError::NonFiniteValue;
    }
    if (!input.normals.empty() && input.normals.size() != input.positions.size())
    {
        error = "NORMAL count differs from POSITION count";
        return GeometryError::CountMismatch;
    }
    if (!input.normals.empty() && !AllFinite3(input.normals))
    {
        error = "NORMAL contains non-finite values";
        return GeometryError::NonFiniteValue;
    }
    if (!input.uv0.empty() && input.uv0.size() != input.positions.size())
    {
        error = "TEXCOORD_0 count differs from POSITION count";
        return GeometryError::CountMismatch;
    }
    if (!input.uv0.empty() && !AllFinite2(input.uv0))
    {
        error = "TEXCOORD_0 contains non-finite values";
        return GeometryError::NonFiniteValue;
    }
    if (!input.tangents.empty() && input.tangents.size() != input.positions.size())
    {
        error = "TANGENT count differs from POSITION count";
        return GeometryError::CountMismatch;
    }
    if (!input.tangents.empty() && !AllFiniteTangent(input.tangents))
    {
        error = "TANGENT contains non-finite values";
        return GeometryError::NonFiniteValue;
    }
    // normal map 需要 UV 作为切线空间的投影基准：缺失即错误（02 篇 Tangent 规则）。
    if (wantsNormalMap && input.uv0.empty())
    {
        error = "normal-mapped primitive requires TEXCOORD_0";
        return GeometryError::MissingUvForNormalMap;
    }

    // indices 缺失 → 0..n-1；否则 3 的倍数 + 越界检查。
    std::vector<std::uint32_t> indices = input.indices;
    if (indices.empty())
    {
        indices.resize(input.positions.size());
        for (std::size_t index = 0; index < indices.size(); ++index)
        {
            indices[index] = static_cast<std::uint32_t>(index);
        }
    }
    else
    {
        if (indices.size() % 3 != 0)
        {
            error = "index count is not a multiple of 3 (TRIANGLES only)";
            return GeometryError::NonTriangleIndexCount;
        }
        for (const std::uint32_t index : indices)
        {
            if (index >= input.positions.size())
            {
                error = "index " + std::to_string(index) + " exceeds vertex count " +
                        std::to_string(input.positions.size());
                return GeometryError::IndexOutOfRange;
            }
        }
    }

    // RH→LH 顶点镜像：position.x/normal.x 取反、三角形交换 index[1]/[2]；UV 不翻转。
    out = EnginePrimitiveMesh{};
    // AABB 不能零初始化：坐标可能全为负（x 镜像后必然如此），否则 fmax 会被
    // 钉在 0。用 ±FLT_MAX 起步，首顶点立即收敛到真实范围。
    out.aabbMin[0] = out.aabbMin[1] = out.aabbMin[2] = std::numeric_limits<float>::max();
    out.aabbMax[0] = out.aabbMax[1] = out.aabbMax[2] = std::numeric_limits<float>::lowest();

    // tangent 三来源决策（02 篇优先级）。
    const bool hasProvidedTangents = !input.tangents.empty();
    const bool generateNormals = input.normals.empty();
    const bool expandCorners = generateNormals || (wantsNormalMap && !hasProvidedTangents);
    out.tangentSource = hasProvidedTangents ? TangentSource::Provided
                                            : (wantsNormalMap ? TangentSource::Generated : TangentSource::Fallback);

    // 中间顶点：转换 + 法线/tangent 逐字段填充，最后统一写 PbrVertex。
    struct ConvertedVertex final
    {
        float position[3];
        float normal[3];
        float tangent[4];
        float uv0[2];
    };
    std::vector<ConvertedVertex> converted(input.positions.size());
    for (std::size_t index = 0; index < input.positions.size(); ++index)
    {
        ConvertedVertex& vertex = converted[index];
        vertex.position[0] = -input.positions[index].x; // RH→LH：x 镜像
        vertex.position[1] = input.positions[index].y;
        vertex.position[2] = input.positions[index].z;
        vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0.0F;
        vertex.tangent[0] = vertex.tangent[1] = vertex.tangent[2] = 0.0F;
        vertex.tangent[3] = 1.0F;
        vertex.uv0[0] = vertex.uv0[1] = 0.0F;
        if (!input.normals.empty())
        {
            // RH→LH：normal.x 同步取反（与 position 同一反射）。
            vertex.normal[0] = -input.normals[index].x;
            vertex.normal[1] = input.normals[index].y;
            vertex.normal[2] = input.normals[index].z;
        }
        if (hasProvidedTangents)
        {
            // 反射变换下保持 B = cross(N, T) * w 一致：tangent.xyz.x 取反 + w 取反
            //（02 篇「Tangent 规则」明确公式；不得改成 Z 轴反射）。
            vertex.tangent[0] = -input.tangents[index].x;
            vertex.tangent[1] = input.tangents[index].y;
            vertex.tangent[2] = input.tangents[index].z;
            vertex.tangent[3] = -input.tangents[index].w;
        }
        if (!input.uv0.empty())
        {
            vertex.uv0[0] = input.uv0[index].u;
            vertex.uv0[1] = input.uv0[index].v;
        }
    }

    // 提供了 NORMAL：逐顶点 normalize 已镜像的法线。
    if (!generateNormals)
    {
        for (ConvertedVertex& vertex : converted)
        {
            const GltfVec3 normalized =
                Normalized(GltfVec3{vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            vertex.normal[0] = normalized.x;
            vertex.normal[1] = normalized.y;
            vertex.normal[2] = normalized.z;
        }
    }

    // fallback tangent 逐顶点生成（只依赖已镜像 normalize 的 normal，确定性）。
    if (out.tangentSource == TangentSource::Fallback)
    {
        for (ConvertedVertex& vertex : converted)
        {
            const GltfVec3 tangent =
                FallbackTangent(GltfVec3{vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = 1.0F;
        }
    }

    // 组装输出顶点流：expandCorners=true 时按交付绕序拆 face-corner（flat normal /
    // MikkTSpace 前置），否则保持索引结构。
    out.vertices.reserve(expandCorners ? indices.size() : converted.size());
    if (!expandCorners)
    {
        for (const ConvertedVertex& source : converted)
        {
            PbrVertex vertex{};
            for (int axis = 0; axis < 3; ++axis)
            {
                vertex.position[axis] = source.position[axis];
                vertex.normal[axis] = source.normal[axis];
                vertex.tangent[axis] = source.tangent[axis];
            }
            vertex.tangent[3] = source.tangent[3];
            vertex.uv0[0] = source.uv0[0];
            vertex.uv0[1] = source.uv0[1];
            UpdateAabb(out, GltfVec3{vertex.position[0], vertex.position[1], vertex.position[2]});
            out.vertices.push_back(vertex);
        }
        out.indices.reserve(indices.size());
        for (std::size_t triangle = 0; triangle < indices.size() / 3; ++triangle)
        {
            // 每个三角形交换 index[1]/[2]（RH→LH 绕序保持可见面一致）。
            out.indices.push_back(indices[triangle * 3]);
            out.indices.push_back(indices[triangle * 3 + 2]);
            out.indices.push_back(indices[triangle * 3 + 1]);
        }
    }
    else
    {
        // face-corner 展开：indices 交付绕序三元组顺序展开；flat normal 路径用
        // cross(b-a, c-a) 重算，Generated 路径保留原 smooth normal（Mikk 前置）。
        for (std::size_t triangle = 0; triangle < indices.size() / 3; ++triangle)
        {
            const std::uint32_t delivered[3] = {indices[triangle * 3], indices[triangle * 3 + 2],
                                                indices[triangle * 3 + 1]};
            GltfVec3 faceNormal{0.0F, 0.0F, 0.0F};
            if (generateNormals)
            {
                const GltfVec3 a{converted[delivered[0]].position[0], converted[delivered[0]].position[1],
                                 converted[delivered[0]].position[2]};
                const GltfVec3 b{converted[delivered[1]].position[0], converted[delivered[1]].position[1],
                                 converted[delivered[1]].position[2]};
                const GltfVec3 c{converted[delivered[2]].position[0], converted[delivered[2]].position[1],
                                 converted[delivered[2]].position[2]};
                faceNormal = Normalized(Cross(Sub(b, a), Sub(c, a)));
            }

            for (const std::uint32_t sourceIndex : delivered)
            {
                ConvertedVertex vertex = converted[sourceIndex];
                if (generateNormals)
                {
                    vertex.normal[0] = faceNormal.x;
                    vertex.normal[1] = faceNormal.y;
                    vertex.normal[2] = faceNormal.z;
                }
                PbrVertex output{};
                for (int axis = 0; axis < 3; ++axis)
                {
                    output.position[axis] = vertex.position[axis];
                    output.normal[axis] = vertex.normal[axis];
                    output.tangent[axis] = vertex.tangent[axis];
                }
                output.tangent[3] = vertex.tangent[3];
                output.uv0[0] = vertex.uv0[0];
                output.uv0[1] = vertex.uv0[1];
                UpdateAabb(out, GltfVec3{output.position[0], output.position[1], output.position[2]});
                out.vertices.push_back(output);
            }
            const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size() - 3);
            out.indices.push_back(base);
            out.indices.push_back(base + 1);
            out.indices.push_back(base + 2);
        }
    }

    // bounding sphere：AABB 中心 + 最远顶点距离（转换后重算）。
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        out.boundingSphereCenter[axis] = (out.aabbMin[axis] + out.aabbMax[axis]) * 0.5F;
    }
    float radius = 0.0F;
    for (const PbrVertex& vertex : out.vertices)
    {
        const float dx = vertex.position[0] - out.boundingSphereCenter[0];
        const float dy = vertex.position[1] - out.boundingSphereCenter[1];
        const float dz = vertex.position[2] - out.boundingSphereCenter[2];
        radius = std::fmax(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    out.boundingSphereRadius = radius;
    return GeometryError::Ok;
}
} // namespace MiniEngine::Tools
