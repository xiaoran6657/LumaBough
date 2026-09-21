#include "ShaderRevisionTransaction.h"
#include <algorithm>
#include <set>
namespace MiniEngine::Rhi
{
namespace
{
[[noreturn]] void Failure(const char* reason)
{
    throw RhiValidationError({RhiErrorCode::InvalidState, "ShaderReload", "ShaderRevision", "", "", reason});
}
} // namespace
ShaderRevisionTransaction::ShaderRevisionTransaction(DeviceLifetime& a, DeviceLifetime& b) : m_devices{&a, &b}
{
    if (&a == &b || a.Backend() != RhiBackend::D3D11 || b.Backend() != RhiBackend::D3D12)
        Failure("dual revision needs ordered D3D11/D3D12 owners");
}
std::size_t ShaderRevisionTransaction::Index(RhiBackend backend)
{
    if (backend != RhiBackend::D3D11 && backend != RhiBackend::D3D12)
        Failure("unknown backend");
    return static_cast<std::size_t>(backend);
}
void ShaderRevisionTransaction::RequireBoundary() const
{
    if (m_reloadActive || m_devices[0]->Diagnostics().activeFrameSerial ||
        m_devices[1]->Diagnostics().activeFrameSerial)
        Failure("revision mutation requires both frame boundaries and cannot reenter");
}
void ShaderRevisionTransaction::Reload(std::string name, std::span<const ShaderPackage> packages,
                                       const std::array<Builder, 2>& builders, const Smoke& smoke)
{
    RequireBoundary();
    if (name.empty() || packages.empty() || !builders[0] || !builders[1] || !smoke ||
        (m_current && m_current->name == name))
        Failure("revision identity, complete builders and smoke are required");
    std::set<std::pair<std::string, ShaderStage>> seen;
    for (const auto& package : packages)
    {
        ValidateShaderPackage(package);
        if (!seen.emplace(package.assetId, package.variants[0].stage).second)
            Failure("duplicate shader asset/stage in revision");
    }
    // 分配先于 native 候选和 commit；最后的旧 revision 入队与指针交换均不分配。
    m_retiring.reserve(m_retiring.size() + 1);
    auto candidate = std::make_unique<Revision>();
    candidate->name = std::move(name);
    m_reloadActive = true;
    try
    {
        for (std::size_t i = 0; i < 2; ++i)
        {
            candidate->backends[i] = builders[i](packages);
            if (!candidate->backends[i].payload || candidate->backends[i].semanticPipelineKey.empty())
                Failure("backend returned an incomplete revision");
        }
        if (candidate->backends[0].semanticPipelineKey != candidate->backends[1].semanticPipelineKey)
            Failure("native candidates were built from different semantic pipeline keys");
        for (std::size_t i = 0; i < 2; ++i)
            if (!smoke(static_cast<RhiBackend>(i), *candidate->backends[i].payload))
                Failure("candidate smoke/fixed draw failed");
        // callback 不得通过另一个入口打开帧并绕过事务边界。
        if (m_devices[0]->Diagnostics().activeFrameSerial || m_devices[1]->Diagnostics().activeFrameSerial)
            Failure("backend callback opened a frame during revision preparation");
    }
    catch (...)
    {
        m_reloadActive = false;
        throw;
    }
    m_reloadActive = false;
    if (m_current)
        m_retiring.push_back(std::move(m_current));
    m_current = std::move(candidate);
}
void ShaderRevisionTransaction::MarkUsed(RhiBackend backend, const FrameToken& frame)
{
    if (m_reloadActive || !m_current)
        Failure("no committed revision available");
    const auto index = Index(backend);
    m_devices[index]->ValidateFrameToken(frame);
    m_current->lastUse[index] = std::max(m_current->lastUse[index], frame.serial);
}
bool ShaderRevisionTransaction::Complete(const Revision& revision) const
{
    return m_devices[0]->Completed() >= revision.lastUse[0] && m_devices[1]->Completed() >= revision.lastUse[1];
}
void ShaderRevisionTransaction::Collect()
{
    if (m_reloadActive)
        Failure("cannot collect while preparing a revision");
    std::erase_if(m_retiring, [&](const auto& revision) { return Complete(*revision); });
}
void ShaderRevisionTransaction::Shutdown()
{
    RequireBoundary();
    if ((m_current && !Complete(*m_current)) ||
        std::any_of(m_retiring.begin(), m_retiring.end(), [&](const auto& r) { return !Complete(*r); }))
        Failure("shader revision still has GPU references");
    m_retiring.clear();
    m_current.reset();
}
std::string_view ShaderRevisionTransaction::CurrentRevision() const
{
    return m_current ? std::string_view(m_current->name) : std::string_view{};
}
ResourcePayload& ShaderRevisionTransaction::BackendPayload(RhiBackend backend)
{
    if (!m_current)
        Failure("no current revision");
    return *m_current->backends[Index(backend)].payload;
}
} // namespace MiniEngine::Rhi
