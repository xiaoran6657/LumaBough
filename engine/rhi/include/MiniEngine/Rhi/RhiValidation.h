#pragma once

#include <MiniEngine/Rhi/RhiCapabilities.h>
#include <MiniEngine/Rhi/RhiDescriptors.h>
#include <MiniEngine/Rhi/RhiError.h>

#include <cstdint>
#include <optional>
#include <string>

#include <cstddef>
#include <span>
namespace MiniEngine::Rhi
{
// 纯 CPU 验证。InvalidArgument 为错误描述；Unsupported 为能力缺失，调用方记录 BLOCKED。
// 返回前不修改 descriptor/capabilities，不降级格式或关闭必需效果。
void ValidateBufferDesc(const BufferDesc& desc);
// InitialData 为空表示稍后上传；非空数据必须完整落在 buffer 内，且 readback buffer 不接受初始内容。
void ValidateBufferInitialData(const BufferDesc& desc, std::span<const std::byte> initialData);
// Upload 的 bytes 必须非空且完整落在 [offset, offset + bytes.size())；GpuToCpu 永不接受上传。
void ValidateBufferUpload(const BufferDesc& desc, std::uint64_t offset, std::span<const std::byte> bytes);
// subresources 按 layer-major、每层 mip-major 提供完整 arrayLayers * mipLevels 项。
// rowPitch/slicePitch 是源数据步长；最后一行只读取 rowBytes。bytes 只需覆盖实际足印，
// 因而允许 slicePitch 大于 bytes.size()（尾部 padding 不必放入 span）。
void ValidateTextureUpload(const TextureDesc& desc, std::span<const TextureSubresourceData> subresources);
void ValidateTextureDesc(const TextureDesc& desc);
void ValidateTextureDesc(const TextureDesc& desc, const RhiCapabilities& capabilities);
void ValidateSamplerDesc(const SamplerDesc& desc, const RhiCapabilities& capabilities);
// 固定 M4/M5 profile 所需资源/格式与计时；不以它替代实际 backend query。
void RequireM6Capabilities(const RhiCapabilities& capabilities);
enum class RhiCapabilityStatus : std::uint8_t
{
    Ready,
    Blocked
};
struct RhiCapabilityAssessment final
{
    RhiCapabilityStatus status = RhiCapabilityStatus::Blocked;
    std::optional<RhiError> error;
};
// 启动门禁的显式结论；缺少必需能力为 Blocked，不静默 fallback。
RhiCapabilityAssessment AssessM6Capabilities(const RhiCapabilities& capabilities);

// schema v1 逐字段语义身份；debugName 不参与，float 的 -0 归一为 +0。
// 非有限 sampler 数值不能进入 semantic key；诊断 JSON 仍能显示其非法值。
std::uint64_t SemanticHash(const BufferDesc& desc);
std::uint64_t SemanticHash(const TextureDesc& desc);
std::uint64_t SemanticHash(const SamplerDesc& desc);
bool SemanticallyEqual(const BufferDesc& left, const BufferDesc& right);
bool SemanticallyEqual(const TextureDesc& left, const TextureDesc& right);
bool SemanticallyEqual(const SamplerDesc& left, const SamplerDesc& right);
std::string ToDiagnosticJson(const BufferDesc& desc);
std::string ToDiagnosticJson(const TextureDesc& desc);
std::string ToDiagnosticJson(const SamplerDesc& desc);
} // namespace MiniEngine::Rhi
