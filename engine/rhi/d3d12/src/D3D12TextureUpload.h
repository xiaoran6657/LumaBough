// ============================================================================
// D3D12TextureUpload.h — 基于 GetCopyableFootprints 的纹理/缓冲上传
// 里程碑：M5（06 篇 Upload Ring、资源上传与生命周期；手抄清单第 4 条）
// 职责：把"CPU 字节 → DEFAULT heap 资源"的固定流程收敛成可测的三个步骤：
//   1) PlanTextureUpload：用 ID3D12Device::GetCopyableFootprints 取得每个
//      mip/subresource 的 placed footprint、row count、row size 与总字节；
//      **绝不自己猜 256/512 padding**，也不把纹理建在 UPLOAD heap；
//   2) PackTextureRows：把源数据**逐行**拷进上传缓冲——源 pitch 与目标
//      Footprint.RowPitch 是两个不同的量，逐行复制才不会串行；
//   3) RecordTextureCopies / RecordTextureTransition：按 subresource 发出
//      CopyTextureRegion，并在全部 subresource 完成后 transition 到 PS_RESOURCE。
// subresource index 一律用 D3D12CalcSubresource 计算（不手写 mip + slice*levels）。
// 内部性说明：后端内部类型（src/）。
// 关联：docs/architecture/README.md（Texture upload / Static buffer upload）
// ============================================================================
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniEngine::Rhi::D3D12
{
// subresource 线性下标（06 篇要求"用 D3D12CalcSubresource 或等价已测试函数"）。
//
// 为什么不直接用 D3D12CalcSubresource：本机 Windows SDK 10.0.26100.0 的 d3d12.h
// **不提供**该函数（它属于 D3DX12 辅助头，我们没有引入 DirectX-Headers 依赖）。
// 因此这里给出等价实现，并由单元测试锁定排序语义：
//   index = mipSlice + arraySlice * mipLevels + planeSlice * mipLevels * arraySize
// 即"先 mip、再 slice、最后 plane"。参数与 D3D12CalcSubresource 完全一致，便于日后
// 换成 SDK/D3DX12 版本时逐参对照。
[[nodiscard]] constexpr std::uint32_t CalcSubresourceIndex(std::uint32_t mipSlice, std::uint32_t arraySlice,
                                                           std::uint32_t planeSlice, std::uint32_t mipLevels,
                                                           std::uint32_t arraySize) noexcept
{
    return mipSlice + arraySlice * mipLevels + planeSlice * mipLevels * arraySize;
}

// 单个 subresource 的上传计划（相对上传缓冲起点的偏移已在 placement.Offset 内）。
struct TextureSubresourcePlan final
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placement{};
    std::uint32_t rowCount = 0;
    std::uint64_t rowSize = 0;    // 未对齐的"有效行字节数"（source 每行的有效长度）
    std::uint64_t totalBytes = 0; // 该 subresource 占用的上传字节（含行间 padding）
};

// 一次纹理上传的完整计划。
struct TextureUploadPlan final
{
    std::vector<TextureSubresourcePlan> subresources;
    std::uint64_t totalBytes = 0; // 全部 subresource 之和（= GetCopyableFootprints 的输出）
    std::uint32_t subresourceCount = 0;
};

// 源数据描述：一整块连续行（rowPitch 是源缓冲里两行之间的距离，可大于 rowSize）。
// byteSize 是源缓冲的**可用字节数**：PackTextureRows 会据此校验
// `(rowCount-1)*rowPitch + rowSize <= byteSize`——只校验目标越界是不够的，
// 传短 buffer 会越界读（审查意见 M5-06 P2-2）。
struct TextureUploadSource final
{
    const std::byte* data = nullptr;
    std::uint64_t rowPitch = 0;
    std::uint64_t byteSize = 0;
};

// 生成上传计划。
//
// 参数：
//   device      —— 查询 footprints
//   description —— 目标纹理描述（DEFAULT heap；格式/尺寸/arraySize/mipLevels 必须已定）
//   baseOffset  —— 上传缓冲内的起点偏移（必须按 512 对齐，06 篇「对齐」表）
// 返回：每个子资源（subresource 顺序 = D3D12CalcSubresource 的顺序）的计划与总量。
// 失败：baseOffset 未按 512 对齐 → std::invalid_argument；GetCopyableFootprints 返回
//   的总量为 0（非法描述）→ std::runtime_error。
[[nodiscard]] TextureUploadPlan PlanTextureUpload(ID3D12Device& device, const D3D12_RESOURCE_DESC& description,
                                                  std::uint64_t baseOffset);

// 把第 subresourceIndex 个源数据逐行拷进上传缓冲。
//
// 参数：
//   plan             —— PlanTextureUpload 的结果
//   subresourceIndex —— 目标子资源下标
//   uploadBase       —— 上传缓冲的 CPU 起点
//   uploadByteSize   —— 上传缓冲总大小（越界即失败，绝不越写）
//   source           —— 源数据与 source 行距
// 失败：下标越界、源/目标行距不足、写越界 → std::out_of_range / std::invalid_argument。
void PackTextureRows(const TextureUploadPlan& plan, std::uint32_t subresourceIndex, std::byte* uploadBase,
                     std::uint64_t uploadByteSize, const TextureUploadSource& source);

// 按 subresource 顺序发出 CopyTextureRegion（要求 destination 处于 COPY_DEST）。
//
// uploadBuffer 必须是计划对应的那个上传缓冲：placed footprint 的 Offset 是
// **缓冲内字节偏移**（不是 GPUVA），因此这里传资源而不是地址。
// 子资源下标用 D3D12CalcSubresource(mip, slice, 0, mipLevels, arraySize) 计算。
void RecordTextureCopies(ID3D12GraphicsCommandList& commandList, ID3D12Resource& destination,
                         const TextureUploadPlan& plan, ID3D12Resource& uploadBuffer);

// 单资源状态转换 helper（纹理与静态缓冲共用；before/after 必须真实一致）。
//
// 语义：`before == after` 时**静默跳过**（与 04 篇 D3D12SwapChain::Transition 一致）——
// 状态真伪由 07 篇的 ResourceStateTracker 判定，本函数只负责"发出这一条 barrier"。
void RecordResourceTransition(ID3D12GraphicsCommandList& commandList, ID3D12Resource& resource,
                              D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
} // namespace MiniEngine::Rhi::D3D12
