// ============================================================================
// BindingContract.hlsli — D3D12 侧寄存器绑定契约的 HLSL 镜像
// 里程碑：M5（05 篇 Descriptor Heap 与 Root Signature；手抄清单第 3 条）
// 职责：以宏给出 b/t/s 三组寄存器的唯一 HLSL 声明，与 C++ 侧
//       engine/rhi/d3d12/include/MiniEngine/Rhi/D3D12/D3D12RootBindings.h 逐项
//       对应（同一组编号、同一组顺序）。shader 通过 `register(ME_XXX_REGISTER)`
//       引用，避免在多个 .hlsl 里各写一遍字面量导致漂移。
// 与 M4 D3D11 的关系：寄存器编号完全一致（同一份材质/常量数据在两后端落到同一
//       组槽位）；差异只在绑定机制——D3D11 逐槽 Set*，D3D12 用 root CBV（b0—b3）
//       与两个 descriptor table（t0—t4 / t5—t8），采样器为 static sampler。
// 变更纪律：本文件与 D3D12RootBindings.h 必须同批修改；两边都有静态检查
//       （C++ 是 static_assert，HLSL 侧由 M5-08 的 DXC 编译 gate 兜底）。
// 关联：docs/architecture/README.md（Root Signature baseline）
//       shaders/d3d11/PbrCommon.hlsli（M4 同源寄存器声明，编号一致）
// ============================================================================

#ifndef MINIENGINE_D3D12_BINDING_CONTRACT_HLSLI
#define MINIENGINE_D3D12_BINDING_CONTRACT_HLSLI

// Root parameter 0—3：root CBV b0—b3（b0/b1 为 ALL，b2/b3 为 PIXEL）。
#define ME_FRAME_CB_REGISTER b0
#define ME_OBJECT_CB_REGISTER b1
#define ME_MATERIAL_CB_REGISTER b2
#define ME_IBL_CB_REGISTER b3

// Root parameter 4：descriptor table t0—t4（材质贴图，PIXEL）。
#define ME_BASE_COLOR_REGISTER t0
#define ME_NORMAL_REGISTER t1
#define ME_METALLIC_ROUGHNESS_REGISTER t2
#define ME_OCCLUSION_REGISTER t3
#define ME_EMISSIVE_REGISTER t4

// Root parameter 5：descriptor table t5—t8（IBL 与阴影，PIXEL）。
#define ME_IRRADIANCE_REGISTER t5
#define ME_PREFILTER_REGISTER t6
#define ME_BRDF_LUT_REGISTER t7
#define ME_SHADOW_REGISTER t8

// Static samplers（不进 descriptor heap）：s0 材质、s1 IBL/后处理、s2 shadow comparison。
#define ME_MATERIAL_SAMPLER_REGISTER s0
#define ME_IBL_SAMPLER_REGISTER s1
#define ME_SHADOW_SAMPLER_REGISTER s2

// 槽位连续性：table 的连续性由 C++ 侧 static_assert 与本文件宏的相邻编号共同保证；
// 若新增/调整槽位，必须同步 D3D12RootBindings.h 的 kMaterialSrvCount / kGlobalSrvCount。

#endif // MINIENGINE_D3D12_BINDING_CONTRACT_HLSLI
