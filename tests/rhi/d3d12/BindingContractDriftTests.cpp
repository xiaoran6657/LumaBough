// ============================================================================
// BindingContractDriftTests.cpp — HLSL 契约文件与 C++ 契约头的漂移检查
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；审查意见 M5-05 P2-4）
// 职责：`shaders/d3d12/BindingContract.hlsli` 是寄存器契约的 HLSL 侧镜像，
//       但 DXC 编译只能证明"宏能编译"，**抓不住寄存器漂移**（例如
//       ME_SHADOW_REGISTER 被改成 t7 而 C++ 侧仍是 t8，编译照样通过）。
//       本用例逐条解析 `.hlsli` 里的 `#define ME_*_REGISTER <bN|tN|sN>`，与
//       D3D12RootBindings.h 的枚举值直接比对——任何一侧单独改动都会失败。
// 定位方式：与 D3D11 的 HLSL 门禁一致，由测试目标注入 MINIENGINE_SHADER_DIR。
// 关联：docs/architecture/README.md（绑定契约）
//       tests/rhi/d3d11/HlslCompileGateTests.cpp（同款目录注入手法）
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12RootBindings.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#ifndef MINIENGINE_SHADER_DIR
#error "MINIENGINE_SHADER_DIR must be defined by the test target (see tests/rhi/d3d12/CMakeLists.txt)"
#endif

namespace D12 = MiniEngine::Rhi::D3D12;

namespace
{
// 读取 .hlsli 并解析 `#define ME_XXX_REGISTER <reg>`，返回宏名 → 寄存器号。
// 只接受 b<t>/t<n>/s<n> 三种形式；解析失败（缺宏/格式异常）视为测试失败的前置。
std::unordered_map<std::string, std::uint32_t> ParseHlslRegisters()
{
    const std::string path = std::string{MINIENGINE_SHADER_DIR} + "/BindingContract.hlsli";
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << "无法打开 " << path;

    std::unordered_map<std::string, std::uint32_t> registers;
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream stream{line};
        std::string defineToken;
        std::string name;
        std::string registerToken;
        stream >> defineToken >> name >> registerToken;
        if (defineToken != "#define" || name.rfind("ME_", 0) != 0 || registerToken.empty())
        {
            continue;
        }
        // 形如 b2 / t8 / s1：首字符是类别，其余是十进制寄存器号。
        const char category = registerToken.front();
        if (category != 'b' && category != 't' && category != 's')
        {
            continue;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(std::stoul(registerToken.substr(1)));
        registers.emplace(name, index);
    }
    return registers;
}
} // namespace

// 常量缓冲（b）与 C++ 枚举逐条一致。
TEST(BindingContractDriftTests, ConstantBufferMacrosMatchCppContract)
{
    const auto registers = ParseHlslRegisters();
    EXPECT_EQ(registers.at("ME_FRAME_CB_REGISTER"), D12::Register(D12::ConstantBufferRegister::Frame));
    EXPECT_EQ(registers.at("ME_OBJECT_CB_REGISTER"), D12::Register(D12::ConstantBufferRegister::Object));
    EXPECT_EQ(registers.at("ME_MATERIAL_CB_REGISTER"), D12::Register(D12::ConstantBufferRegister::Material));
    EXPECT_EQ(registers.at("ME_IBL_CB_REGISTER"), D12::Register(D12::ConstantBufferRegister::Ibl));
}

// 材质 SRV（t0—t4）与表起点逐条一致。
TEST(BindingContractDriftTests, MaterialSrvMacrosMatchCppContract)
{
    const auto registers = ParseHlslRegisters();
    EXPECT_EQ(registers.at("ME_BASE_COLOR_REGISTER"), D12::Register(D12::MaterialSrvRegister::BaseColor));
    EXPECT_EQ(registers.at("ME_NORMAL_REGISTER"), D12::Register(D12::MaterialSrvRegister::Normal));
    EXPECT_EQ(registers.at("ME_METALLIC_ROUGHNESS_REGISTER"),
              D12::Register(D12::MaterialSrvRegister::MetallicRoughness));
    EXPECT_EQ(registers.at("ME_OCCLUSION_REGISTER"), D12::Register(D12::MaterialSrvRegister::Occlusion));
    EXPECT_EQ(registers.at("ME_EMISSIVE_REGISTER"), D12::Register(D12::MaterialSrvRegister::Emissive));
}

// 全局 SRV（t5—t8）与 C++ 枚举逐条一致——这是最容易被"顺移一格"的地方。
TEST(BindingContractDriftTests, GlobalSrvMacrosMatchCppContract)
{
    const auto registers = ParseHlslRegisters();
    EXPECT_EQ(registers.at("ME_IRRADIANCE_REGISTER"), D12::Register(D12::GlobalSrvRegister::DiffuseIrradiance));
    EXPECT_EQ(registers.at("ME_PREFILTER_REGISTER"), D12::Register(D12::GlobalSrvRegister::PrefilteredSpecular));
    EXPECT_EQ(registers.at("ME_BRDF_LUT_REGISTER"), D12::Register(D12::GlobalSrvRegister::BrdfLut));
    EXPECT_EQ(registers.at("ME_SHADOW_REGISTER"), D12::Register(D12::GlobalSrvRegister::ShadowMap));
}

// static sampler（s0—s2）与 C++ 枚举逐条一致。
TEST(BindingContractDriftTests, SamplerMacrosMatchCppContract)
{
    const auto registers = ParseHlslRegisters();
    EXPECT_EQ(registers.at("ME_MATERIAL_SAMPLER_REGISTER"), D12::Register(D12::SamplerRegister::Material));
    EXPECT_EQ(registers.at("ME_IBL_SAMPLER_REGISTER"), D12::Register(D12::SamplerRegister::IblAndPost));
    EXPECT_EQ(registers.at("ME_SHADOW_SAMPLER_REGISTER"), D12::Register(D12::SamplerRegister::ShadowComparison));
}

// 宏集合本身也要对齐：漏定义或多定义都会让 shader 侧与 C++ 契约分叉。
TEST(BindingContractDriftTests, MacroSetIsCompleteAndUnambiguous)
{
    const auto registers = ParseHlslRegisters();
    // 16 个宏 = 4 个 b（b0—b3）+ 9 个 t（t0—t8）+ 3 个 s（s0—s2）。
    EXPECT_EQ(registers.size(), 16U) << "宏集合多一个或少一个都意味着两侧契约分叉";

    // 表内连续性：材质表 5 个、全局表 4 个，且起点等于 C++ 侧常量。
    EXPECT_EQ(registers.at("ME_BASE_COLOR_REGISTER"), D12::kMaterialSrvBaseRegister);
    EXPECT_EQ(registers.at("ME_IRRADIANCE_REGISTER"), D12::kGlobalSrvBaseRegister);
    EXPECT_EQ(registers.at("ME_EMISSIVE_REGISTER") + 1U, registers.at("ME_IRRADIANCE_REGISTER"))
        << "材质表最后一个槽位必须紧邻全局表起点（t4 → t5）";
}
