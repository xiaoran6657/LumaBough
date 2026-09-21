// ============================================================================
// WicImage.h — WIC（Windows 图像处理组件）解码入口（PNG/JPEG → RGBA8）
// 里程碑：M3（03 篇 WIC 阶段）
// 职责：把 Cooker 读入的编码图像字节解码为固定规格的 RGBA8（单帧、自上而下、
//       不 premultiply / 不 resize / 不生成 mip），并执行尺寸与像素预算校验。
//       这是运行时之外唯一接触图像解码的模块。
// 关联：docs/architecture/README.md（WIC 阶段）
//       tools/asset_cooker/src/TextureArtifactWriter.h（解码结果的写入方）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniEngine::Tools
{
// 03 篇 WIC 阶段的解码结果：RGBA8，top-to-bottom，不 premultiply / resize / mip。
struct DecodedImage final
{
    // 宽度（像素）；解码端保证非零且不超过 Recipe 预算。
    std::uint32_t width{};
    // 高度（像素）；同上。
    std::uint32_t height{};
    // 像素数据，长度恒为 width * height * 4 字节。
    std::vector<std::byte> rgba8;
};

// PNG/JPEG（mime 由编码字节前几字节自动识别）→ RGBA8。encoded 字节来源不限：
// 外部文件 / Data URI / GLB bufferView 已由 GltfImportAdapter 统一读入 embeddedBytes。
// 返回 false 时 error 描述拒绝原因（含 frame/dimension/budget 校验）。
[[nodiscard]] bool DecodeImageBytes(const std::vector<std::byte>& encoded, DecodedImage& out, std::string& error);
} // namespace MiniEngine::Tools
