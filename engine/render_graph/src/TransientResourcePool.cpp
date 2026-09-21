#include <MiniEngine/RenderGraph/RenderGraphTypes.h>
#include <MiniEngine/RenderGraph/TransientResourcePool.h>
#include <MiniEngine/Rhi/RhiValidation.h>
#include <algorithm>
#include <map>
#include <vector>

namespace MiniEngine::RenderGraph
{
struct TransientResourcePool::Impl
{
    struct Entry
    {
        Rhi::TextureHandle texture;
        Rhi::BufferHandle buffer;
        Rhi::TextureDesc textureDesc;
        Rhi::BufferDesc bufferDesc;
        std::uint64_t bytes = 0;
        bool used = false;
    };
    Rhi::IRhiDevice& device;
    std::map<std::uint32_t, std::vector<Entry>> lanes;
    std::vector<Entry>* active = nullptr;
    std::uint64_t lastSerial = 0;
    TransientPoolStatistics stats;
    explicit Impl(Rhi::IRhiDevice& owner) : device(owner)
    {
    }
    void Retire(Entry& entry)
    {
        if (entry.texture)
            device.Destroy(entry.texture);
        else
            device.Destroy(entry.buffer);
        stats.bytes -= entry.bytes;
        --stats.resources;
        ++stats.retired;
    }
    void Added(std::uint64_t bytes)
    {
        ++stats.created;
        ++stats.resources;
        stats.bytes += bytes;
        stats.highWaterBytes = std::max(stats.highWaterBytes, stats.bytes);
        stats.highWaterResources = std::max(stats.highWaterResources, stats.resources);
    }
};
TransientResourcePool::TransientResourcePool(Rhi::IRhiDevice& device) : m_impl(std::make_unique<Impl>(device))
{
}
TransientResourcePool::~TransientResourcePool()
{
    // 析构不抛异常；每个资源单独清理，避免一个失效句柄阻止剩余退休。
    for (auto& [lane, entries] : m_impl->lanes)
    {
        (void)lane;
        for (auto& entry : entries)
            try
            {
                m_impl->Retire(entry);
            }
            catch (...)
            {
            }
    }
}
TransientPoolStatistics TransientResourcePool::Statistics() const
{
    return m_impl->stats;
}
void TransientResourcePool::Clear()
{
    auto& pool = *m_impl;
    if (pool.active)
        throw GraphPhaseError("cannot clear a leased transient pool");
    for (auto& [lane, entries] : pool.lanes)
    {
        (void)lane;
        while (!entries.empty())
        {
            pool.Retire(entries.back());
            entries.pop_back();
        }
    }
    pool.lanes.clear();
}
void TransientResourcePool::BeginLease(Rhi::IRhiDevice& device, const Rhi::FrameToken& frame)
{
    auto& pool = *m_impl;
    if (&device != &pool.device || pool.active || frame.serial <= pool.lastSerial)
        throw GraphPhaseError("transient pool device/frame/lease mismatch");
    // 真实设备校验 token 与当前活动帧；不能只信调用者传来的 lane 数字。
    (void)device.AcquireBackBuffer(frame);
    pool.active = &pool.lanes[frame.recycleLane];
    pool.lastSerial = frame.serial;
    for (auto& entry : *pool.active)
        entry.used = false;
}
Rhi::TextureHandle TransientResourcePool::Acquire(const Rhi::TextureDesc& desc)
{
    auto& pool = *m_impl;
    if (!pool.active)
        throw GraphPhaseError("transient acquisition requires active lease");
    for (auto& entry : *pool.active)
        if (!entry.used && entry.texture && Rhi::SemanticallyEqual(entry.textureDesc, desc))
        {
            entry.used = true;
            ++pool.stats.reused;
            return entry.texture;
        }
    Rhi::ValidateTextureDesc(desc, pool.device.Capabilities());
    Impl::Entry entry;
    entry.textureDesc = desc;
    entry.used = true;
    auto width = desc.extent.width, height = desc.extent.height;
    for (std::uint16_t mip = 0; mip < desc.mipLevels; ++mip)
    {
        entry.bytes += std::uint64_t{width} * height * desc.arrayLayers * desc.sampleCount *
                       (desc.format == Rhi::Format::Rgba16Float ? 8U : 4U);
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    // 先准备容器容量，Create 成功后发布不会再分配，避免异常丢失 native owner。
    pool.active->reserve(pool.active->size() + 1);
    entry.texture = pool.device.CreateTexture(desc);
    const auto result = entry.texture;
    const auto bytes = entry.bytes;
    pool.active->push_back(std::move(entry));
    pool.Added(bytes);
    return result;
}
Rhi::BufferHandle TransientResourcePool::Acquire(const Rhi::BufferDesc& desc)
{
    auto& pool = *m_impl;
    if (!pool.active)
        throw GraphPhaseError("transient acquisition requires active lease");
    for (auto& entry : *pool.active)
        if (!entry.used && entry.buffer && Rhi::SemanticallyEqual(entry.bufferDesc, desc))
        {
            entry.used = true;
            ++pool.stats.reused;
            return entry.buffer;
        }
    Rhi::ValidateBufferDesc(desc);
    Impl::Entry entry;
    entry.bufferDesc = desc;
    entry.used = true;
    entry.bytes = desc.size;
    pool.active->reserve(pool.active->size() + 1);
    entry.buffer = pool.device.CreateBuffer(desc, {});
    const auto result = entry.buffer;
    pool.active->push_back(std::move(entry));
    pool.Added(desc.size);
    return result;
}
void TransientResourcePool::EndLease(bool succeeded)
{
    auto& pool = *m_impl;
    if (!pool.active)
        return;
    auto* entries = pool.active;
    pool.active = nullptr;
    // 失败帧不把可能部分录制的逻辑 owner 带回池；RHI 决定实际退休时间。
    for (std::size_t i = entries->size(); i-- > 0;)
        if (!succeeded || !(*entries)[i].used)
        {
            pool.Retire((*entries)[i]);
            entries->erase(entries->begin() + static_cast<std::ptrdiff_t>(i));
        }
}
} // namespace MiniEngine::RenderGraph
