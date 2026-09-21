// ============================================================================
// D3D11ImageComparison.h — 视觉回归比较器（纯 CPU，无 D3D 类型）
// 里程碑：M4（07 篇「先测重复性，再定阈值」「Diff 输出」）
// 职责：PNG 解码（统一转 RGBA8）、归一化 RGB 指标（MAE/RMSE/p99/max/changedRate，
//       sampleCount = width*height*3）与截图元数据配置的 INCOMPARABLE 判定。
//       capture 侧（staging ring + WIC 原子写）在内部头 D3D11Screenshot.h；
//       比较器放公共头是因为 Sandbox 的 --compare 模式是它的第二个消费方。
// 关联：docs/architecture/README.md
//       tests/rendering/ImageComparisonTests.cpp（数学与拒绝语义的单元测试）
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace MiniEngine::Rhi::D3D11
{
// 07 篇「先测重复性，再定阈值」的指标族。sampleCount = width*height*3（RGB 通道
// sample，不是像素数）；误差归一化到 [0,1]（abs(candidate-golden)/255）。
struct ImageDiffMetrics final
{
    double mae{};
    double rmse{};
    double p99{};
    double maxError{};
    double changedRate{};
    std::uint64_t sampleCount{};
    std::uint32_t worstX{}; // maxError 像素坐标（诊断用）
    std::uint32_t worstY{};
};

// 比较两块等长 RGBA8 像素缓冲（alpha 忽略，契约固定为 1）。
// 失败（空、长度不等、非 4 倍数）抛 std::invalid_argument——必须在计算分位数前失败。
// changedThreshold 是"该通道算变化"的绝对 0..255 阈值（07 篇示例 3）。
[[nodiscard]] ImageDiffMetrics CompareRgba8(std::span<const std::uint8_t> golden,
                                            std::span<const std::uint8_t> candidate, std::uint8_t changedThreshold);

// 截图元数据中"必须一致才算可比"的配置键（07 篇「元数据文件」：配置不同报
// INCOMPARABLE，不能给出像素 PASS）。
struct ScreenshotConfigIdentity final
{
    std::uint32_t schemaVersion{}; // 元数据 schema 版本：演进时旧 golden 必须被拒绝比较
    std::string scene{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t fixedTick{};
    std::string assetManifestSha256{};
    std::string environmentArtifactSha256{};
    std::string iblProfile{}; // baseline
    double exposureEv{};      // 固定曝光
    std::string toneMapper{}; // reinhard
};

// 返回空字符串 = 可比较；否则返回第一个不一致的配置键描述（INCOMPARABLE 原因）。
[[nodiscard]] std::string MetadataIncomparableReason(const ScreenshotConfigIdentity& golden,
                                                     const ScreenshotConfigIdentity& candidate);

// 解码 PNG 到 RGBA8 紧密排列缓冲。失败（不可读/非 PNG/位深不支持）返回 false。
bool DecodePngToRgba8(const std::filesystem::path& path, std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                      std::uint32_t& height, std::string& error);

// 比较两个 PNG 文件（先解码，再走 CompareRgba8；尺寸不同返回 false 并写 error）。
bool ComparePngFiles(const std::filesystem::path& golden, const std::filesystem::path& candidate,
                     std::uint8_t changedThreshold, ImageDiffMetrics& metrics, std::string& error);

// 把 |candidate-golden| × scale 写成诊断 PNG（07 篇 diff-abs-x8）。两输入须同尺寸。
bool WriteAbsDiffPng(std::span<const std::uint8_t> golden, std::span<const std::uint8_t> candidate, std::uint32_t width,
                     std::uint32_t height, float scale, const std::filesystem::path& target, std::string& error);
} // namespace MiniEngine::Rhi::D3D11
