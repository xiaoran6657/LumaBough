#pragma once
#include <MiniEngine/RenderGraph/CompiledRenderGraph.h>
#include <MiniEngine/RenderGraph/RenderGraphBuilder.h>
#include <MiniEngine/RenderGraph/RgResources.h>
#include <any>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace MiniEngine::RenderGraph
{
class RenderGraph final
{
  public:
    RenderGraph();
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;
    // setup/execute 回调内禁止 Reset；Reset 令旧 handle、builder、plan、resolver 全失效。
    void Reset();
    void Reserve(GraphCapacity capacity);
    [[nodiscard]] GraphPhase Phase() const;
    [[nodiscard]] std::size_t PassCount() const;
    [[nodiscard]] std::size_t ResourceCount() const;
    RgTexture CreateTexture(std::string_view name, const Rhi::TextureDesc& descriptor);
    RgBuffer CreateBuffer(std::string_view name, const Rhi::BufferDesc& descriptor);
    RgTexture ImportTexture(std::string_view name, const TextureImport& imported);
    RgBuffer ImportBuffer(std::string_view name, const BufferImport& imported);
    void Present(RgTexture texture);
    void Export(RgTexture texture);
    void Export(RgBuffer buffer);
    [[nodiscard]] CompiledRenderGraph Compile();
    // 回调覆盖整个声明+编译边界；Release 生产路径据 error 跳过本帧，不能提交残缺 plan。
    [[nodiscard]] GraphCompileResult TryCompile(const std::function<void(RenderGraph&)>& declare = {});

    // setup 同步执行；name、descriptor、PassData 与 execute callable 均由 graph 持有。
    // execute 按值捕获 immutable packet/handle；C++ 无法反射 lambda 是否偷借 raw pointer，
    // 调用者须保证 RenderPacket 生命周期，不得捕获局部引用或 backend 对象。
    // setup 失败将整个未完成草稿置 Failed，必须 Reset；防止逃逸 handle 的 ABA 重用。
    template <class PassData, class Setup, class Execute>
    void AddPass(std::string_view name, Setup&& setup, Execute&& execute)
    {
        static_assert(std::is_copy_constructible_v<PassData> && std::is_default_constructible_v<PassData>);
        using Fn = std::decay_t<Execute>;
        static_assert(std::is_copy_constructible_v<Fn>);
        AddPassErased(
            name, std::any(PassData{}),
            [&](RgBuilder& builder, std::any& data) { std::invoke(setup, builder, std::any_cast<PassData&>(data)); },
            [fn = Fn(std::forward<Execute>(execute))](const std::any& data, const RgResources& resources,
                                                      Rhi::IRhiCommandList& commands)
            { std::invoke(fn, std::any_cast<const PassData&>(data), resources, commands); });
    }

  private:
    using SetupFunction = std::function<void(RgBuilder&, std::any&)>;
    using ExecuteFunction = std::function<void(const std::any&, const RgResources&, Rhi::IRhiCommandList&)>;
    void AddPassErased(std::string_view name, std::any data, const SetupFunction& setup, ExecuteFunction execute);
    std::shared_ptr<Detail::GraphState> m_state;
};
} // namespace MiniEngine::RenderGraph
