// ============================================================================
// D3D12ConstantsTests.cpp — root CBV（b0—b3）与 pass 私有 b0 的布局契约测试
// 里程碑：M5（09 篇 迁移 M4 Pass 与输出一致性；迁移顺序第 1 步的"constant"）
// 职责：把 D3D12Constants.h 的冻结布局做成**可读的运行期断言**。编译期
//       static_assert 已经拦住"改错大小"，但它在失败时只报行号；这里的用例
//       把"哪个结构体的哪个字段在哪个偏移"写进断言消息，回归时能一眼定位。
//       另外锁定两个矩阵 helper 的语义：转置方向（HLSL mul(vector, matrix)
//       要求转置行主序）与 skybox 去平移。
// 为什么必须逐字节锁定：常量布局一旦与 HLSL cbuffer 错位，症状是"某个字段读到
//       另一个字段的值"（M4-03 实测 DebugMode 恒 0），在像素比较里只能看到
//       一个来源不明的色差——这是 M5-09 parity 最贵的一类返工。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（M4 同源布局的唯一参照）
// ============================================================================
#include "D3D12Constants.h"

#include <gtest/gtest.h>

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>

using MiniEngine::Rhi::D3D12::FrameConstants;
using MiniEngine::Rhi::D3D12::IblConstants;
using MiniEngine::Rhi::D3D12::MaterialConstants;
using MiniEngine::Rhi::D3D12::ObjectConstants;
using MiniEngine::Rhi::D3D12::PostProcessConstants;
using MiniEngine::Rhi::D3D12::SkyboxConstants;
using MiniEngine::Rhi::D3D12::TransposedForHlsl;
using MiniEngine::Rhi::D3D12::ViewProjectionWithoutTranslation;

namespace
{
// 与 D3D11 侧完全一致的期望尺寸（M4 冻结值）；改成别的值就等于破坏 parity。
constexpr std::size_t kFrameBytes = 128U;
constexpr std::size_t kObjectBytes = 208U;
constexpr std::size_t kMaterialBytes = 48U;
constexpr std::size_t kIblBytes = 16U;
constexpr std::size_t kSkyboxBytes = 64U;
constexpr std::size_t kPostProcessBytes = 16U;

// 造一个非对称矩阵：转置与自身不同，任何"忘了转置"的实现都会被抓出来。
MiniEngine::World::Matrix4 MakeAsymmetricMatrix()
{
    MiniEngine::World::Matrix4 matrix;
    for (std::size_t index = 0; index < matrix.values.size(); ++index)
    {
        matrix.values[index] = static_cast<float>(index + 1U); // 1..16，逐元素可辨
    }
    return matrix;
}
} // namespace

TEST(D3D12ConstantsTests, RootConstantBufferLayoutsAreFrozen)
{
    EXPECT_EQ(sizeof(FrameConstants), kFrameBytes) << "b0 与 PbrForward.hlsl 的 FrameConstants 必须同为 128B";
    EXPECT_EQ(sizeof(ObjectConstants), kObjectBytes) << "b1 必须与 ShadowDepth.hlsl 的声明逐字节一致";
    EXPECT_EQ(sizeof(MaterialConstants), kMaterialBytes) << "b2 是 02 篇冻结的 48B 契约";
    EXPECT_EQ(sizeof(IblConstants), kIblBytes) << "b3 是单个 float4";
    EXPECT_EQ(sizeof(SkyboxConstants), kSkyboxBytes) << "skybox 的 b0 只有一个 4x4";
    EXPECT_EQ(sizeof(PostProcessConstants), kPostProcessBytes) << "tone map 的 b0 是 16B";

    // 每个结构体都必须是 16 字节对齐：D3D12 的 CBV 以 256B 为基址、按 16B 步进，
    // 非 16B 对齐的结构体放进常量缓冲会让后续字段整体错位。
    EXPECT_EQ(alignof(FrameConstants), 16U);
    EXPECT_EQ(alignof(ObjectConstants), 16U);
    EXPECT_EQ(alignof(MaterialConstants), 16U);
    EXPECT_EQ(alignof(IblConstants), 16U);
    EXPECT_EQ(alignof(SkyboxConstants), 16U);
    EXPECT_EQ(alignof(PostProcessConstants), 16U);
}

TEST(D3D12ConstantsTests, FrameAndObjectFieldOffsetsMatchHlslPacking)
{
    // 本用例锁定的是**CPU 侧**的字段偏移（float4-only 打包的产物），把"哪个字段在
    // 哪个偏移"写进断言消息，回归时能一眼定位。
    // 注意边界（审查三-3）：断言 CPU 偏移**不等于**证明"与 HLSL 一致"——那只证明
    // CPU 结构体没变。真正的两侧对账在 D3D12CbufferDriftTests：它解析
    // shaders/d3d12/*.hlsl 的 cbuffer 声明，逐字段核对数量/顺序/类型/偏移。
    EXPECT_EQ(offsetof(FrameConstants, viewProjection), 0U);
    EXPECT_EQ(offsetof(FrameConstants, cameraPositionAndDebugMode), 64U);
    EXPECT_EQ(offsetof(FrameConstants, directionAndIntensity), 80U);
    EXPECT_EQ(offsetof(FrameConstants, lightColorAndPadding), 96U);
    EXPECT_EQ(offsetof(FrameConstants, shadowMapSizeAndPadding), 112U);

    EXPECT_EQ(offsetof(ObjectConstants, world), 0U);
    EXPECT_EQ(offsetof(ObjectConstants, normalMatrix), 64U);
    EXPECT_EQ(offsetof(ObjectConstants, lightWorldViewProjection), 128U);
    EXPECT_EQ(offsetof(ObjectConstants, handednessAndReceivesShadow), 192U);

    // b2 三行：与 02 篇「Material constants」契约的 16B 步进一致（审查小问题③补齐）。
    EXPECT_EQ(offsetof(MaterialConstants, baseColorFactor), 0U);
    EXPECT_EQ(offsetof(MaterialConstants, emissiveAndMetallic), 16U);
    EXPECT_EQ(offsetof(MaterialConstants, roughnessNormalOcclusionFlags), 32U);

    EXPECT_EQ(offsetof(PostProcessConstants, exposureEv), 0U);
    EXPECT_EQ(offsetof(PostProcessConstants, debugHdr), 4U);
    EXPECT_EQ(offsetof(PostProcessConstants, inverseOutputSize), 8U);
}

TEST(D3D12ConstantsTests, SingleFieldConstantRowsStartAtZero)
{
    // 三个"只有一行常量"的结构体（b3 Ibl、skybox b0、tone map b0）都必须让唯一字段
    // 落在偏移 0——任何前置 padding 都会让整行错位（审查小问题③补齐）。
    EXPECT_EQ(offsetof(IblConstants, prefilterMipCountAndFlags), 0U);
    EXPECT_EQ(offsetof(SkyboxConstants, viewProjectionWithoutTranslation), 0U);

    // 反向哨兵：这些结构体确实只有一个字段，sizeof 就等于该字段大小（16B / 64B）。
    // 若有人偷偷塞进第二个字段，上面"偏移 0"仍会通过，但这里会失败。
    EXPECT_EQ(sizeof(IblConstants) - offsetof(IblConstants, prefilterMipCountAndFlags), sizeof(DirectX::XMFLOAT4));
    EXPECT_EQ(sizeof(SkyboxConstants) - offsetof(SkyboxConstants, viewProjectionWithoutTranslation),
              sizeof(DirectX::XMFLOAT4X4));
}

TEST(D3D12ConstantsTests, TransposeFlipsRowAndColumnMajor)
{
    const MiniEngine::World::Matrix4 matrix = MakeAsymmetricMatrix();
    const DirectX::XMFLOAT4X4 transposed = TransposedForHlsl(matrix);

    // XMFLOAT4X4 是 row-major：transposed.m[row][col] 必须等于 values[col * 4 + row]。
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            EXPECT_FLOAT_EQ(transposed.m[row][column], matrix.values[column * 4U + row])
                << "row=" << row << " column=" << column << "：转置方向写反会让所有矩阵变换失效";
        }
    }

    // 非对称输入下"转置 == 原样"必须为假，否则上面的循环可能是恒真断言。
    EXPECT_NE(transposed.m[0][1], matrix.values[1]);
}

TEST(D3D12ConstantsTests, SkyboxViewProjectionDropsCameraTranslation)
{
    MiniEngine::World::Matrix4 view;
    view.values = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                   0.0F, 0.0F, 1.0F, 0.0F, 7.0F, 8.0F, 9.0F, 1.0F}; // 平移在最后一 row
    MiniEngine::World::Matrix4 identity;
    identity.values = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

    const DirectX::XMFLOAT4X4 result = ViewProjectionWithoutTranslation(view, identity);

    // 平移被清零 + 3x3 为单位 + projection 为单位 → 结果必为单位矩阵（转置后仍单位）。
    for (std::size_t row = 0; row < 4U; ++row)
    {
        for (std::size_t column = 0; column < 4U; ++column)
        {
            const float expected = (row == column) ? 1.0F : 0.0F;
            EXPECT_FLOAT_EQ(result.m[row][column], expected)
                << "row=" << row << " column=" << column << "：平移未被清零会让天空随相机移动产生视差";
        }
    }
}
