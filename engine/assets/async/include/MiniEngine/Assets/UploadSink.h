// ============================================================================
// UploadSink.h — 后端无关的"上传端口"（M7-07）
// 里程碑：M7-07（上传预算、热重载与背压）
// 职责：把"创建 GPU 资源 + 上传 + fence + 延迟销毁"从 assets 层抽象出去。
//       engine/assets/** 不得 include RHI/平台头（tools/validation/check_rhi_boundary.py），
//       因此 AssetUploadCoordinator 只依赖本端口；真实实现放在 render/运行器侧。
// 约束：所有方法只在 render thread 语义的调用点执行（与 AssetUploadCoordinator 相同）；
//       worker/I/O 线程不得触碰本端口，也不得触碰 RHI device/command list/descriptor heap。
// 关联：docs/architecture/README.md「UploadRequest 契约」
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetLoadPayload.h>
#include <MiniEngine/Assets/AssetLoadRequest.h>
#include <MiniEngine/Assets/AssetRegistry.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace MiniEngine::Assets
{
// GPU 资源句柄的进程内标识（0 = 无）。真实对象由 sink 自己持有，协调器只记 token。
using UploadResourceToken = std::uint64_t;

// worker 生成的"后端无关描述"在 render thread 侧补齐只读引用后的形态。
// payload 指针只在本次调用内有效：sink 必须当场复制/上传，不得保存。
struct UploadRequestDescription final
{
    RequestId requestId = 0;
    AssetId assetId{};
    std::uint64_t revision = 0;
    std::uint64_t dependencyHash = 0;
    AssetKind kind = AssetKind::Mesh;
    std::size_t byteCount = 0;
    const CpuAssetPayload* payload = nullptr;
};

struct UploadResult final
{
    bool success = false;
    UploadResourceToken resource = 0; // 成功时的 GPU 资源 token
    // 上传何时对 GPU 可用：0 = 创建即完成（同步后端/D3D11 语义）；
    // 非 0 = 需要等 fence 达到该值（D3D12 的帧 serial）。
    std::uint64_t completionFence = 0;
    std::size_t actualBytes = 0; // 实际消耗（与描述的估计值分开记账）
    std::string errorText;       // 可读原因（不含绝对路径）
};

class UploadSink
{
  public:
    UploadSink() = default;
    virtual ~UploadSink() = default;

    UploadSink(const UploadSink&) = delete;
    UploadSink& operator=(const UploadSink&) = delete;

    // render thread：创建 RHI 资源并把 payload 上传进去。
    // 契约：同一资源可被多次创建（N+1 覆盖 N），旧资源由协调器通过 DeferDestroy 交还给 sink。
    [[nodiscard]] virtual UploadResult CreateAndUpload(const UploadRequestDescription& description) = 0;
    // render thread：完成判据（fence == 0 视为已完成）。
    [[nodiscard]] virtual bool IsFenceComplete(std::uint64_t fence) const noexcept = 0;
    // render thread：延迟销毁（旧 revision 资源、失败/过期的新资源）。
    virtual void DeferDestroy(UploadResourceToken resource) noexcept = 0;
    // render thread：关闭前把仍在 sink 手里的资源全部交还（默认无额外动作）。
    virtual void ReleaseAll() noexcept = 0;
};
} // namespace MiniEngine::Assets
