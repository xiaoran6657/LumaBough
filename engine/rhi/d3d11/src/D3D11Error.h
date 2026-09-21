// ============================================================================
// D3D11Error.h — D3D11 HRESULT 处理与资源命名辅助
// 里程碑：M2
// 职责：提供统一的 HRESULT 失败抛出 helper（ThrowIfFailed）与为 D3D11 资源设置
//   调试名称的 helper（SetDebugObjectName）。所有返回 HRESULT 的关键 D3D 调用都应
//   经 ThrowIfFailed 处理，避免在 Release 构建中吞掉失败。
// 关联：docs/architecture/README.md
// ============================================================================
#pragma once

#include <d3d11.h>

#include <string_view>

namespace MiniEngine
{
// 若 HRESULT 表示失败则抛出 std::runtime_error，文本含 operation 与十六进制错误码。
//
// 参数：
//   result    —— 待检查的 HRESULT
//   operation —— 失败时写入错误信息的操作名（如 "ID3D11Device::CreateBuffer"）
// 失败：result 失败时抛 std::runtime_error；成功时直接返回。
void ThrowIfFailed(HRESULT result, std::string_view operation);

// 为 D3D11 设备子对象设置 Debug 对象名（WKPDID_D3DDebugObjectName）。
//
// 名称会被 D3D11 Debug Layer 的 live-object 报告与 RenderDoc 资源检查器读取，
// 便于在捕获中定位资源；名称须在资源创建后立即设置。空名称或空对象直接忽略。
//
// 参数：
//   object —— 目标 ID3D11DeviceChild 对象（可为空）
//   name   —— 可读名称，长度按字节传入
// 失败：SetPrivateData 失败时经 ThrowIfFailed 抛 std::runtime_error。
void SetDebugObjectName(ID3D11DeviceChild* object, std::string_view name);
} // namespace MiniEngine
