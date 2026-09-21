// ============================================================================
// D3D11Renderer.h — 具体 D3D11 渲染器的公共头文件
// 里程碑：M2 / M3
// 职责：以 PImpl 模式暴露具体 D3D11 渲染器的窄接口：构造（窗口句柄/尺寸/着色器
//   目录/Debug 开关）、Resize 与每帧 Render。本文件是 PUBLIC 头，不能包含
//   Windows.h、d3d11.h 等类型，D3D11Renderer 的具体类型与 PImpl 实现全部隐藏在
//   src/D3D11Renderer.cpp 中，从而保持 D3D 类型只出现在 engine/rhi/d3d11/ 边界内。
// 关联：docs/architecture/DECISIONS.md、docs/architecture/README.md
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace MiniEngine
{
// 前置声明：仅以引用方式使用，避免在本 PUBLIC 头中暴露 D3D/World 具体类型。
// M4-02 起 Render 直接消费 World::RenderPacket（01 篇"当前帧数据边界"：
// Sandbox/World → RenderPacket → D3D11，渲染端不再逐 item 接收松散参数）。
namespace Assets
{
class AssetManager;
} // namespace Assets

namespace World
{
struct RenderPacket;
} // namespace World

namespace Rhi
{
namespace D3D11
{
struct GpuFrameTiming;    // 08 篇：GPU 帧计时 sample（值类型，无 D3D 依赖）
struct GpuSampleCounters; // 08 篇：valid/missing/disjoint/failed 计数
} // namespace D3D11
} // namespace Rhi

// 一次完成截图的结果（07 篇）：PNG 已原子落盘；pngSha256 是最终文件字节的哈希，
// 供元数据 JSON 的 pngSha256 字段使用。纯值类型，不含 D3D 类型。
struct ScreenshotResult final
{
    std::filesystem::path pngPath;
    std::uint32_t width{};
    std::uint32_t height{};
    std::string pngSha256;
};

// 具体 D3D11 渲染器：负责设备、交换链、管线、资源与每帧绘制的完整生命周期。
//
// 生命周期约定：D3D11Renderer 的析构会释放全部 D3D 资源并生成 live-object 报告，
// 因此调用方必须保证 renderer 先于它使用的原生窗口析构，避免窗口销毁后仍持有
// 其句柄。本类型不可拷贝、不可移动，所有权完全由内部 Impl（unique_ptr）承载。
class D3D11Renderer
{
  public:
    // 创建并初始化 D3D11 渲染器（设备、交换链、着色器、立方体、纹理与深度资源）。
    //
    // nativeWindow 是 WindowsWindow::NativeHandle() 返回的不透明 void* 桥，内部
    // 转换为 Win32 HWND 用于创建交换链；renderer 不拥有、不销毁该窗口。shaderDirectory
    // 指向包含 PbrForward.hlsl 的目录（M4-02 起的运行时管线；tests/rhi 的 HLSL gate
    // 以同一目录为编译清单），运行时据此编译着色器。enableDebugLayer 为 true
    // 时强制启用 D3D11 Debug Layer，缺失时失败而非静默降级。
    //
    // 参数：
    //   nativeWindow    —— 原生窗口句柄（期望为 Win32 HWND，经 void* 传入）
    //   width / height —— 初始客户区尺寸（像素），必须非零
    //   shaderDirectory—— 着色器源码所在目录
    //   enableDebugLayer—— 是否启用 D3D11 Debug Layer（Debug 构建由 main.cpp 传入 true）
    // 失败：窗口句柄无效、尺寸为零、着色器缺失、Debug Layer 缺失或任何 D3D 调用失败时
    //   抛出 std::runtime_error / std::invalid_argument。
    D3D11Renderer(void* nativeWindow, std::uint32_t width, std::uint32_t height, std::filesystem::path shaderDirectory,
                  bool enableDebugLayer);
    // 释放全部 D3D 资源并输出 live-object 报告（Debug 构建）。须先于窗口析构。
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;
    D3D11Renderer(D3D11Renderer&&) = delete;
    D3D11Renderer& operator=(D3D11Renderer&&) = delete;

    // 按新客户区尺寸重建尺寸相关资源（后备缓冲 RTV、深度纹理/DSV、Viewport）。
    //
    // 内部按安全顺序执行：解绑 RTV/DSV 与清理状态 → 释放尺寸相关资源 → ResizeBuffers
    // → 重建 RTV/DSV/Viewport。零尺寸或尺寸未变化时直接忽略，避免在拖拽窗口的中间态
    // 触发无意义的重建。
    //
    // 参数：
    //   width / height —— 新的客户区尺寸（像素）
    void Resize(std::uint32_t width, std::uint32_t height);
    // 渲染一帧烘焙场景并呈现到屏幕。
    //
    // packet 是当前帧不可变快照（RenderQueueBuilder 产出：culling + AssetId 稳定排序
    // + 相机/光照数据）；渲染器内部用 D3D11AssetCache 按 revision 上传 VB/IB/Texture，
    // 逐 draw 从 AssetManager 解析 MaterialAsset 填 b2 常量并绑定 t0–t4（缺省槽位绑定
    // 真实 fallback SRV）。M4-05 起场景（PBR + skybox）写入 RGBA16F HDR target，
    // tone map pass 再把它按固定曝光 + Reinhard + 显式 sRGB 写入 UNORM 后备缓冲。
    // 窗口被完全遮挡时使用低频 DXGI_PRESENT_TEST 探测待机，不忙等。
    //
    // 参数：
    //   packet —— 本帧渲染快照（mainOpaque 为已排序的绘制列表）
    //   assets —— CPU AssetManager（上传所需的 MeshAsset/TextureAsset/MaterialAsset）
    // 返回：true 表示已正常呈现；false 表示交换链被遮挡，本次未呈现。
    // 失败：Present 返回设备移除类错误时抛 std::runtime_error（含 removal reason）。
    [[nodiscard]] bool Render(const World::RenderPacket& packet, const Assets::AssetManager& assets);

    // 设置 debug view 模式（b0 DebugMode，03/04 篇"Debug UI/CLI 可调"的 CLI 入口）：
    // 0 = 最终着色；1 = baseColor；2 = normal；3 = metallic；4 = roughness；
    // 5 = AO；6 = direct；7 = indirect（IBL）；8 = shadow factor；9 = shadow map 灰度；
    // 10 = SceneLuminance 假色（05 篇：在 tone map pass 生效，洋红标记亮度 >1 的
    // 像素，用于证明 HDR target 中确实存在超过 1 的 radiance）。
    // 仅供校准/RenderDoc 取证使用；golden capture 必须使用默认值 0。
    void SetDebugMode(std::uint32_t debugMode);

    // 设置固定曝光（05 篇「手动曝光」）：相对 EV，着色器按 exp2(exposureEv) 缩放
    // HDR radiance。基线固定 0.0（场景亮度靠 light/environment 数据校准），固定截图
    // 禁止键盘临时值；范围预算 [-10, +10]，越界按端点钳制（可用 ExposureEv() 读回
    // 实际生效值并记入性能报告）。
    void SetExposureEv(float exposureEv);
    // 返回当前实际生效的曝光值（钳制后的结果）。
    [[nodiscard]] float ExposureEv() const noexcept;

    // 提供环境 panorama（04 篇 IBL 输入；RGBA16F 半精度逐像素，size = width*height*4）。
    // revision 与上次不同时：上传 2D panorama → BuildTemporary（conversion→mip 链→
    // irradiance→prefilter→LUT→finite 校验）→ 校验通过 CommitTemporary（帧边界单次
    // 执行，不逐帧重建）；失败丢弃 temporary 并记日志——有旧 active 则继续用旧资源
    // （04 篇 reload 失败语义），无 active 则 IBL 保持空（渲染端绑定 fallback，indirect=0）。
    // 传空 pixels（size=0）视为移除环境：释放 IBL 资源，回到 direct-only。
    // 返回 true 表示本次调用完成了重建且成功；false 表示无变化或重建失败。
    bool UpdateEnvironmentPanorama(std::uint32_t width, std::uint32_t height, const std::uint16_t* rgba16HalfPixels,
                                   std::uint64_t revision);

    // 返回当前后备缓冲的客户区宽度（像素）。
    [[nodiscard]] std::uint32_t Width() const noexcept;
    // 返回当前后备缓冲的客户区高度（像素）。
    [[nodiscard]] std::uint32_t Height() const noexcept;

    // ---- 07 篇截图与基线采集 ----

    // 请求在下一帧 tone-map 写完后备缓冲后截图（staging ring 异步读回，不阻塞帧）。
    // 重复调用以最后一次为准；失败（格式不支持/ring 满）经日志与 PollScreenshot 的
    // Failed 状态报告，不抛异常（截图是证据路径，不是帧管线的一部分）。
    void RequestScreenshot(const std::filesystem::path& pngPath);
    // 轮询截图状态：true = out 已填充（PNG 完成）；false = 尚未完成。失败会写入日志。
    bool PollScreenshot(ScreenshotResult& out);
    // IBL 是否处于 Ready（07 篇：IblState::Ready 才允许捕获）。
    [[nodiscard]] bool IblReady() const noexcept;
    // 设置 Present 间隔（0 = 关闭垂直同步；capture/RenderDoc 用，默认 1）。
    void SetPresentInterval(std::uint32_t interval) noexcept;
    // GPU 适配器描述（截图元数据的 gpu 字段）。
    [[nodiscard]] std::string GpuDescription() const noexcept;
    // 本进程累计的 Debug Layer 消息数（WARNING 及以上；元数据 debugLayerMessages）。
    [[nodiscard]] std::uint32_t DebugLayerMessageCount() const noexcept;

    // ---- 08 篇性能基线 ----

    // GPU 硬件信息（benchmark 报告的 gpu 字段；取自 DXGI 适配器描述）。
    struct GpuInfo final
    {
        std::string adapter;
        std::uint32_t vendorId{};
        std::uint32_t deviceId{};
        std::string driverVersion; // 64 位驱动版本的高低位拼接（"31.0.x.y" 形态）
    };

    [[nodiscard]] GpuInfo GetGpuInfo() const noexcept;
    // 最近一次 Present 的 CPU 耗时（微秒）——presentCpu 是 CPU 在 Present 中的
    // 耗时，不等于 GPU frame（08 篇「CPU timing」）。
    [[nodiscard]] double LastPresentCpuMicroseconds() const noexcept;
    // 轮询一个 GPU 计时 sample：true = out 已填充（valid/disjoint/missing 语义见
    // GpuFrameTiming；计数器经 GpuSampleCounters() 观察）。
    bool PollGpuSample(Rhi::D3D11::GpuFrameTiming& out) noexcept;
    [[nodiscard]] Rhi::D3D11::GpuSampleCounters GpuSampleCounters() const noexcept;
    // ring 全部 pending（写者追上读者）而跳过计时的帧数——benchmark 据此判 BLOCKED。
    [[nodiscard]] std::uint32_t GpuSkippedFrameCount() const noexcept;

  private:
    // PImpl：持有全部 D3D 具体类型（ID3D11Device 等），隔离在本文件之外。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace MiniEngine
