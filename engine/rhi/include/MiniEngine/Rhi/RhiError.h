#pragma once

// M6-02 公共值契约；对象创建/图执行仍由后续 adapter 接线。
// descriptor 可按值准备；span/string_view 仅在消费调用期间借用。

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace MiniEngine::Rhi
{
enum class RhiErrorCode : std::uint8_t
{
    InvalidArgument,
    InvalidHandle,
    Unsupported,
    InvalidState,
    OutOfMemory,
    DeviceLost,
    BackendFailure
};

struct RhiError final
{
    RhiErrorCode code = RhiErrorCode::BackendFailure;
    std::string operation;
    std::string objectType;
    std::string objectName;
    std::string backend;
    std::string message;
};

// 公共失败携带引擎错误码；native 诊断仅留在后端日志。
class RhiException : public std::runtime_error
{
  public:
    explicit RhiException(RhiError error) : std::runtime_error(error.message), m_error(std::move(error))
    {
    }
    [[nodiscard]] const RhiError& Error() const noexcept
    {
        return m_error;
    }

  private:
    RhiError m_error;
};

// 后续公共验证器使用此类型；不得吞掉创建失败并返回貌似有效的 handle。
class RhiValidationError final : public RhiException
{
  public:
    using RhiException::RhiException;
};
} // namespace MiniEngine::Rhi
