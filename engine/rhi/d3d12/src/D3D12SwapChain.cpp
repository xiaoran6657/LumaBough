// ============================================================================
// D3D12SwapChain.cpp — 交换链创建、Present 与安全点 Resize 实现
// 里程碑：M5（04 篇 Command List、SwapChain 与 Resize；手抄清单第 1/2 条）
// 职责：实现 D3D12SwapChain.h。三个关键契约：
//   1) current index 只来自 IDXGISwapChain3::GetCurrentBackBufferIndex；
//   2) back buffer 状态每帧 PRESENT→RENDER_TARGET→PRESENT，Present 前断言；
//   3) resize 只在 FlushGpu 之后一次性重建，0×0 走挂起而不碰 ResizeBuffers。
// 关联：docs/architecture/README.md（Resize 八步）
//       Microsoft Learn：D3D12 Swap Chains
// ============================================================================
#include "D3D12SwapChain.h"

#include "D3D12Diagnostics.h"
#include "D3D12Queue.h"
#include "D3D12ResourceStateTracker.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include <Windows.h>

#include <cstdio>
#include <format>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
std::uint32_t ResolvePresentFlags(const bool vsync, const bool tearingEnabled, const bool isWindowed)
{
    // 只有"允许 tearing 的交换链 + vsync=0 + 窗口化"三者同时成立才传 tearing flag。
    // 独占全屏下 DXGI 会拒绝该 flag，传了会让 Present 失败而不是"更流畅"。
    if (tearingEnabled && !vsync && isWindowed)
    {
        return DXGI_PRESENT_ALLOW_TEARING;
    }
    return 0U;
}

bool IsSuspendedSize(const std::uint32_t width, const std::uint32_t height)
{
    return width == 0U || height == 0U;
}

bool ResolveTearingEnabled(const bool tearingSupported, const bool vsync, const std::uint32_t swapChainFlags)
{
    const bool flagPresent = (swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0U;
    return tearingSupported && flagPresent && !vsync;
}

void D3D12SwapChain::Initialize(ID3D12Device& device, IDXGIFactory7& factory, ID3D12CommandQueue& queue,
                                D3D12ResourceStateTracker& tracker, void* nativeWindow, const std::uint32_t width,
                                const std::uint32_t height, const bool vsync)
{
    m_tracker = &tracker;
    if (nativeWindow == nullptr)
    {
        throw std::invalid_argument{"D3D12SwapChain::Initialize requires a valid window handle"};
    }
    if (IsSuspendedSize(width, height))
    {
        // 创建期 0×0 是非法请求：挂起语义只在运行期（窗口被最小化）出现。
        throw std::invalid_argument{"D3D12SwapChain::Initialize requires non-zero size"};
    }

    m_device = &device;
    m_config.width = width;
    m_config.height = height;
    m_config.bufferCount = kSwapChainBufferCount;
    m_config.vsync = vsync;
    m_config.resizeCount = 0;

    // 可选 tearing（04 篇四步）：先查驱动能力，再决定创建标志。
    BOOL allowTearing = FALSE;
    const HRESULT tearingHr =
        factory.CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    m_config.tearingSupported = SUCCEEDED(tearingHr) && allowTearing == TRUE;
    m_config.flags = (m_config.tearingSupported && !vsync) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;
    // 创建即用同一判定（此处 flags 已定，等价于 supported && !vsync）。
    m_config.tearingEnabled = ResolveTearingEnabled(m_config.tearingSupported, vsync, m_config.flags);

    // baseline：R8G8B8A8_UNORM + 三缓冲 + FLIP_DISCARD + 无 MSAA + RENDER_TARGET_OUTPUT。
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = kSwapChainFormat;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kSwapChainBufferCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = m_config.flags;

    // device 参数传 Direct Queue（04 篇明确要求）；fullscreen 与 restrictToOutput 置空。
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory.CreateSwapChainForHwnd(&queue, static_cast<HWND>(nativeWindow), &description, nullptr,
                                                 nullptr, &swapChain1),
                  "IDXGIFactory7::CreateSwapChainForHwnd");
    ThrowIfFailed(swapChain1.As(&m_swapChain), "IDXGISwapChain1 -> IDXGISwapChain3");

    // 禁止 Alt+Enter 独占全屏（M1/M2 窗口契约的延续）。
    ThrowIfFailed(factory.MakeWindowAssociation(static_cast<HWND>(nativeWindow), DXGI_MWA_NO_ALT_ENTER),
                  "IDXGIFactory::MakeWindowAssociation");

    // CPU-only RTV heap：三个连续 descriptor，Resize 时原位重写（flush 已证明无引用）。
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.NumDescriptors = kSwapChainBufferCount;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device.CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_rtvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(RTV)");
    m_rtvDescriptorSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    Internal::SetDxgiDebugName(m_swapChain.Get(), "M5.SwapChain");
    Internal::SetDebugName(m_rtvHeap.Get(), L"M5.RtvHeap");

    CreateBackBufferViews(device);

    // 07 篇：把 back buffer 以**实际初始状态**（新建 buffer 即 COMMON，其数值 = PRESENT）
    // 注册进 tracker——状态真源从此只有一处。
    RegisterBackBuffers();
    m_suspended = false;
    m_failed = false;
}

void D3D12SwapChain::RegisterBackBuffers()
{
    for (std::uint32_t index = 0; index < kSwapChainBufferCount; ++index)
    {
        const std::wstring name = std::format(L"M5.BackBuffer[{}]", index);
        m_backBufferKeys[index] = m_tracker->Register(*m_backBuffers[index].Get(), 1U, D3D12_RESOURCE_STATE_PRESENT,
                                                      name.c_str(), m_generation);
    }
}

// GetBuffer(i) → CreateRenderTargetView 写入第 i 个 RTV slot，并命名 M5.BackBuffer[i]。
void D3D12SwapChain::CreateBackBufferViews(ID3D12Device& device)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kSwapChainBufferCount; ++index)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index])), "IDXGISwapChain3::GetBuffer");
        device.CreateRenderTargetView(m_backBuffers[index].Get(), nullptr, handle);
        Internal::SetDebugName(m_backBuffers[index].Get(), std::format(L"M5.BackBuffer[{}]", index));
        handle.ptr += m_rtvDescriptorSize;
    }
}

std::uint32_t D3D12SwapChain::CurrentBackBufferIndex() const
{
    // 只信 SwapChain：自行 (index+1)%3 在 drop/ResizeBuffers 后会与真实前台缓冲脱节。
    return m_swapChain->GetCurrentBackBufferIndex();
}

ID3D12Resource& D3D12SwapChain::BackBuffer(const std::uint32_t index) const
{
    return *m_backBuffers.at(index).Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::RtvHandle(const std::uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
    return handle;
}

D3D12_RESOURCE_STATES D3D12SwapChain::TrackedState(const std::uint32_t index) const
{
    // 状态真源在 tracker（07 篇）：这里读它的 pending 视角。
    return m_tracker->CurrentState(m_backBufferKeys.at(index), 0U);
}

ResourceKey D3D12SwapChain::BackBufferKey(const std::uint32_t index) const
{
    return m_backBufferKeys.at(index);
}

void D3D12SwapChain::TrackTransition(const std::uint32_t index, const D3D12_RESOURCE_STATES after)
{
    // 只登记状态请求（barrier 进入 tracker 的批），由调用方在依赖新状态的绘制之前
    // FlushBarriersTo 发出——这样 pass 入口的一小批 barrier 能一次提交（07 篇 batching）。
    if (m_backBuffers[index] == nullptr)
    {
        throw std::logic_error{"back-buffer transition without a resource (released by resize?)"};
    }
    m_tracker->Transition(m_backBufferKeys.at(index), after);
}

bool D3D12SwapChain::Present()
{
    const std::uint32_t index = CurrentBackBufferIndex();
    // Present 前断言：当前 back buffer 必须已回到 PRESENT，否则 DXGI 会报 warning
    // （或更糟：GPU 仍在写时被呈现）。断言由 tracker 的记账给出（07 篇：状态真源唯一）。
    m_tracker->VerifyState(m_backBufferKeys.at(index), D3D12_RESOURCE_STATE_PRESENT);

    const UINT syncInterval = m_config.vsync ? 1U : 0U;
    // 第三个前提用 config 的 windowed 字段而不是字面 true：本篇固定窗口化，
    // 但加入独占全屏时该前提必须由配置派生（否则会静默传入被 DXGI 拒绝的 flag）。
    const UINT flags = ResolvePresentFlags(m_config.vsync, m_config.tearingEnabled, m_config.windowed);
    const HRESULT result = m_swapChain->Present(syncInterval, flags);
    if (result == DXGI_STATUS_OCCLUDED)
    {
        return false; // 被遮挡：调用方进入低频探测待机（M2 契约延续）
    }
    ThrowIfFailed(result, "IDXGISwapChain3::Present");
    return true;
}

void D3D12SwapChain::RequestResize(const std::uint32_t width, const std::uint32_t height)
{
    // 消息路径只记录：0×0 = minimized，挂起；其余尺寸等安全点再动 GPU 资源。
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_hasPendingResize = true;
}

bool D3D12SwapChain::HasPendingResize() const noexcept
{
    return m_hasPendingResize;
}

void D3D12SwapChain::ApplyPendingResize(D3D12Queue& queue)
{
    if (!m_hasPendingResize)
    {
        return;
    }
    m_hasPendingResize = false;

    // 0×0（minimized）：挂起，不渲染也不调用 ResizeBuffers（04 篇明确规定）。
    if (IsSuspendedSize(m_pendingWidth, m_pendingHeight))
    {
        m_suspended = true;
        return;
    }

    // 尺寸未变：只需解除挂起。
    if (m_pendingWidth == m_config.width && m_pendingHeight == m_config.height)
    {
        m_suspended = false;
        return;
    }

    const std::uint32_t newWidth = m_pendingWidth;
    const std::uint32_t newHeight = m_pendingHeight;
    const std::uint32_t oldWidth = m_config.width;
    const std::uint32_t oldHeight = m_config.height;

    // 安全点第 2 步：先 flush，证明没有任何提交还引用旧 back buffer。
    // reason 写明场景，供"Flush 只出现在允许场景"取证（03 篇契约）。
    queue.FlushGpu("swapchain-resize");

    // 第 3 步：**先注销旧 generation，再释放 buffer**（07 篇顺序要求：tracker 持有
    // 非拥有指针，先释放会让 state table 留下悬垂 id）。
    if (m_tracker != nullptr)
    {
        for (const ResourceKey key : m_backBufferKeys)
        {
            m_tracker->Unregister(key);
        }
    }
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& backBuffer : m_backBuffers)
    {
        backBuffer.Reset();
    }

    // 第 4 步：ResizeBuffers（保持 BufferCount/Format/Flags 与原创建一致）。
    const HRESULT resizeResult =
        m_swapChain->ResizeBuffers(kSwapChainBufferCount, newWidth, newHeight, kSwapChainFormat, m_config.flags);
    if (FAILED(resizeResult))
    {
        // 失败策略：输出 HRESULT + 旧/新尺寸 + flags，并进入不可渲染状态（不半初始化）。
        m_failed = true;
        char detail[192]{};
        std::snprintf(detail, sizeof(detail), " (old=%ux%u new=%ux%u flags=0x%X bufferCount=%u)", oldWidth, oldHeight,
                      newWidth, newHeight, m_config.flags, kSwapChainBufferCount);
        throw HResultError(resizeResult, std::string{"IDXGISwapChain3::ResizeBuffers"} + detail);
    }

    // 第 5 步起：重新 GetBuffer + 原位重写 RTV（flush 已证明无 GPU 引用），
    // 状态复位 PRESENT；本篇没有 window-size depth/HDR/readback（随 05/09 落地）。
    m_config.width = newWidth;
    m_config.height = newHeight;
    ++m_config.resizeCount;
    m_suspended = false;

    // 旧 descriptor slot 原位重写：CreateBackBufferViews 覆盖三个 slot 的内容；
    // 然后以**新 generation** 重新注册——旧引用（旧 generation）自此立即失效。
    CreateBackBufferViews(*m_device);
    ++m_generation;
    RegisterBackBuffers();
}

bool D3D12SwapChain::IsSuspended() const noexcept
{
    return m_suspended;
}

bool D3D12SwapChain::IsFailed() const noexcept
{
    return m_failed;
}

const SwapChainConfig& D3D12SwapChain::Config() const noexcept
{
    return m_config;
}

void D3D12SwapChain::SetVsync(const bool vsync) noexcept
{
    m_config.vsync = vsync;
    // 运行期切换必须重算 tearingEnabled：交换链创建时若没带 ALLOW_TEARING 标志，
    // 之后切到 vsync=0 也不能传 tearing flag（DXGI 会拒绝），metadata 与实际
    // Present flag 必须始终一致。
    m_config.tearingEnabled = ResolveTearingEnabled(m_config.tearingSupported, vsync, m_config.flags);
}
} // namespace MiniEngine::Rhi::D3D12
