// ============================================================================
// CaptureScopes.h — GPU capture 作用域（RenderDoc / PIX）
// 里程碑：M6-10（RenderDocScope/PixScope 原始实现）→ M7-12（抽成共享头，M6/M7 两个运行器复用）
// 职责：在**单帧**范围内启动/结束 GPU 捕获，用于"固定帧 capture + 可读性"证据（M7-A25/A26）。
// 设计：不向高层暴露 native device（Start/End 用 (nullptr, nullptr) 组合根）；
//       捕获失败一律抛异常——"没有 capture 文件"不能被当成通过。
// 依赖：RenderDoc 需要由 `renderdoccmd capture` 启动（renderdoc.dll 已注入），
//       PIX 需要由 `pixtool.exe` 启动（PIXIsAttachedForGpuCapture 为真）。
// 关联：samples/rhi_sandbox/M6SceneRunner.cpp、samples/rhi_sandbox/M7SceneRunner.cpp
// ============================================================================

#pragma once

#include <MiniEngine/Core/Assert.h>

// 顺序硬约束：Windows.h 必须在 pix3.h / renderdoc_app.h 之前（它们引用 PCWSTR 等 Win32 类型）。
#include <Windows.h>

#include <pix3.h>

#ifdef M610_HAS_RENDERDOC
#include <renderdoc_app.h>
#endif

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace MiniEngine::Sandbox
{
namespace fs = std::filesystem;

// RenderDoc：末帧捕获（要求进程由 renderdoccmd capture 启动）。
class RenderDocScope final
{
#ifdef M610_HAS_RENDERDOC
    RENDERDOC_API_1_6_0* m_api = nullptr;
    fs::path m_output;
    bool m_active = false;
    std::uint32_t m_before = 0;
#endif
  public:
    explicit RenderDocScope(const fs::path& output)
    {
#ifdef M610_HAS_RENDERDOC
        if (fs::exists(output))
            throw std::runtime_error("RenderDoc capture output already exists");
        const auto module = GetModuleHandleW(L"renderdoc.dll");
        const auto getApi =
            module ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
        if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&m_api)))
            throw std::runtime_error("RenderDoc capture requires renderdoccmd capture launch");
        if (m_api->IsFrameCapturing())
            throw std::runtime_error("another RenderDoc capture is active");
        m_api->MaskOverlayBits(0, 0);
        m_output = output;
        fs::create_directories(output.parent_path());
        m_api->SetCaptureFilePathTemplate((output.parent_path() / "programmatic").string().c_str());
        m_before = m_api->GetNumCaptures();
        // 单一 device/window 的组合根；NULL wildcard 不向高层 pass 暴露 native device。
        m_api->StartFrameCapture(nullptr, nullptr);
        m_active = m_api->IsFrameCapturing() != 0;
        if (!m_active)
            throw std::runtime_error("RenderDoc failed to start fixed-frame capture");
#else
        (void)output;
        throw std::runtime_error("this build has no RenderDoc application API header");
#endif
    }
    ~RenderDocScope()
    {
#ifdef M610_HAS_RENDERDOC
        if (m_active)
            (void)m_api->DiscardFrameCapture(nullptr, nullptr);
#endif
    }
    void End()
    {
#ifdef M610_HAS_RENDERDOC
        m_active = false;
        if (!m_api->EndFrameCapture(nullptr, nullptr) || m_api->GetNumCaptures() != m_before + 1)
            throw std::runtime_error("RenderDoc fixed-frame capture failed");
        std::uint32_t length = 0;
        if (!m_api->GetCapture(m_before, nullptr, &length, nullptr) || length == 0)
            throw std::runtime_error("RenderDoc capture path missing");
        std::vector<char> path(length + 1, 0);
        if (!m_api->GetCapture(m_before, path.data(), &length, nullptr))
            throw std::runtime_error("RenderDoc capture path unavailable");
        fs::copy_file(fs::path(path.data()), m_output);
#endif
    }
};

// PIX：末帧捕获（要求进程由 pixtool.exe 启动）。
class PixScope final
{
    bool m_active = false;

  public:
    explicit PixScope(const fs::path& path)
    {
        if (path.empty())
            return;
        if (!PIXIsAttachedForGpuCapture())
            throw std::runtime_error("PIX capture requires pixtool launch");
        PIXCaptureParameters parameters{};
        parameters.GpuCaptureParameters.FileName = path.c_str();
        if (FAILED(PIXBeginCapture(PIX_CAPTURE_GPU, &parameters)))
            throw std::runtime_error("PIXBeginCapture failed");
        m_active = true;
    }
    ~PixScope()
    {
        if (m_active)
            (void)PIXEndCapture(FALSE);
    }
    void End()
    {
        if (m_active)
        {
            m_active = false;
            if (FAILED(PIXEndCapture(FALSE)))
                throw std::runtime_error("PIXEndCapture failed");
        }
    }
};
} // namespace MiniEngine::Sandbox
