// ============================================================================
// ShadowDepth.hlsl — shadow pass depth-only 顶点着色器（03 篇「Shadow pass」）
// 里程碑：M4（03 篇 Directional Shadow）
// 职责：把 caster 顶点变换到光空间 clip 坐标；无 pixel shader（opaque depth-only，
//       M4 拒绝 alpha-masked 材质，caster 覆盖率 = 几何覆盖率）。b1 布局必须与
//       PbrForward.hlsl 逐字节一致（02/03 篇硬约束）——本文件与 PbrForward 的
//       cbuffer ObjectConstants 声明同步维护，任何字段增删两侧同改并重跑 gate。
// 关联：docs/architecture/README.md「Shadow pass」「Light matrix」
//       engine/rhi/d3d11/src/D3D11Renderer.cpp（shadow pass 装配；POSITION-only
//       input layout 从本 VS 字节码创建，stride 仍是 PbrVertex 48B）
// ============================================================================
// 矩阵约定与 PbrForward 相同：CPU 侧已转置的行主序，mul(vector, matrix)。
cbuffer ObjectConstants : register(b1)
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

// 无 pixel shader：opaque depth-only pass（03 篇 Shadow pass 入口清单 PSSetShader(null)）。
