// ============================================================================
// D3D12PsoKey.h — PSO 缓存键与 Pass 枚举（纯值契约，不含 D3D 类型）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 2 条）
// 职责：定义"哪些状态差异需要另一个 PSO"。M5 只需 pass × mirrored 两个维度，
//       加上两个版本号（shader/root signature）用于热重载失效。
// **DebugMode 不进入 key**：debug view 走 b0 里的常量（FrameConstants.w），
//       由 draw 时的常量决定，不需要为每个 debug view 生成 PSO（08 篇明确要求）。
// 边界：本头是纯值契约（无 D3D 类型），因此可被公共边界接受；格式表与 PSO desc
//       的构造留在 src/D3D12PsoFactory.h（内部）。
// 关联：docs/architecture/README.md（PSO key / PSO lifecycle）
//       engine/rhi/d3d12/src/D3D12PsoFactory.h（格式表与创建实现）
// ============================================================================

#pragma once

#include <compare>
#include <cstdint>

namespace MiniEngine::Rhi::D3D12
{
// Pass 种类（08 篇「PSO key」）：M5 baseline 为前四个；Ibl* 四个随 09 篇的 IBL
// bake pass 接线（键已在此冻结，避免 09 篇再改契约）。
//
// TriangleSmoke（09 篇迁移顺序第 1 步「固定 triangle」新增）：它**不是**渲染 pass
// 的入口（见 shaders/d3d12/TriangleSmoke.hlsl 的头注释），而是管线闭环的最小目标——
// 用它证明"root signature + PSO + 根 CBV + 主深度 + draw"这条链路成立。之所以
// 必须独立成一个 PassKind 而不是复用 PbrOpaque：键→shader 的映射是按 pass 查的
// （`D3D12Renderer::m_baselineShaders`），复用 pass 会让键与字节码不再一一对应。
// 取值为 8（追加在末尾）：既有取值一个不动，避免任何序列化/索引假设被打破。
enum class PassKind : std::uint8_t
{
    Shadow = 0,    // depth-only，DSV = D32_FLOAT
    PbrOpaque = 1, // forward PBR，RTV = RGBA16F + 主深度
    Skybox = 2,    // 线性 radiance 写入同一 HDR target
    ToneMap = 3,   // HDR → UNORM 后备缓冲（显式 sRGB 编码）
    EquirectToCube = 4,
    Irradiance = 5,
    Prefilter = 6,
    BrdfLut = 7,
    EnvironmentDownsample = 9, // 环境 mip 链的确定性下采样。
    TriangleSmoke = 8          // 固定三角形：RTV = 后备缓冲 UNORM + 主深度（09 篇第 1 步）
};

// 枚举项个数（= 最大值 + 1）。它存在的原因是**可维护的覆盖断言**：
// `PipelineKeyTests.PassKindNamesAreUnique` 既要求 0..kPassKindCount-1 全部有可读名且互不相同，
// 又要求 kPassKindCount 本身返回 "Unknown"——新增 pass 却忘记把计数 +1 时，
// 后一条断言会失败（否则新 pass 会静默逃出重名检查，M5-08 审查已见过一次这类"清单漏项"）。
inline constexpr std::uint8_t kPassKindCount = 10;

// 可读名（日志、PSO 调试名、metadata 共用）。
[[nodiscard]] const char* PassKindName(PassKind pass) noexcept;

// PSO 缓存键。
//
// 字段说明：
//   pass                 —— 见上（决定 formats/depth/raster/blend/输入布局）
//   mirrored             —— 镜像实例需要**另一个 rasterizer winding 的 PSO**，
//                           而不是在 draw 前动态改状态（D3D12 无动态 rasterizer
//                           state，改状态等于重建 PSO——所以必须进 key）
//   shaderRevision       —— shader 集版本（热重载后 +1）
//   rootSignatureRevision—— root signature 版本（重建后 +1）
struct PsoKey final
{
    PassKind pass = PassKind::PbrOpaque;
    bool mirrored = false;
    std::uint64_t shaderRevision = 0;
    std::uint64_t rootSignatureRevision = 0;

    auto operator<=>(const PsoKey&) const = default;
};
} // namespace MiniEngine::Rhi::D3D12
