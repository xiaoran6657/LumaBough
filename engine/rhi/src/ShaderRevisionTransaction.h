#pragma once
#include "DeviceLifetime.h"
#include <MiniEngine/Rhi/RhiShaderPackage.h>

namespace MiniEngine::Rhi
{
// 资产/RHI 组合层的双变体事务。payload 必须完整拥有该后端 revision 的
// Shader/Pipeline/Set 原生候选，析构仅释放已完成对象；不得留下外部拥有者。
struct ShaderRevisionBackend final
{
    std::unique_ptr<ResourcePayload> payload;
    std::string semanticPipelineKey;
};
class ShaderRevisionTransaction final
{
  public:
    using Builder = std::function<ShaderRevisionBackend(std::span<const ShaderPackage>)>;
    // smoke 返回/抛出前必须完成本次 smoke 的 GPU 工作；未完成的测试提交不得遗留到回滚。
    using Smoke = std::function<bool(RhiBackend, ResourcePayload&)>;
    ShaderRevisionTransaction(DeviceLifetime& d3d11, DeviceLifetime& d3d12);
    void Reload(std::string revision, std::span<const ShaderPackage> packages, const std::array<Builder, 2>& builders,
                const Smoke& smoke);
    // backend 在录制当前 revision 时登记；完成号由各自真实 backend 交回 DeviceLifetime。
    void MarkUsed(RhiBackend backend, const FrameToken& frame);
    void Collect();
    void Shutdown();
    std::string_view CurrentRevision() const;
    std::size_t RetiringCount() const
    {
        return m_retiring.size();
    }
    ResourcePayload& BackendPayload(RhiBackend backend);

  private:
    struct Revision final
    {
        std::string name;
        std::array<ShaderRevisionBackend, 2> backends;
        std::array<std::uint64_t, 2> lastUse{};
    };
    bool Complete(const Revision& revision) const;
    void RequireBoundary() const;
    static std::size_t Index(RhiBackend backend);
    std::array<DeviceLifetime*, 2> m_devices;
    std::unique_ptr<Revision> m_current;
    std::vector<std::unique_ptr<Revision>> m_retiring;
    bool m_reloadActive = false;
};
} // namespace MiniEngine::Rhi
