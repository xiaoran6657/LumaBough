// ============================================================================
// AssetLoadPayload.h — decode 阶段的 typed CPU 输出（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：decode task 把 artifact 字节解码成 typed payload；render thread 在
//       上传阶段（M7-07）消费它。刻意与 AssetPool 的 payload 类型同源
//       （MeshAsset / TextureAsset / MaterialAsset）——提交进池时无需再转换。
// World 产物：M7-06 只做 hash + BakedReader 校验，字节原样交给调用方
//       （World 两遍实例化属于 engine/world，assets 不得反向依赖）。
// 关联：docs/architecture/README.md「第 3 步：decode task」
//       engine/assets/include/MiniEngine/Assets/Internal/AssetDecode.h（解码原语）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/MaterialAsset.h>
#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Assets/TextureAsset.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniEngine::Assets
{
enum class AssetPayloadKind : std::uint8_t
{
    None,     // 占位（尚未 decode）
    Mesh,     // mesh 字段有效
    Texture,  // texture 字段有效
    Material, // material 字段有效
    RawBytes, // 已校验字节（World 等不在 assets 层解码的 kind）
};

// decode 输出：三个 typed payload 之一 + （World 的）原始字节。
// 只保留一个分支有效，其余保持空（vector 为空时开销极小）。
struct CpuAssetPayload final
{
    AssetPayloadKind kind = AssetPayloadKind::None;
    MeshAsset mesh{};
    TextureAsset texture{};
    MaterialAsset material{};
    std::vector<std::byte> rawBytes{};

    // 字节背压计量：CpuReady 停车区与上传队列按它计费。
    [[nodiscard]] std::size_t ByteSize() const noexcept
    {
        std::size_t total = rawBytes.size();
        total += mesh.vertexData.size() + mesh.indexData.size();
        total += texture.pixels.size();
        // 材质是固定大小的小结构（因子 + 5 个 AssetId），不计入字节背压。
        return total;
    }
};
} // namespace MiniEngine::Assets
