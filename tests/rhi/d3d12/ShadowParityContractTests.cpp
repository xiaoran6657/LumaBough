// ============================================================================
// ShadowParityContractTests.cpp — 两后端 shadow caster/receiver 契约回归
// ============================================================================
#include <MiniEngine/World/RenderQueueBuilder.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace
{
std::string Read(const std::filesystem::path& p)
{
    std::ifstream f(p);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
std::filesystem::path Root()
{
    return std::filesystem::path{MINIENGINE_PROJECT_ROOT};
}
std::string Shader(const char* backend, const char* name)
{
    return Read(Root() / "shaders" / backend / name);
}
} // namespace

TEST(ShadowParityContractTests, BothBackendsTransformCasterAndReceiverThroughWorld)
{
    constexpr const char* kExpr = "mul(mul(float4(input.position, 1.0), World), LightWorldViewProjection)";
    for (const char* backend : {"d3d11", "d3d12"})
    {
        EXPECT_NE(Shader(backend, "ShadowDepth.hlsl").find(kExpr), std::string::npos) << backend;
        EXPECT_NE(Shader(backend, "PbrForward.hlsl").find(kExpr), std::string::npos) << backend;
    }
}

TEST(ShadowParityContractTests, D3D11ShadowPassOwnsDepthStateBeforeDraw)
{
    const std::string source = Read(Root() / "engine/rhi/d3d11/src/D3D11Renderer.cpp");
    const auto begin = source.find("Pass 1/5");
    const auto draw = source.find("DrawIndexed", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    const auto depth = source.find("OMSetDepthStencilState(m_depthStencilState.Get(), 0)", begin);
    ASSERT_NE(depth, std::string::npos);
    EXPECT_LT(depth, draw);
}
