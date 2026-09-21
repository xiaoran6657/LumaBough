// ============================================================================
// D3D12Common.h — D3D12 后端共用的对齐与错误处理基元
// 里程碑：M5（01 篇架构边界与输出一致性契约；手抄清单第 1 条）
// 职责：集中 D3D12/DXGI 头文件包含、Microsoft::WRL::ComPtr 别名、HRESULT 失败
//       异常（HResultError）与按 2 的幂向上对齐的 AlignUp。后续 D3D12 模块
//       （Device/Queue/Descriptor/UploadRing/PSO）一律经本头进入后端实现，
//       避免每个文件各写一份错误转义逻辑；它是 M5 唯一的错误处理入口。
// 对齐约定：AlignUp 只接受 2 的幂，且明确检查上溢——D3D12 的 CBV 256 字节对齐、
//       纹理上传 footprint 对齐与 Upload Ring 偏移都依赖此函数，静默回绕会让
//       越界写在很远的别的 Subresource 上才暴露。
// 边界约定：本头文件出现在 include/ 下即意味着它属于 D3D12 后端边界内部——
//       Core/Assets/World 与 RenderPacket 不得包含本头，也不得出现 ID3D12* 类型。
// 关联：docs/architecture/README.md（手抄清单第 1 条）
//       docs/architecture/DECISIONS.md
// ============================================================================

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

// d3d12.h 与 dxgi1_6.h 同时汇集 HRESULT 定义与后续各类 D3D12/DXGI 接口声明；
// 只有本后端边界内部允许出现它们（见 01 篇「Composition 而非 RHI」）。
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace MiniEngine::Rhi::D3D12
{
// D3D12 对象生命周期一律走 COM 引用计数 RAII；裸 AddRef/Release 不出现在业务代码里。
using Microsoft::WRL::ComPtr;

// D3D12 调用失败异常：保留原始 HRESULT 与失败点上下文串，供上层决定是致命失败
// 还是降级处理（例如 Tier 能力探测失败 ≠ 致命）。
class HResultError final : public std::runtime_error
{
  public:
    // 构造带上下文的错误。
    //
    // 参数：
    //   result   —— 失败的 HRESULT（原值保留，不做翻译）
    //   context  —— 失败接口/操作名（如 "ID3D12Device::CreateCommittedResource"）
    HResultError(HRESULT result, std::string_view context);

    // 取原始 HRESULT。
    //
    // 返回：构造时传入的 HRESULT。
    [[nodiscard]] HRESULT Result() const noexcept;

  private:
    HRESULT m_result;
};

// 检查 HRESULT，失败时抛出 HResultError。
//
// 参数：
//   result  —— 待检查的返回值
//   context —— 失败时写入异常文本的操作名；必须能唯一定位到调用点
// 失败：result 为 FAILED 时抛 MiniEngine::Rhi::D3D12::HResultError。
void ThrowIfFailed(HRESULT result, std::string_view context);

// 按 2 的幂向上对齐（CBV 偏移、Subresource footprint、Upload Ring 指针必备）。
//
// 参数：
//   value     —— 待对齐的值（字节）
//   alignment —— 对齐粒度（字节），必须是非零的 2 的幂
// 返回：value 向上取整到 alignment 的倍数。
// 失败：alignment 不是 2 的幂时抛 std::invalid_argument；结果超出 uint64 表示
//       范围时抛 std::overflow_error（溢出曾是 D3D12 Mum Driver race 之外的
//       高频静默事故来源，这里必须是显式失败而非回绕）。
constexpr std::uint64_t AlignUp(const std::uint64_t value, const std::uint64_t alignment)
{
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
    {
        throw std::invalid_argument{"AlignUp alignment must be a non-zero power of two"};
    }
    if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1U))
    {
        throw std::overflow_error{"AlignUp result exceeds uint64 range"};
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}
} // namespace MiniEngine::Rhi::D3D12
