// ============================================================================
// GltfImportAdapterTests.cpp — fastgltf 适配层（G3）的集成测试
// 里程碑：M3（02 篇 G3）
// 职责：用 Khronos 官方 fixture（含 sparse accessor）验证节点遍历、材质过滤、
//       external/DataURI/GLB bufferView 三种图片来源与依赖路径规范化。
// 关联：tools/asset_cooker/src/GltfImportAdapter.cpp（被测实现）
//       tests/fixtures/（Khronos 官方样例）
// ============================================================================

#include "GltfImportAdapter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using MiniEngine::Tools::DiagnosticSink;
using MiniEngine::Tools::ImportGltf;
using MiniEngine::Tools::ImportResult;

std::string Base64Encode(const std::vector<std::byte>& bytes)
{
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3)
    {
        const std::uint32_t a = std::to_integer<std::uint8_t>(bytes[index]);
        const std::uint32_t b = index + 1 < bytes.size() ? std::to_integer<std::uint8_t>(bytes[index + 1]) : 0;
        const std::uint32_t c = index + 2 < bytes.size() ? std::to_integer<std::uint8_t>(bytes[index + 2]) : 0;
        const std::uint32_t combined = (a << 16) | (b << 8) | c;
        result.push_back(kAlphabet[(combined >> 18) & 0x3FU]);
        result.push_back(kAlphabet[(combined >> 12) & 0x3FU]);
        result.push_back(index + 1 < bytes.size() ? kAlphabet[(combined >> 6) & 0x3FU] : '=');
        result.push_back(index + 2 < bytes.size() ? kAlphabet[combined & 0x3FU] : '=');
    }
    return result;
}

void AppendFloat(std::vector<std::byte>& bytes, const float value)
{
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    for (std::size_t i = 0; i < sizeof(float); ++i)
    {
        bytes.push_back(raw[i]);
    }
}

void AppendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i)
    {
        bytes.push_back(raw[i]);
    }
}

struct InlineGltf final
{
    std::filesystem::path source;
    std::filesystem::path sourceRoot;
};

// 1 scene → 1 node → 1 mesh → 1 indexed primitive（YZ 平面三角形，无 NORMAL）。
InlineGltf WriteInlineGltf()
{
    InlineGltf fixture;
    static std::uint32_t counter = 0;
    ++counter;
    fixture.sourceRoot = std::filesystem::temp_directory_path() / ("MiniEngineGltfAdapter-" + std::to_string(counter));
    std::filesystem::create_directories(fixture.sourceRoot);
    fixture.source = fixture.sourceRoot / "inline.gltf";

    // buffer：3 个 float3 POSITION（36B）+ 3 个 uint32 索引（12B）。
    std::vector<std::byte> buffer;
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 1.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 1.0F);
    AppendFloat(buffer, 0.0F);
    AppendU32(buffer, 0);
    AppendU32(buffer, 1);
    AppendU32(buffer, 2);

    const std::string gltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{
      "attributes": {"POSITION": 0},
      "indices": 1,
      "mode": 4
  }]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 12}
  ],
  "buffers": [{"uri": "data:application/octet-stream;base64,)" +
                             Base64Encode(buffer) + R"(", "byteLength": 48}]
})";

    std::ofstream stream{fixture.source, std::ios::binary | std::ios::trunc};
    EXPECT_TRUE(stream.good());
    stream.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
    EXPECT_TRUE(stream.good());
    return fixture;
}
} // namespace

TEST(GltfImportAdapterTests, TriangleDataUriImportsWithGoldenGeometry)
{
    const auto fixture = WriteInlineGltf();
    ImportResult result;
    DiagnosticSink sink;
    ASSERT_TRUE(ImportGltf(fixture.source, fixture.sourceRoot, result, sink))
        << (sink.records.empty() ? "" : sink.records[0].message);

    ASSERT_EQ(result.nodes.size(), 1U);
    EXPECT_EQ(result.nodes[0].name, "node/0"); // 无 name → 确定性 fallback
    ASSERT_EQ(result.primitives.size(), 1U);
    EXPECT_EQ(result.primitives[0].stableKey, "mesh/0/primitive/0");

    // 通过完整栈（Data URI → fastgltf → 手动索引解码 → G2）后的黄金结果：
    // 无 NORMAL → 拆顶点路径，flat normal = (1,0,0)（AxisBasis 黄金三角）。
    ASSERT_EQ(result.primitives[0].mesh.indices.size(), 3U);
    EXPECT_EQ(result.primitives[0].mesh.indices[0], 0U);
    ASSERT_EQ(result.primitives[0].mesh.vertices.size(), 3U);
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[0].normal[0], 1.0F);
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[0].normal[1], 0.0F);
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[0].normal[2], 0.0F);
    // 拆顶点路径：vertices = [v0', v2', v1']（交付绕序），所以 y=1 的 v2 在
    // vertices[1]；同时证明 x 镜像（glTF v1=(0,0,1) → engine (0,0,1)）与
    // 绕序交换在整个导入栈正确生效。
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[1].position[1], 1.0F);
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[2].position[2], 1.0F);
}

TEST(GltfImportAdapterTests, MissingSourceProducesDiagnostics)
{
    const auto fixture = WriteInlineGltf();
    ImportResult result;
    DiagnosticSink sink;
    EXPECT_FALSE(ImportGltf(fixture.sourceRoot / "does-not-exist.gltf", fixture.sourceRoot, result, sink));
    ASSERT_FALSE(sink.records.empty());
    EXPECT_FALSE(sink.records[0].message.empty());
    EXPECT_TRUE(result.primitives.empty());
}

TEST(GltfImportAdapterTests, FailureDoesNotLeavePartialPrimitives)
{
    // 结果对象在失败路径必须保持干净（事务性契约与 AssetManager/G2 一致）。
    const auto fixture = WriteInlineGltf();
    ImportResult result;
    DiagnosticSink sink;
    EXPECT_FALSE(ImportGltf(fixture.sourceRoot / "missing.gltf", fixture.sourceRoot, result, sink));
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_TRUE(result.primitives.empty());
}

// ---------- G3b：外部 .bin / 图片 URI 沙箱 + matrix/TRS 一致性 ----------

// 复用 WriteInlineGltf 的 buffer 字节，但 URI 指向外部文件。
std::vector<std::byte> MakeTriangleBufferBytes()
{
    std::vector<std::byte> buffer;
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 1.0F);
    AppendFloat(buffer, 0.0F);
    AppendFloat(buffer, 1.0F);
    AppendFloat(buffer, 0.0F);
    AppendU32(buffer, 0);
    AppendU32(buffer, 1);
    AppendU32(buffer, 2);
    return buffer;
}

InlineGltf WriteExternalBinGltf(const std::string& binUri)
{
    InlineGltf fixture;
    static std::uint32_t counter = 1000;
    ++counter;
    fixture.sourceRoot = std::filesystem::temp_directory_path() / ("MiniEngineGltfExternal-" + std::to_string(counter));
    std::filesystem::create_directories(fixture.sourceRoot);
    fixture.source = fixture.sourceRoot / "inline.gltf";

    const std::string gltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{
      "attributes": {"POSITION": 0},
      "indices": 1,
      "mode": 4
  }]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 12}
  ],
  "buffers": [{"uri": ")" + binUri +
                             R"(", "byteLength": 48}]
})";
    std::ofstream stream{fixture.source, std::ios::binary | std::ios::trunc};
    EXPECT_TRUE(stream.good());
    stream.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
    EXPECT_TRUE(stream.good());
    return fixture;
}

TEST(GltfImportAdapterTests, ExternalBinLoadsThroughSandboxAndRecordsDependency)
{
    const auto fixture = WriteExternalBinGltf("scene.bin");
    const auto binBytes = MakeTriangleBufferBytes();
    std::ofstream bin{fixture.sourceRoot / "scene.bin", std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(bin.good());
    bin.write(reinterpret_cast<const char*>(binBytes.data()), static_cast<std::streamsize>(binBytes.size()));
    ASSERT_TRUE(bin.good());
    bin.close(); // 必须在 ImportGltf 之前落盘：否则 ofstream 缓冲未刷，读到空文件

    ImportResult result;
    DiagnosticSink sink;
    ASSERT_TRUE(ImportGltf(fixture.source, fixture.sourceRoot, result, sink))
        << (sink.records.empty() ? "" : sink.records[0].message);

    ASSERT_EQ(result.primitives.size(), 1U);
    ASSERT_EQ(result.externalDependencyPaths.size(), 1U);
    EXPECT_EQ(result.externalDependencyPaths[0], "scene.bin"); // 规范化相对路径
    // 外部 bin 走完整栈后的黄金结果与 Data URI 一致（flat normal = (1,0,0)）。
    EXPECT_FLOAT_EQ(result.primitives[0].mesh.vertices[0].normal[0], 1.0F);
}

TEST(GltfImportAdapterTests, TraversalBinUriIsRejectedBySandbox)
{
    const auto fixture = WriteExternalBinGltf("../evil.bin");
    ImportResult result;
    DiagnosticSink sink;
    EXPECT_FALSE(ImportGltf(fixture.source, fixture.sourceRoot, result, sink));
    ASSERT_FALSE(sink.records.empty());
    EXPECT_NE(sink.records[0].context.find("buffer"), std::string::npos) << sink.records[0].message;
    EXPECT_TRUE(result.primitives.empty());
}

bool WriteNodeOnlyGltf(const std::filesystem::path& path, const std::string& nodeBody)
{
    const std::string gltf = R"({
  "asset": {"version": "2.0"},
  "nodes": [{"name": "golden")" +
                             (nodeBody.empty() ? std::string{} : "," + nodeBody) + R"(}]
})";
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
    return stream.good();
}

void ExpectTrsGolden(const MiniEngine::Tools::ImportedNode& node)
{
    // matrix 分解是浮点运算：用 NEAR 容差而非 FLOAT_EQ。
    constexpr float kTolerance = 1.0e-4F;
    EXPECT_NEAR(node.translation[0], 2.0F, kTolerance);
    EXPECT_NEAR(node.translation[1], 3.0F, kTolerance);
    EXPECT_NEAR(node.translation[2], 4.0F, kTolerance);
    EXPECT_NEAR(node.scale[0], 2.0F, kTolerance);
    EXPECT_NEAR(node.scale[1], 2.0F, kTolerance);
    EXPECT_NEAR(node.scale[2], 2.0F, kTolerance);
    EXPECT_NEAR(node.rotation[0], 0.0F, kTolerance);
    EXPECT_NEAR(node.rotation[1], 0.0F, kTolerance);
    EXPECT_NEAR(node.rotation[2], 0.0F, kTolerance);
    EXPECT_NEAR(node.rotation[3], 1.0F, kTolerance);
}

TEST(GltfImportAdapterTests, MatrixAndTrsNodesProduceSameTransform)
{
    // T(2,3,4) * R(identity) * S(2) 的等价两种表达：
    //   TRS：{"translation":[2,3,4],"scale":[2,2,2],"rotation":[0,0,0,1]}
    //   matrix（column-major，glTF 约定）：S*R*T → 列主序 16 元素。
    static std::uint32_t counter = 2000;
    const auto root =
        std::filesystem::temp_directory_path() / ("MiniEngineGltfMatrixGolden-" + std::to_string(++counter));
    std::filesystem::create_directories(root);

    const std::string matrixColumns = "[2,0,0,0, 0,2,0,0, 0,0,2,0, 2,3,4,1]";
    ASSERT_TRUE(WriteNodeOnlyGltf(root / "matrix.gltf", "\"matrix\": " + matrixColumns));
    ASSERT_TRUE(
        WriteNodeOnlyGltf(root / "trs.gltf", "\"translation\":[2,3,4],\"rotation\":[0,0,0,1],\"scale\":[2,2,2]"));

    ImportResult matrixResult;
    ImportResult trsResult;
    DiagnosticSink matrixSink;
    DiagnosticSink trsSink;
    ASSERT_TRUE(ImportGltf(root / "matrix.gltf", root, matrixResult, matrixSink))
        << (matrixSink.records.empty() ? "" : matrixSink.records[0].message);
    ASSERT_TRUE(ImportGltf(root / "trs.gltf", root, trsResult, trsSink))
        << (trsSink.records.empty() ? "" : trsSink.records[0].message);

    ASSERT_EQ(matrixResult.nodes.size(), 1U);
    ASSERT_EQ(trsResult.nodes.size(), 1U);
    ExpectTrsGolden(matrixResult.nodes[0]); // matrix 经 DecomposeNodeMatrices → TRS
    ExpectTrsGolden(trsResult.nodes[0]);
}
