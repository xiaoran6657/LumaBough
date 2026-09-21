// ============================================================================
// ShadowDepth.hlsl — shadow pass depth-only 顶点着色器（D3D12 / SM6 侧）
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；与 M4 同源）
// 职责：把 caster 顶点变换到光空间 clip 坐标；无 pixel shader（opaque depth-only）。
//       b1 布局必须与 PbrForward.hlsl 逐字节一致（M4-02/03 硬约束）——本文件与
//       PbrForward 的 cbuffer ObjectConstants 声明同步维护，任何字段增删两侧同改
//       并重跑 shader 编译 gate。
// 与 M4 的关系：逐行同源移植；寄存器改用契约宏（ME_OBJECT_CB_REGISTER = b1）。
// 关联：shaders/d3d12/PbrForward.hlsl（b1 逐字节一致性）
//       shaders/d3d11/ShadowDepth.hlsl（M4 同源唯一实现）
// ============================================================================
#include "BindingContract.hlsli"

// 矩阵约定与 PbrForward 相同：CPU 侧已转置的行主序，mul(vector, matrix)。
cbuffer ObjectConstants : register(ME_OBJECT_CB_REGISTER)
{
    float4x4 World;                     // caster world 矩阵
    float4x4 NormalMatrix;              // shadow pass 不用，占位保持 b1 逐字节一致
    float4x4 LightWorldViewProjection;  // 光空间 VP（固定 shadow volume，M4-01 数学）
    float4 HandednessAndReceivesShadow; // shadow pass 不用（x = 手性，y = receiver 开关）
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 VSMain(VertexInput input) : SV_POSITION
{
    return mul(mul(float4(input.position, 1.0), World), LightWorldViewProjection);
}

// 无 pixel shader：opaque depth-only pass（PSO 的 PS 为空、DSV = D32_FLOAT）。
