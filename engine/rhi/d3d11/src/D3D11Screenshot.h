// ============================================================================
// D3D11Screenshot.h — M4-07 截图 readback（staging ring + WIC 原子写）与图像比较
// 里程碑：M4（07 篇「截图读取路径」「先测重复性，再定阈值」；手抄清单第 2、3 条）
// 职责：三部分，全部是"帧外"的证据采集路径，不进入 steady-state 帧基线：
//   1. 3-slot staging ring：把 tone-map 后的后备缓冲 CopyResource 到 STAGING 纹理，
//      用 D3D11_QUERY_EVENT 等较老 slot 完成后再 Map——不阻塞提交帧（07 篇 4）。
//      Map 后按 RowPitch 逐行打包（不假定紧密排列，07 篇 5）。
//   2. WIC PNG 原子写：写 target.tmp → 读取哈希 → rename 到目标；任何失败只清理
//      临时文件，绝不留下半成品 PNG（07 篇 8）。后备缓冲只接受 R8G8B8A8/B8G8R8A8
//      UNORM 且无 MSAA，其余 DXGI_FORMAT 直接失败并报出实际格式（07 篇：不静默转换）。
//   3. 纯 CPU 比较器：归一化 RGB 的 MAE/RMSE/p99/max/changedRate（sampleCount =
//      width*height*3，不是像素数，07 篇「先测重复性」），以及截图元数据配置的
//      INCOMPARABLE 判定（配置不同必须拒绝比较，不能给出像素 PASS）。
// 关联：docs/architecture/README.md
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（唯一消费方：RequestScreenshot/Poll）
//       tests/rendering/ImageComparisonTests.cpp（比较器数学的单元测试）
// ============================================================================
#pragma once

#include <MiniEngine/Rhi/D3D11/D3D11ImageComparison.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace MiniEngine::Rhi::D3D11
{
// 一次完成的截图：PNG 路径、尺寸与文件哈希（元数据 JSON 的 pngSha256 来源）。
struct ScreenshotResult final
{
    std::filesystem::path pngPath;
    std::uint32_t width{};
    std::uint32_t height{};
    std::string pngSha256; // 十六进制（小写），对最终 PNG 文件字节计算
};

// 3-slot staging ring：Enqueue 占用一个 slot 并提交 CopyResource + EVENT 查询；
// Poll 只处理最老的 pending slot，完成才 Map + 编码（07 篇：等较老 slot ready）。
// 非拷贝非移动（ComPtr 持有）；Create 失败抛 std::runtime_error。
class D3D11Screenshot final
{
  public:
    D3D11Screenshot() = default;
    ~D3D11Screenshot() = default;
    D3D11Screenshot(const D3D11Screenshot&) = delete;
    D3D11Screenshot& operator=(const D3D11Screenshot&) = delete;

    // 预创建 3 个 EVENT 查询（staging 纹理按需创建——尺寸随窗口变化）。
    void Create(ID3D11Device& device);
    void Release() noexcept;

    // 把后备缓冲复制进一个空闲 slot。失败（格式不支持/MSAA/ring 满）返回 false
    // 并写 error；后备缓冲格式不是 RGBA8/BGRA8 UNORM 时 error 报出实际格式。
    bool Enqueue(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11Texture2D& backBuffer,
                 const std::filesystem::path& target, std::string& error);

    enum class PollResult
    {
        Idle,      // 无 pending slot
        Pending,   // 最老 slot 尚未完成（GPU 仍在跑），下帧再试
        Completed, // out 已填充（PNG 已原子落盘）
        Failed     // 读回/编码失败，error 已填写；该 slot 已被丢弃
    };

    // 轮询最老的 pending slot；完成后 Map → RowPitch 逐行打包 → WIC 原子写。
    PollResult Poll(ID3D11DeviceContext& context, ScreenshotResult& out, std::string& error);

    // 是否所有 slot 空闲（同时只允许一张在途截图的调用方用不到，仅供诊断）。
    [[nodiscard]] bool AllIdle() const noexcept;

  private:
    struct Slot final
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Query> completed;
        std::filesystem::path target;
        bool pending = false;
    };

    std::array<Slot, 3> m_slots{};
    std::uint32_t m_nextSlot = 0; // 下一个 Enqueue 使用的 slot（轮转）
    std::uint32_t m_oldest = 0;   // 最老的 pending slot（Poll 从这里开始）
    std::uint32_t m_pendingCount = 0;
};
} // namespace MiniEngine::Rhi::D3D11
