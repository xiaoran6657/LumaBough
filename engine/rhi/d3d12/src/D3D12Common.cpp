// ============================================================================
// D3D12Common.cpp — HResultError 与 ThrowIfFailed 的实现
// 里程碑：M5（01 篇架构边界与输出一致性契约）
// 职责：实现 Backend-Contract §「无裸 HRESULT 静默丢弃」唯一的失败转义通道。
//       放在单一 translation unit 中而非头文件内联，是为了让大量调用点不重复
//       实例化格式化代码，并使异常文本口径在 Debug/Release 两个配置下完全一致
//       （与安全-oracle 证据有关：同一 HRESULT 在两个后端应报同一操作名）。
// 文本格式：`"<context> failed with HRESULT 0x<8 位大写十六进制>"`——
//       HRESULT 是 32 位有符号值，按 unsigned 输出可避免负号干扰 comparisons 与
//       registry 查找（0x80070057 比 -2147024809 更易查）。
// 关联：docs/architecture/README.md
// ============================================================================
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <format>

namespace MiniEngine::Rhi::D3D12
{
HResultError::HResultError(const HRESULT result, const std::string_view context)
    : std::runtime_error(std::format("{} failed with HRESULT 0x{:08X}", context, static_cast<std::uint32_t>(result))),
      m_result(result)
{
}

HRESULT HResultError::Result() const noexcept
{
    return m_result;
}

void ThrowIfFailed(const HRESULT result, const std::string_view context)
{
    if (FAILED(result))
    {
        throw HResultError(result, context);
    }
}
} // namespace MiniEngine::Rhi::D3D12
