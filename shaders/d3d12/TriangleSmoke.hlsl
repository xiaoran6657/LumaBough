// ============================================================================
// TriangleSmoke.hlsl — 首个 D3D12 shader：工具链闭环的最小目标（05/08 篇）
// 里程碑：M5（05 篇手抄清单第 5 条给出模板；08 篇接入 DXC 编译并产出 DXIL+manifest）
// 职责：只依赖 b0 root CBV 与顶点颜色，用来证明"HLSL → DXC(SM6) → DXIL → manifest"
//       这条链路成立。它**不是**渲染 pass 的入口（真正的 pass 入口见 PbrForward /
//       ShadowDepth / Skybox / ToneMap）。
// 为什么先有它：shader 编译链路的失败模式（target/entry/参数/PDB 名）与 PBR 数学
//       无关；用最小 shader 把链路钉住，之后任何一次编译失败都只能是"这个 shader
//       自己的问题"。
// 关联：docs/architecture/README.md（手抄清单第 4 条）
//       tools/shader_compiler（本文件的编译入口与其 manifest）
//       shaders/d3d12/BindingContract.hlsli（b0 契约）
// ============================================================================
#include "BindingContract.hlsli"

cbuffer FrameConstants : register(ME_FRAME_CB_REGISTER)
{
    float4x4 ViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0), ViewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
