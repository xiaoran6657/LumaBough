// ============================================================================
// D3D12FrameReadback.h — 有界的 D3D12 帧时间戳与截图回读
// 里程碑：M5（09 篇输出一致性；08 篇 GPU timestamp）
// 职责：为三个 FrameContext 持有独立 timestamp query slice 与 READBACK buffer，
//       按提交 fence 回收；提供三槽 FIFO 的 RGBA8 截图机械回读。
//       本模块不负责 Renderer、barrier、PNG 编码、fence 等待或 GPU Flush。
// 关联：docs/architecture/README.md
//       docs/architecture/DECISIONS.md
//       docs/architecture/DECISIONS.md
// ============================================================================
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// 回读槽位与 M5 冻结的三个 FrameContext 一一对应，不能跨槽位复用。
inline constexpr std::uint32_t kD3D12FrameReadbackCount = 3U;

// pass 顺序固定为 Shadow、Forward、Skybox、Tonemap（下标 0—3）；下标 4 保留给可选总 GPU frame。
inline constexpr std::uint32_t kD3D12FrameReadbackPassCount = 5U;

// 一帧已消费的 GPU pass 时间；valid=false 表示该 pass 没有成对执行 query。
struct D3D12FrameTiming final
{
    std::uint32_t frameIndex{};
    std::uint64_t frameNumber{};
    std::array<double, kD3D12FrameReadbackPassCount> passMilliseconds{};
    std::array<bool, kD3D12FrameReadbackPassCount> valid{};
};

// 已完成截图的尺寸和 top-to-bottom、紧密排列的 RGBA8 像素。
struct D3D12Rgba8Image final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

// 固定槽位的 D3D12 timestamp 与三槽截图 readback 生命周期管理器。
//
// query heap 可共享，但每个槽位有独立 READBACK buffer 和不重叠 query slice。
// BeginFrame 只能在上一次 Commit 的 fence 完成后复用；Map 前始终检查
// completedFence >= fence 且 fence 非零。本类不保存 fence 对象，也不主动等待。
class D3D12FrameReadback final
{
  public:
    D3D12FrameReadback() = default;
    ~D3D12FrameReadback() = default;
    D3D12FrameReadback(const D3D12FrameReadback&) = delete;
    D3D12FrameReadback& operator=(const D3D12FrameReadback&) = delete;

    // 创建 timestamp query heap、三个 timestamp READBACK buffer，并读取 queue 频率。
    //
    // 参数：
    //   device —— 创建 query heap 与 READBACK 资源的 D3D12 设备。
    //   queue  —— 提供 timestamp frequency 的 command queue。
    // 失败：重复初始化抛 std::logic_error；D3D12 调用失败抛 HResultError；零频率抛 std::runtime_error。
    void Initialize(ID3D12Device& device, ID3D12CommandQueue& queue);

    // 消费已完成的旧 timestamp，并开启指定帧槽位的录制。
    //
    // 参数：
    //   frameIndex —— [0，kD3D12FrameReadbackCount) 内的槽位。
    //   completedFence —— 调用者观察到的 queue completed fence。
    // 失败：未初始化、下标越界、槽位仍在录制或其 fence 未完成抛异常；本函数不等待。
    void BeginFrame(std::uint32_t frameIndex, std::uint64_t completedFence, std::uint64_t frameNumber = 0U);

    // 轮询并消费所有已完成的 timestamp 槽位；不等待 GPU，供退出或 benchmark 收尾使用。
    // 已由 BeginFrame 消费的槽位会被清空，不会重复返回计时。
    void Poll(std::uint64_t completedFence);

    // 记录固定 pass 的 timestamp begin 点；D3D12 两个端点均使用 EndQuery。
    //
    // 参数：list —— 当前 command list；frameIndex —— 帧槽位；passIndex —— Shadow/Forward/Skybox/Tonemap 的 0—3。
    // 失败：槽位未开启、下标越界或重复开始抛异常。
    void BeginPass(ID3D12GraphicsCommandList& list, std::uint32_t frameIndex, std::uint32_t passIndex);

    // 记录 timestamp end 点并使该 query 对可被 Resolve。
    //
    // 参数：list —— 当前 command list；frameIndex —— 帧槽位；passIndex —— 与 BeginPass 相同的下标。
    // 失败：槽位未开启、下标越界、没有 BeginPass 或重复结束抛异常。
    void EndPass(ID3D12GraphicsCommandList& list, std::uint32_t frameIndex, std::uint32_t passIndex);

    // 只 resolve 已成对执行的 query；未执行 pass 不会进入 ResolveQueryData。
    //
    // 参数：list —— 当前 command list；frameIndex —— 帧槽位。
    // 失败：槽位未开启或重复 resolve 抛 std::logic_error。
    void Resolve(ID3D12GraphicsCommandList& list, std::uint32_t frameIndex);

    // 将 timestamp 回读槽位标记到包含 Resolve 的非零提交 fence。
    //
    // 参数：frameIndex —— 帧槽位；fence —— 同一 queue 提交的 fence。
    // 失败：未开启、未 Resolve、重复提交或 fence 为零抛异常。
    void Commit(std::uint32_t frameIndex, std::uint64_t fence);

    // 仅录制资源到 READBACK buffer 的 texture copy；资源 barrier 由外部负责。
    // 支持单采样 RGBA8/BGRA8；BGRA 在读回时转换为 RGBA；本函数不编码 PNG。
    //
    // 参数：device —— 计算 footprint 并创建 READBACK；list —— 当前 command list；
    //   resource —— 已处于 COPY_SOURCE 的二维后备缓冲；width/height —— 像素尺寸。
    // 失败：三个槽位均待消费或已有未提交截图、尺寸/格式不匹配、footprint 非法或资源创建失败抛异常。
    void RecordScreenshot(ID3D12Device& device, ID3D12GraphicsCommandList& list, ID3D12Resource& resource,
                          std::uint32_t width, std::uint32_t height);

    // 为截图 copy 写入非零提交 fence；完成前 TryReadScreenshot 返回空。
    //
    // 参数：fence —— 包含截图 copy 的 queue 提交 fence。
    // 失败：无待提交截图、重复提交或 fence 为零抛异常。
    void CommitScreenshot(std::uint64_t fence);

    // fence 完成后 Map 并逐行打包紧密 RGBA8；未完成时不 Map。
    //
    // 参数：completedFence —— 调用者观察到的 queue completed fence。
    // 返回：截图已完成返回图像，否则返回 std::nullopt。
    // 失败：Map 失败、范围溢出或 footprint 行距不足抛异常；失败时保留待读状态。
    [[nodiscard]] std::optional<D3D12Rgba8Image> TryReadScreenshot(std::uint64_t completedFence);

    // 取走自上次调用以来已消费的帧计时，按消费顺序返回，每帧含四个 pass 的毫秒值和 valid。
    [[nodiscard]] std::vector<D3D12FrameTiming> TakeTimings() noexcept;

    // 返回是否存在已录制但尚未读回的截图。
    [[nodiscard]] bool IsScreenshotPending() const noexcept;
    [[nodiscard]] bool CanRecordScreenshot() const noexcept;

  private:
    struct FrameSlot final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback;
        std::uint64_t fence{};
        std::uint64_t frameNumber{};
        bool recording{};
        bool resolved{};
        std::array<bool, kD3D12FrameReadbackPassCount> began{};
        std::array<bool, kD3D12FrameReadbackPassCount> ended{};
    };

    void ConsumeTimestamp(FrameSlot& slot, std::uint32_t frameIndex, std::uint64_t completedFence);
    FrameSlot& RequireFrame(std::uint32_t frameIndex);

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap;
    std::array<FrameSlot, kD3D12FrameReadbackCount> m_frames{};
    std::uint64_t m_timestampFrequency{};
    std::vector<D3D12FrameTiming> m_completedTimings;

    struct ScreenshotSlot final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint32_t width{}, height{};
        std::uint64_t totalBytes{}, fence{};
        bool bgra{}, pending{}, committed{};
    };
    std::array<ScreenshotSlot, kD3D12FrameReadbackCount> m_screenshots{};
    std::size_t m_screenshotReadIndex{}, m_screenshotWriteIndex{}, m_screenshotCount{};
    bool m_initialized{};
};
} // namespace MiniEngine::Rhi::D3D12
