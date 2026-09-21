// ============================================================================
// D3D11Error.cpp — HRESULT 失败抛出与 D3D11 资源命名实现
// 里程碑：M2
// 职责：实现 ThrowIfFailed 与 SetDebugObjectName 两个 helper，统一错误文本格式
//   （操作名 + 十六进制 HRESULT）并借助 Debug 对象名提升 live-object 报告与
//   RenderDoc 的可读性。
// 关联：docs/architecture/README.md
// ============================================================================
#include "D3D11Error.h"

#include <d3dcommon.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace MiniEngine
{
// 统一检查 HRESULT：成功静默返回，失败抛出带操作名与十六进制错误码的运行时异常。
void ThrowIfFailed(const HRESULT result, const std::string_view operation)
{
    // 成功则直接返回，避免为常见路径引入开销。
    if (SUCCEEDED(result))
    {
        return;
    }

    // 失败时把操作名与固定宽度、大写、十六进制的 HRESULT 拼进异常文本，便于定位。
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << static_cast<unsigned long>(result);
    throw std::runtime_error{stream.str()};
}

// 为 D3D11 设备子对象设置 Debug 对象名；空对象或空名称直接忽略。
void SetDebugObjectName(ID3D11DeviceChild* object, const std::string_view name)
{
    // 空对象或空名称没有命名意义，直接忽略。
    if (object == nullptr || name.empty())
    {
        return;
    }

    // WKPDID_D3DDebugObjectName 来自 d3dcommon.h（经 dxguid 链接），长度按字节传入。
    ThrowIfFailed(object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data()),
                  "ID3D11DeviceChild::SetPrivateData");
}
} // namespace MiniEngine
