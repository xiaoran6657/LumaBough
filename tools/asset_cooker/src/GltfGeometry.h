#pragma once

// ============================================================================
// GltfGeometry.h — glTF 几何导入核心（G2，v2 契约）
// 里程碑：M3（02 篇 G2）→ M4-02 升级 PBR Vertex / tangent 三来源规则
// 职责：把 adapter（G3）展开好的原始数组转换为 `.memesh` v2 的 PbrVertex 流：
//       RH→LH 镜像、缺失索引/法线补全、tangent 三来源决策（Provided/Fallback/
//       Generated）、AABB 与包围球重算。纯数据算法，不接触 fastgltf、不做 IO。
// v2 行为合同（02 篇）：
// - 只接受 TRIANGLES；indices 缺失 → 0..n-1；统一 uint32；每条 index < vertexCount；
// - RH→LH：position.x/normal.x 取反、每三角形交换 index[1]/[2]、UV 不翻转；
//   glTF 自带 TANGENT 时同样反射：tangent.xyz.x 取反、tangent.w 取反
//   （反射变换下保持 B = cross(N, T) * w 一致，02 篇「Tangent 规则」）；
// - NORMAL 缺失 → 按三角形拆顶点生成 flat normal；法线转换后 normalize；
// - tangent 决策（优先级）：
//     1) glTF 有 TANGENT → Provided（反射后逐顶点写入）；
//     2) 无 TANGENT 且材质无 normal map（wantsNormalMap=false）→ Fallback，
//        用稳定参考向量对 normal 正交化，w=+1；
//     3) 无 TANGENT 且 wantsNormalMap=true → Generated：顶点按 face-corner
//        展开（indices 重写为顺序三元组），tangent 先清零，由 Cooker 的
//        TangentGenerator（MikkTSpace）填充后确定性去重——不允许把 seam 两侧
//        结果写回同一源 index；
// - wantsNormalMap=true 且缺 TEXCOORD_0 → 错误（MikkTSpace 无切线投影基准）；
// - 顶点布局 = PbrVertex（48 字节，见 engine/assets/include/.../PbrVertex.h）；
// - AABB 在转换后重算；包围球 = AABB 中心 + 最远顶点距离；非有限值一律错误。
// 关联：tools/asset_cooker/src/TangentGenerator.h（Generated 路径的 MikkTSpace 调用方）
//       tools/asset_cooker/src/MeshArtifactWriter.cpp（v2 写入端）
//       docs/architecture/README.md「PBR Vertex」「Tangent 规则」
// ============================================================================

#include <MiniEngine/Assets/PbrVertex.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// 顶点契约类型来自 Assets 层；本命名空间内统一裸名使用。
using MiniEngine::Assets::PbrVertex;

struct GltfVec2 final
{
    float u{};
    float v{};
};

struct GltfVec3 final
{
    float x{};
    float y{};
    float z{};
};

// GltfTangent：glTF TANGENT accessor 展开结果（xyz 单位切线 + w 手性，右手系原值）。
struct GltfTangent final
{
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

// adapter 层交付的原始几何（glTF 右手系原值，未转换）。
struct GeometryPrimitiveInput final
{
    std::vector<GltfVec3> positions;
    std::vector<GltfVec3> normals;      // 可为空 → 生成 flat normals
    std::vector<GltfVec2> uv0;          // 可为空（wantsNormalMap 时必须非空）
    std::vector<GltfTangent> tangents;  // 可为空（glTF TANGENT accessor）
    std::vector<std::uint32_t> indices; // 可为空 → 0..n-1
};

// tangent 三来源决策的结果（写入 artifact 后由测试与诊断消费）。
enum class TangentSource : std::uint8_t
{
    // glTF 自带 TANGENT，已按 RH→LH 反射规则转换。
    Provided = 0,
    // 无 normal map，使用稳定 fallback tangent（不进 MikkTSpace）。
    Fallback = 1,
    // 无 TANGENT 且有 normal map，已由 MikkTSpace 生成并去重。
    Generated = 2
};

[[nodiscard]] const char* TangentSourceName(TangentSource source) noexcept;

// 转换产物：`.memesh` v2 直接可写的 PbrVertex 流 + 包围体 + tangent 来源。
struct EnginePrimitiveMesh final
{
    std::vector<PbrVertex> vertices;
    std::vector<std::uint32_t> indices;
    // 包围盒（引擎左手系，转换后重算）。
    float aabbMin[3]{};
    float aabbMax[3]{};
    // 包围球：中心取 AABB 中点，半径为该点到最远顶点的距离。
    float boundingSphereCenter[3]{};
    float boundingSphereRadius{};
    // 本 primitive 的 tangent 来源；Generated 时 vertices 必为 face-corner 展开。
    TangentSource tangentSource{TangentSource::Fallback};
};

enum class GeometryError
{
    Ok,
    EmptyPositions,
    CountMismatch,
    NonTriangleIndexCount,
    IndexOutOfRange,
    NonFiniteValue,
    MissingUvForNormalMap
};

[[nodiscard]] const char* GeometryErrorName(GeometryError code) noexcept;

// 失败时 out 不被写入（先全部验证，再产出）。wantsNormalMap 来自材质的
// normalTexture 是否存在——tangent 决策是几何与材质的交叉决策，由调用方传入。
[[nodiscard]] GeometryError BuildRuntimeMesh(const GeometryPrimitiveInput& input, bool wantsNormalMap,
                                             EnginePrimitiveMesh& out, std::string& error);
} // namespace MiniEngine::Tools
