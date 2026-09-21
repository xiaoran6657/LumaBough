// ============================================================================
// GltfGeometryTests.cpp — glTF 几何转换核心（G2）的 golden 与负向测试
// 里程碑：M3（02 篇 G2）
// 职责：锁定 RH→LH 镜像、绕序交换、flat normals 生成、索引补全、AABB/包围球
//       与非有限值拒绝等行为；纯数据算法，可无 glTF 依赖直接构造输入。
// 关联：tools/asset_cooker/src/GltfGeometry.cpp（被测实现）
//       docs/architecture/README.md 第 6/7 节
// ============================================================================

#include "GltfGeometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

namespace
{
using MiniEngine::Tools::GeometryError;
using MiniEngine::Tools::GeometryPrimitiveInput;
using MiniEngine::Tools::GltfTangent;
using MiniEngine::Tools::GltfVec2;
using MiniEngine::Tools::GltfVec3;

GeometryPrimitiveInput MakeIndexedTriangle()
{
    GeometryPrimitiveInput input;
    input.positions = {GltfVec3{0.0F, 0.0F, 0.0F}, GltfVec3{0.0F, 0.0F, 1.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.indices = {0, 1, 2};
    return input;
}
} // namespace

TEST(GltfGeometryTests, PositionMirrorMatchesAxisBasisGolden)
{
    // glTF -X 是"右"，MiniEngine +X 是"右"：glTF (1,2,3) → engine (-1,2,3)；
    // glTF 右方向的点 (-1,0,0) → engine (1,0,0)；glTF forward +Z 保持 +Z。
    // 提供 NORMAL 走 indexed 路径：vertices 与 glTF 顶点 1:1 对应，断言最直白。
    GeometryPrimitiveInput input;
    input.positions = {GltfVec3{1.0F, 2.0F, 3.0F}, GltfVec3{-1.0F, 0.0F, 0.0F}, GltfVec3{0.0F, 0.0F, 1.0F}};
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.indices = {0, 1, 2};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    EXPECT_FLOAT_EQ(mesh.vertices[0].position[0], -1.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].position[1], 2.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].position[2], 3.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[1].position[0], 1.0F); // glTF 的"右"落在 engine +X
    EXPECT_FLOAT_EQ(mesh.vertices[2].position[2], 1.0F); // glTF 的"前"保持在 engine +Z
}

TEST(GltfGeometryTests, WindingSwapIsAppliedPerTriangle)
{
    // 提供 NORMAL → indexed 路径：index[1]/index[2] 交换 [0,1,2] → [0,2,1]。
    auto input = MakeIndexedTriangle();
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    ASSERT_EQ(mesh.indices.size(), 3U);
    EXPECT_EQ(mesh.indices[0], 0U);
    EXPECT_EQ(mesh.indices[1], 2U);
    EXPECT_EQ(mesh.indices[2], 1U);
}

TEST(GltfGeometryTests, MissingNormalsGenerateFlatNormalsWithSplitVertices)
{
    // glTF 三角形 (0,0,0),(0,0,1),(0,1,0)：RH 几何法线 cross = (-1,0,0)。
    // 转换 + 绕序交换后的 flat normal 应为 (1,0,0)——同时证明 x 镜像与
    // 绕序交换一致（只做其一都会得到相反结果）。
    auto input = MakeIndexedTriangle();
    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    ASSERT_EQ(mesh.vertices.size(), 3U); // 拆顶点：3 索引 → 3 顶点
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[0], 1.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[2], 0.0F);
    for (const auto& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] +
                                       vertex.normal[2] * vertex.normal[2]);
        EXPECT_FLOAT_EQ(length, 1.0F);
    }

    // 拆分路径的"绕序交换"体现在顶点顺序：delivered[1] 是转换后的原 v2，
    // delivered[2] 是转换后的原 v1。
    EXPECT_FLOAT_EQ(mesh.vertices[1].position[1], 1.0F); // 原 v2 的 y
    EXPECT_FLOAT_EQ(mesh.vertices[2].position[1], 0.0F); // 原 v1 的 y
    EXPECT_FLOAT_EQ(mesh.vertices[2].position[2], 1.0F); // 原 v1 的 z
}

TEST(GltfGeometryTests, ProvidedNormalsAreMirroredAndNormalized)
{
    GeometryPrimitiveInput input = MakeIndexedTriangle();
    // 非单位法线且 x 非零：(-3,0,4)（长度 5）→ 镜像 (3,0,4) → normalize (0.6,0,0.8)。
    input.normals = {GltfVec3{-3.0F, 0.0F, 4.0F}, GltfVec3{-3.0F, 0.0F, 4.0F}, GltfVec3{-3.0F, 0.0F, 4.0F}};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    ASSERT_EQ(mesh.vertices.size(), 3U); // 有 NORMAL：不拆顶点
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[0], 0.6F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[2], 0.8F);
}

TEST(GltfGeometryTests, UvPassesThroughWithoutFlip)
{
    GeometryPrimitiveInput input = MakeIndexedTriangle();
    input.uv0 = {GltfVec2{0.0F, 0.0F}, GltfVec2{1.0F, 0.0F}, GltfVec2{0.0F, 1.0F}};
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    // UV 不做任何翻转：(0,0) 保持 (0,0)（首行对应图片顶部）。
    EXPECT_FLOAT_EQ(mesh.vertices[0].uv0[0], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].uv0[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[1].uv0[0], 1.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[2].uv0[1], 1.0F);
}

TEST(GltfGeometryTests, SequentialIndicesGeneratedWhenMissing)
{
    GeometryPrimitiveInput input = MakeIndexedTriangle();
    input.indices.clear();

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);
    ASSERT_EQ(mesh.indices.size(), 3U);
    EXPECT_EQ(mesh.indices[0], 0U);
    EXPECT_EQ(mesh.indices[1], 1U);
    EXPECT_EQ(mesh.indices[2], 2U);
}

TEST(GltfGeometryTests, AabbAndBoundingSphereAreComputedAfterConversion)
{
    GeometryPrimitiveInput input;
    input.positions = {GltfVec3{1.0F, 0.0F, 0.0F}, GltfVec3{2.0F, 1.0F, 0.0F}, GltfVec3{1.5F, 0.5F, 2.0F}};
    input.indices = {0, 1, 2};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    // 转换后的 x：-1、-2、-1.5 → AABB x = [-2, -1]。
    EXPECT_FLOAT_EQ(mesh.aabbMin[0], -2.0F);
    EXPECT_FLOAT_EQ(mesh.aabbMax[0], -1.0F);
    EXPECT_FLOAT_EQ(mesh.aabbMin[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh.aabbMax[1], 1.0F);
    EXPECT_FLOAT_EQ(mesh.aabbMin[2], 0.0F);
    EXPECT_FLOAT_EQ(mesh.aabbMax[2], 2.0F);
    EXPECT_FLOAT_EQ(mesh.boundingSphereCenter[0], -1.5F);
    EXPECT_FLOAT_EQ(mesh.boundingSphereCenter[1], 0.5F);
    EXPECT_FLOAT_EQ(mesh.boundingSphereCenter[2], 1.0F);
    // 半径 = 所有顶点到中心的最大距离（独立重算，不挑单个顶点）。
    float expectedRadius = 0.0F;
    for (const auto& vertex : mesh.vertices)
    {
        const float dx = vertex.position[0] - mesh.boundingSphereCenter[0];
        const float dy = vertex.position[1] - mesh.boundingSphereCenter[1];
        const float dz = vertex.position[2] - mesh.boundingSphereCenter[2];
        expectedRadius = std::fmax(expectedRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    EXPECT_FLOAT_EQ(mesh.boundingSphereRadius, expectedRadius);
    EXPECT_GT(mesh.boundingSphereRadius, 0.0F);
}

TEST(GltfGeometryTests, RejectsIndexOutOfRangeAndNonTriangleCountAndNonFinite)
{
    GeometryPrimitiveInput outOfRange = MakeIndexedTriangle();
    outOfRange.indices = {0, 1, 3}; // vertexCount == 3
    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(outOfRange, false, mesh, error), GeometryError::IndexOutOfRange);

    GeometryPrimitiveInput nonTriangle = MakeIndexedTriangle();
    nonTriangle.indices = {0, 1, 2, 0};
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(nonTriangle, false, mesh, error),
              GeometryError::NonTriangleIndexCount);

    GeometryPrimitiveInput nonFinite = MakeIndexedTriangle();
    nonFinite.positions[1].y = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(nonFinite, false, mesh, error), GeometryError::NonFiniteValue);

    GeometryPrimitiveInput countMismatch = MakeIndexedTriangle();
    countMismatch.normals = {GltfVec3{0.0F, 1.0F, 0.0F}};
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(countMismatch, false, mesh, error), GeometryError::CountMismatch);

    GeometryPrimitiveInput empty;
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(empty, false, mesh, error), GeometryError::EmptyPositions);
}

TEST(GltfGeometryTests, FailedBuildDoesNotWriteOutput)
{
    GeometryPrimitiveInput input = MakeIndexedTriangle();
    input.indices = {0, 1, 9};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    mesh.vertices.resize(7); // 预置数据：失败时必须原样保留
    std::string error;
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::IndexOutOfRange);
    EXPECT_EQ(mesh.vertices.size(), 7U); // out 未被写入
}

// ---------- M4-02 v2：tangent 三来源规则 ----------

TEST(GltfGeometryTests, ProvidedTangentIsReflectedForLeftHanded)
{
    // glTF 自带 TANGENT（Provided）：RH→LH 反射必须同时取反 tangent.xyz.x 与
    // tangent.w——反射变换下保持 B = cross(N, T) * w 指向同一几何切向
    // （02 篇「Tangent 规则」，与 position/normal 的 x 镜像同一契约）。
    GeometryPrimitiveInput input;
    input.positions = {GltfVec3{0.0F, 0.0F, 0.0F}, GltfVec3{0.0F, 0.0F, 1.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.tangents = {GltfTangent{1.0F, 0.0F, 0.0F, 1.0F}, GltfTangent{1.0F, 0.0F, 0.0F, 1.0F},
                      GltfTangent{1.0F, 0.0F, 0.0F, 1.0F}};
    // normal-mapped primitive 需要 UV 作为切线空间投影基准（G2 的统一前置校验）。
    input.uv0 = {GltfVec2{0.0F, 0.0F}, GltfVec2{1.0F, 0.0F}, GltfVec2{0.0F, 1.0F}};
    input.indices = {0, 1, 2};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, true, mesh, error), GeometryError::Ok);

    EXPECT_EQ(mesh.tangentSource, MiniEngine::Tools::TangentSource::Provided);
    EXPECT_FLOAT_EQ(mesh.vertices[0].tangent[0], -1.0F); // x 取反
    EXPECT_FLOAT_EQ(mesh.vertices[0].tangent[1], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].tangent[2], 0.0F);
    EXPECT_FLOAT_EQ(mesh.vertices[0].tangent[3], -1.0F); // w 取反
}

TEST(GltfGeometryTests, FallbackTangentIsOrthogonalWithoutNormalMap)
{
    // 无 TANGENT 且材质无 normal map（wantsNormalMap=false）→ Fallback：稳定参考
    // 向量对 normal 正交化，w=+1，单位长度，不进 MikkTSpace。
    auto input = MakeIndexedTriangle();
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, false, mesh, error), GeometryError::Ok);

    EXPECT_EQ(mesh.tangentSource, MiniEngine::Tools::TangentSource::Fallback);
    for (const auto& vertex : mesh.vertices)
    {
        const float tx = vertex.tangent[0];
        const float ty = vertex.tangent[1];
        const float tz = vertex.tangent[2];
        EXPECT_FLOAT_EQ(std::sqrt(tx * tx + ty * ty + tz * tz), 1.0F);
        EXPECT_NEAR(tx * vertex.normal[0] + ty * vertex.normal[1] + tz * vertex.normal[2], 0.0F, 1.0e-6F);
        EXPECT_FLOAT_EQ(vertex.tangent[3], 1.0F);
    }
}

TEST(GltfGeometryTests, MissingUvForNormalMapIsRejected)
{
    // 无 TANGENT 且 wantsNormalMap=true 但缺 TEXCOORD_0：MikkTSpace 无切线投影
    // 基准 → 明确错误，不静默用 fallback tangent 画错法线。
    auto input = MakeIndexedTriangle();
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    mesh.vertices.resize(4); // 预置数据：失败时必须原样保留
    std::string error;
    EXPECT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, true, mesh, error), GeometryError::MissingUvForNormalMap);
    EXPECT_EQ(mesh.vertices.size(), 4U);
}

TEST(GltfGeometryTests, GeneratedSourceExpandsFaceCornersAndZeroesTangents)
{
    // 无 TANGENT 且 wantsNormalMap=true 且有 UV → Generated：顶点按 face-corner
    // 展开（indices 重写为顺序三元组），tangent 清零待 MikkTSpace 填充；
    // seam 两侧结果不允许写回同一源 index（去重由 TangentGenerator 负责）。
    GeometryPrimitiveInput input;
    input.positions = {GltfVec3{0.0F, 0.0F, 0.0F}, GltfVec3{0.0F, 0.0F, 1.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.normals = {GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}, GltfVec3{0.0F, 1.0F, 0.0F}};
    input.uv0 = {GltfVec2{0.0F, 0.0F}, GltfVec2{1.0F, 0.0F}, GltfVec2{0.0F, 1.0F}};
    input.indices = {0, 2, 1};

    MiniEngine::Tools::EnginePrimitiveMesh mesh;
    std::string error;
    ASSERT_EQ(MiniEngine::Tools::BuildRuntimeMesh(input, true, mesh, error), GeometryError::Ok);

    EXPECT_EQ(mesh.tangentSource, MiniEngine::Tools::TangentSource::Generated);
    ASSERT_EQ(mesh.indices.size(), 3U);
    EXPECT_EQ(mesh.indices[0], 0U);
    EXPECT_EQ(mesh.indices[1], 1U);
    EXPECT_EQ(mesh.indices[2], 2U);
    ASSERT_EQ(mesh.vertices.size(), mesh.indices.size()); // face-corner 展开不变式
    for (const auto& vertex : mesh.vertices)
    {
        // Generated 路径：tangent.xyz 清零待 MikkTSpace 填充；w 保持 +1 占位
        // （手性符号域为 ±1，Mikk 输出按 face-corner 写 xyz 后去重）。
        EXPECT_FLOAT_EQ(vertex.tangent[0], 0.0F);
        EXPECT_FLOAT_EQ(vertex.tangent[1], 0.0F);
        EXPECT_FLOAT_EQ(vertex.tangent[2], 0.0F);
        EXPECT_FLOAT_EQ(vertex.tangent[3], 1.0F);
    }
}
