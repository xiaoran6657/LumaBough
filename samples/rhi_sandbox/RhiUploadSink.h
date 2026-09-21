// ============================================================================
// RhiUploadSink.h — UploadSink 的真实实现（M7-07，render thread）
// 里程碑：M7-07（上传预算、热重载与背压）
// 职责：把 decode payload 变成 RHI 资源：
//         Mesh      → VB + IB（CreateBuffer(GpuOnly, initialData)，与 M6 路径同源）
//         Texture   → CreateTexture + UploadTexture（mip 链逐级上传）
//         Material  → 无 GPU 对象（材质是逐 draw 常量；分配 token 以便提交事务统一记账）
//         World     → 无 GPU 对象（已校验字节交给 engine/world 实例化）
//       fence：用"录制上传所在帧的 serial"。该帧完成后（completedSerial 达标）上传才可用，
//       这正是 D3D12 的语义；D3D11 由 backend 的 PollCompleted 推进同一个计数。
// 线程归属：只允许在 render thread 的帧内（BeginFrame 之后、EndFrame 之前）调用
//       CreateAndUpload；IsFenceComplete/DeferDestroy 同样是 render thread 语义。
// 关联：engine/assets/async/include/MiniEngine/Assets/UploadSink.h（端口契约）
//       engine/render/src/M6SceneResources.cpp（Mesh/Texture 上传的既有惯例）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/UploadSink.h>
#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Rhi/IRhiDevice.h>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace MiniEngine::Sandbox
{
// 已上传资源登记项：token → 真实句柄（材料/World 只登记字节，无 GPU 句柄）。
struct UploadedResource final
{
    Assets::UploadResourceToken token = 0;
    Assets::AssetId assetId{};
    Assets::AssetKind kind = Assets::AssetKind::Mesh;
    std::uint64_t revision = 0;
    std::size_t bytes = 0;
    bool hasGpuObject = false;
    bool alive = false;
    Rhi::BufferHandle vertex;
    Rhi::BufferHandle index;
    Rhi::TextureHandle texture;
};

class RhiUploadSink final : public Assets::UploadSink
{
  public:
    explicit RhiUploadSink(Rhi::IRhiDevice& device)
        : m_device(device), m_ownerThread(std::this_thread::get_id()) // M7-10：owner thread（A13）
    {
    }

    // ---- UploadSink ----
    // 必须在**帧外**调用：公共 RHI 的 UploadTexture/UploadBuffer 是独立上传（自带提交与
    // signal），帧内调用会被 `upload needs an unreferenced resource and a frame boundary` 拒绝。
    [[nodiscard]] Assets::UploadResult CreateAndUpload(const Assets::UploadRequestDescription& description) override;
    [[nodiscard]] bool IsFenceComplete(std::uint64_t fence) const noexcept override;
    void DeferDestroy(Assets::UploadResourceToken resource) noexcept override;
    void ReleaseAll() noexcept override;

    // ---- 取证 ----
    [[nodiscard]] const std::vector<UploadedResource>& Resources() const noexcept
    {
        return m_resources;
    }
    [[nodiscard]] std::size_t LiveResourceCount() const noexcept; // 仍持有 GPU 句柄/登记项
    [[nodiscard]] std::uint32_t DestroyedCount() const noexcept
    {
        return m_destroyed;
    }
    [[nodiscard]] std::uint32_t CreateFailures() const noexcept
    {
        return m_failures;
    }

    // M7-10（A13）：owner thread 断言。上传/提交/退休只允许在构造该 sink 的线程
    // （render thread 语义）上发生；Debug 下违反即断言失败。
    void AssertOwnerThread(const char* operation) const noexcept
    {
        ME_ASSERT(std::this_thread::get_id() == m_ownerThread, operation);
        static_cast<void>(operation);
    }

  private:
    Rhi::IRhiDevice& m_device;
    std::thread::id m_ownerThread;
    std::vector<UploadedResource> m_resources;
    Assets::UploadResourceToken m_nextToken = 1;
    std::uint32_t m_destroyed = 0;
    std::uint32_t m_failures = 0;
};
} // namespace MiniEngine::Sandbox
