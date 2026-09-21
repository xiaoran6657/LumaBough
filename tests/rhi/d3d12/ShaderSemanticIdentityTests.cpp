#include "ParityIdentity.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace
{
std::filesystem::path ShaderRoot()
{
    return std::filesystem::path{MINIENGINE_PROJECT_ROOT} / "shaders";
}
} // namespace

TEST(ShaderSemanticIdentityTests, D3D11AndD3D12ShareSemanticHash)
{
    EXPECT_EQ(MiniEngine::Samples::ShaderSemanticHash(ShaderRoot(), "d3d11"),
              MiniEngine::Samples::ShaderSemanticHash(ShaderRoot(), "d3d12"));
}

TEST(ShaderSemanticIdentityTests, BindingAndNumericChangesChangeHash)
{
    const auto source = ShaderRoot() / "d3d12";
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() / ("miniengine-parity-identity-" + std::to_string(unique));
    ASSERT_TRUE(std::filesystem::create_directories(temp / "d3d12"));
    for (const auto& entry : std::filesystem::directory_iterator(source))
        if (entry.is_regular_file())
            std::filesystem::copy_file(entry.path(), temp / "d3d12" / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing);
    const auto before = MiniEngine::Samples::ShaderSemanticHash(temp, "d3d12");
    {
        std::ofstream f(temp / "d3d12" / "PbrForward.hlsl", std::ios::app);
        f << "\nstatic const float parityIdentityProbe = 1.0;\n";
    }
    EXPECT_NE(before, MiniEngine::Samples::ShaderSemanticHash(temp, "d3d12"));
    {
        std::ofstream f(temp / "d3d12" / "BindingContract.hlsli", std::ios::app);
        f << "\n#define ME_BASE_COLOR_REGISTER t9\n";
    }
    EXPECT_NE(before, MiniEngine::Samples::ShaderSemanticHash(temp, "d3d12"));
    std::filesystem::remove_all(temp);
}
