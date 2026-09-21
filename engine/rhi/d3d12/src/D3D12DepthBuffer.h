// ============================================================================
// D3D12DepthBuffer.h — 主深度缓冲（R32_TYPELESS + D32_FLOAT DSV）
// 里程碑：M5（09 篇迁移顺序第 1 步「固定 triangle」；审查 4.1 缺口③的 Depth 部分）
// 职责：创建并持有主深度资源与它的 DSV，注册进 07 篇的状态跟踪器。主深度的存在是
//       "PbrOpaque / Skybox / TriangleSmoke 的 DSV 有真实资源"这件事的落地——此前
//       这些 pass 的 PSO 已经声明了 `kMainDepthFormat`，但没有任何资源与之对应。
// 为什么 R32_TYPELESS + D32_FLOAT 视图：09 篇「M4 resource profile 保持」要求
//       Shadow 与主深度都是 `R32_TYPELESS` resource + `D32_FLOAT` DSV——同一份资源
//       在需要时可以再建 `R32_FLOAT` SRV 做可视化，而不必重建资源。
// 为什么自持 DSV heap：DSV 是 CPU-only 描述符（不参与 shader 可见堆），把它和资源放在
//       一起，调用方就不必为了"有一个深度"去额外接线一个 DSV heap；同时也避免与
//       沙盒的 DSV heap 容量预算耦合。
// Resize 顺序（与 07 篇 back buffer 同款）：**先 Unregister 再释放**，再按新尺寸重建并
//       以新 generation 重新注册。tracker 不持有引用，所以顺序反了会留下悬垂指针。
// 内部性说明：后端内部类型（src/），直接暴露 ID3D12Resource 与 CPU 描述符句柄。
// 关联：docs/architecture/README.md（M4 resource profile 保持）
//       engine/rhi/d3d12/src/D3D12SwapChain.cpp（同款 Resize 顺序与注册手法）
//       engine/rhi/d3d12/src/D3D12ResourceStateTracker.h（状态真源）
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "D3D12ResourceStateRegistry.h"
#include "D3D12ResourceStateTracker.h"

namespace MiniEngine::Rhi::D3D12
{
class D3D12DepthBuffer final
{
  public:
    D3D12DepthBuffer() = default;
    ~D3D12DepthBuffer();
    D3D12DepthBuffer(const D3D12DepthBuffer&) = delete;
    D3D12DepthBuffer& operator=(const D3D12DepthBuffer&) = delete;

    // 创建深度资源与 DSV，并以**实际初始状态**（DEPTH_WRITE）注册进 tracker。
    // tracker 必须比本对象活得久（非拥有指针）。
    // 失败：device/tracker 为空、尺寸为 0、重复 Initialize → 抛异常。
    void Initialize(ID3D12Device& device, std::uint32_t width, std::uint32_t height,
                    D3D12ResourceStateTracker& tracker);

    // 换尺寸：先 Unregister 再释放旧资源，再按新尺寸重建（generation 递增）。
    // 前置条件：调用方已证明 GPU 不再引用旧资源（渲染器的 FlushGpu 安全点）。
    // 失败：未初始化、尺寸为 0 → 抛异常。
    void Resize(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] ID3D12Resource& Resource() const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle() const noexcept;
    [[nodiscard]] std::uint32_t Width() const noexcept;
    [[nodiscard]] std::uint32_t Height() const noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;
    // tracker 里的资源键（渲染器用它请求 transition）。
    [[nodiscard]] ResourceKey Key() const noexcept;
    // 重建次数（resize 后 +1）：进 metadata，用来证明"换尺寸确实换了资源"。
    [[nodiscard]] std::uint32_t Generation() const noexcept;

  private:
    void CreateAndRegister(std::uint32_t width, std::uint32_t height);
    void UnregisterAndRelease();

    // 非拥有：device 与 tracker 都由组合根/渲染器持有，生命周期长于本对象。
    ID3D12Device* m_device = nullptr;
    D3D12ResourceStateTracker* m_tracker = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsv{};
    ResourceKey m_key{};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_generation = 0;
    bool m_registered = false;
};
} // namespace MiniEngine::Rhi::D3D12
