// ============================================================================
// D3D12DepthBuffer.cpp — 主深度实现（见头注释）
// 里程碑：M5（09 篇迁移顺序第 1 步「固定 triangle」；审查 4.1 缺口③的 Depth 部分）
// 关联：engine/rhi/d3d12/src/D3D12DepthBuffer.h
// ============================================================================
#include "D3D12DepthBuffer.h"

#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include "D3D12Diagnostics.h"

#include <format>
#include <stdexcept>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 深度清屏值：1.0 = 远平面（D3D 的 [0,1] clip 约定下，反向 Z 未启用）。
constexpr float kClearDepth = 1.0F;
// 深度资源的诊断名（PIX 与调试层可见；与 05 篇 M5.* 前缀约定一致）。
constexpr const wchar_t* kResourceName = L"M5.MainDepth";
constexpr const wchar_t* kHeapName = L"M5.DsvHeap";
} // namespace

D3D12DepthBuffer::~D3D12DepthBuffer()
{
    // 析构只释放自有 COM 对象；tracker 的注销必须由调用方在释放前完成（渲染器
    // Shutdown 路径显式调用 UnregisterAndRelease 的语义由 Resize/Shutdown 保证）。
    // 这里不再调用 tracker：析构时 tracker 可能已经先析构（成员顺序不保证），
    // 触碰它会是悬垂访问。
}

void D3D12DepthBuffer::Initialize(ID3D12Device& device, const std::uint32_t width, const std::uint32_t height,
                                  D3D12ResourceStateTracker& tracker)
{
    if (m_device != nullptr)
    {
        throw std::logic_error{"D3D12DepthBuffer::Initialize called twice"};
    }
    if (width == 0U || height == 0U)
    {
        throw std::invalid_argument{"D3D12DepthBuffer::Initialize requires a non-zero size"};
    }

    m_device = &device;
    m_tracker = &tracker;

    // DSV heap：CPU-only，单槽。RTV/DSV 都不参与 shader 可见堆（05 篇四类 heap profile）。
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDescription.NumDescriptors = 1U;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device.CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_dsvHeap)),
                  "ID3D12Device::CreateDescriptorHeap(DSV)");
    Internal::SetDebugName(m_dsvHeap.Get(), kHeapName);

    CreateAndRegister(width, height);
}

void D3D12DepthBuffer::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (m_device == nullptr)
    {
        throw std::logic_error{"D3D12DepthBuffer::Resize before Initialize"};
    }
    if (width == 0U || height == 0U)
    {
        throw std::invalid_argument{"D3D12DepthBuffer::Resize requires a non-zero size"};
    }

    // 与 07 篇 back buffer 的 resize 顺序一致：**先 Unregister 再释放**。
    // tracker 只持有非拥有指针，顺序反了就会留下悬垂指针。
    UnregisterAndRelease();
    CreateAndRegister(width, height);
}

ID3D12Resource& D3D12DepthBuffer::Resource() const noexcept
{
    return *m_resource.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DepthBuffer::DsvHandle() const noexcept
{
    return m_dsv;
}

std::uint32_t D3D12DepthBuffer::Width() const noexcept
{
    return m_width;
}

std::uint32_t D3D12DepthBuffer::Height() const noexcept
{
    return m_height;
}

bool D3D12DepthBuffer::IsInitialized() const noexcept
{
    return m_registered;
}

ResourceKey D3D12DepthBuffer::Key() const noexcept
{
    return m_key;
}

std::uint32_t D3D12DepthBuffer::Generation() const noexcept
{
    return m_generation;
}

void D3D12DepthBuffer::CreateAndRegister(const std::uint32_t width, const std::uint32_t height)
{
    // 资源格式用 R32_TYPELESS、视图用 D32_FLOAT（09 篇 M4 resource profile）：
    // 同一份资源之后可以再建 R32_FLOAT SRV 做深度可视化，不必重建。
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0U;
    description.Width = static_cast<UINT64>(width);
    description.Height = static_cast<UINT>(height);
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1U; // baseline 无 MSAA（08 篇固定）
    description.SampleDesc.Quality = 0U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // 优化清屏值：让驱动按"每帧清成 1.0"优化放置；同时是 DSV 创建的必需信息。
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = kClearDepth;
    clearValue.DepthStencil.Stencil = 0U;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

    // 初始状态就是 DEPTH_WRITE：资源一创建即可作为深度目标使用，注册时按**实际**
    // 初始状态登记（07 篇：未知 state 不允许猜）。
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                    IID_PPV_ARGS(&m_resource)),
                  "ID3D12Device::CreateCommittedResource(main depth)");
    Internal::SetDebugName(m_resource.Get(), kResourceName);

    // D32_FLOAT 视图：DSV 用 Typeless 资源时必须显式给出 Format。
    D3D12_DEPTH_STENCIL_VIEW_DESC viewDescription{};
    viewDescription.Format = DXGI_FORMAT_D32_FLOAT;
    viewDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    viewDescription.Flags = D3D12_DSV_FLAG_NONE;
    viewDescription.Texture2D.MipSlice = 0U;

    m_dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateDepthStencilView(m_resource.Get(), &viewDescription, m_dsv);

    m_key = m_tracker->Register(*m_resource.Get(), 1U, D3D12_RESOURCE_STATE_DEPTH_WRITE, kResourceName, m_generation);
    m_width = width;
    m_height = height;
    m_registered = true;
}

void D3D12DepthBuffer::UnregisterAndRelease()
{
    if (m_registered)
    {
        m_tracker->Unregister(m_key);
        m_registered = false;
    }
    m_resource.Reset();
    m_key = ResourceKey{};
    m_width = 0U;
    m_height = 0U;
    // generation 递增：旧 generation 的引用（若有）自此立即失效（07 篇）。
    ++m_generation;
}
} // namespace MiniEngine::Rhi::D3D12
