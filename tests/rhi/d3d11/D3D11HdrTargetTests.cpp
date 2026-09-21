// ============================================================================
// D3D11HdrTargetTests.cpp — M4-05 HDR target 与后处理常量的契约测试
// 里程碑：M4（05 篇「HDR target」「Post constants」）
// 职责：在不需要 GPU 的前提下锁定 05 篇冻结的两组契约：
//   1. scene color 描述：格式 `R16G16B16A16_FLOAT`、MipLevels=1、ArraySize=1、
//      Sample.Count=1、Usage=DEFAULT、BindFlags=RENDER_TARGET|SHADER_RESOURCE
//      （05 篇「HDR target」表格；M4 固定无 MSAA）。
//   2. `PostProcessConstants` 的 16B 布局与字节序（与 shaders/d3d11/ToneMap.hlsl
//      的 b0 逐字段对应）——字段重排/大小变化必须在此失败，而不是在 RenderDoc 里
//      靠"某个像素看起来不对"才发现。
// 说明：Create() 的设备路径需要真实 D3D11 设备，不在本篇单元测试范围内；
//       其行为由 Renderer 的创建期 ThrowIfFailed 与 Debug Layer 覆盖。
// 关联：docs/architecture/README.md「自动化测试」
//       engine/rhi/d3d11/include/MiniEngine/Rhi/D3D11/D3D11HdrTarget.h
// ============================================================================

#include <MiniEngine/Rhi/D3D11/D3D11HdrTarget.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace
{
// 把 PostProcessConstants 的 16 字节按小端读出，用于布局级断言（不依赖设备）。
std::array<std::uint8_t, 16> Serialize(const MiniEngine::Rhi::D3D11::PostProcessConstants& constants)
{
    std::array<std::uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), &constants, sizeof(constants));
    return bytes;
}
} // namespace

TEST(D3D11HdrTargetTests, SceneColorDescMatchesFrozenContract)
{
    using MiniEngine::Rhi::D3D11::MakeHdrSceneColorDesc;

    // 05 篇冻结：RGBA16F、单 mip、单 array slice、无 MSAA、DEFAULT、RTV+SRV 双绑定。
    const D3D11_TEXTURE2D_DESC description = MakeHdrSceneColorDesc(1280, 720);

    EXPECT_EQ(description.Width, 1280U);
    EXPECT_EQ(description.Height, 720U);
    EXPECT_EQ(description.Format, DXGI_FORMAT_R16G16B16A16_FLOAT) << "05 篇冻结的 scene color 格式";
    EXPECT_EQ(description.MipLevels, 1U) << "无 mip：tone map 只采 LOD0";
    EXPECT_EQ(description.ArraySize, 1U);
    EXPECT_EQ(description.SampleDesc.Count, 1U) << "M4 固定无 MSAA（避免 Resolve/screenshot 分支）";
    EXPECT_EQ(description.SampleDesc.Quality, 0U);
    EXPECT_EQ(description.Usage, D3D11_USAGE_DEFAULT);
    EXPECT_EQ(description.BindFlags, static_cast<UINT>(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        << "同一个资源既要当 RTV 写、又要当 SRV 读（hazard 由 pass 边界管理）";
    EXPECT_EQ(description.CPUAccessFlags, 0U);
    EXPECT_EQ(description.MiscFlags, 0U);

    // 描述构造入口必须与类内常量同源，避免"两处格式各写一份"的漂移。
    EXPECT_EQ(description.Format, MiniEngine::Rhi::D3D11::kHdrSceneColorFormat);
}

TEST(D3D11HdrTargetTests, PostProcessConstantsHasFrozenLayout)
{
    using MiniEngine::Rhi::D3D11::PostProcessConstants;

    // 大小/对齐：必须恰好一个常量缓冲行（16B），否则 HLSL 侧会读到错位字段。
    EXPECT_EQ(sizeof(PostProcessConstants), 16U);
    EXPECT_EQ(alignof(PostProcessConstants), 16U);
    EXPECT_TRUE(std::is_trivially_copyable_v<PostProcessConstants>) << "常量缓冲按字节 memcpy 上传";

    // 字段偏移：与 ToneMap.hlsl 的 float / uint / float2 声明顺序一致。
    EXPECT_EQ(offsetof(PostProcessConstants, exposureEv), 0U);
    EXPECT_EQ(offsetof(PostProcessConstants, debugHdr), 4U);
    EXPECT_EQ(offsetof(PostProcessConstants, inverseOutputSize), 8U);

    // 字节序锚点：exposureEv=0.5、debugHdr=1、inverseOutputSize={0.25,0.5}
    // 的 16 字节内容固定；字段被重排或插入 padding 时本用例失败。
    PostProcessConstants constants{};
    constants.exposureEv = 0.5F;
    constants.debugHdr = 1U;
    constants.inverseOutputSize[0] = 0.25F;
    constants.inverseOutputSize[1] = 0.5F;

    const std::array<std::uint8_t, 16> bytes = Serialize(constants);
    float exposureEv = 0.0F;
    std::uint32_t debugHdr = 0U;
    float inverseWidth = 0.0F;
    float inverseHeight = 0.0F;
    std::memcpy(&exposureEv, bytes.data() + 0, sizeof(exposureEv));
    std::memcpy(&debugHdr, bytes.data() + 4, sizeof(debugHdr));
    std::memcpy(&inverseWidth, bytes.data() + 8, sizeof(inverseWidth));
    std::memcpy(&inverseHeight, bytes.data() + 12, sizeof(inverseHeight));

    EXPECT_FLOAT_EQ(exposureEv, 0.5F);
    EXPECT_EQ(debugHdr, 1U) << "debugHdr 是 uint 语义位，不能被当作 float 重解释";
    EXPECT_FLOAT_EQ(inverseWidth, 0.25F);
    EXPECT_FLOAT_EQ(inverseHeight, 0.5F);
}
