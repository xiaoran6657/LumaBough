// ============================================================================
// FullscreenTriangle.hlsli — SV_VertexID 全屏三角形（04/05 篇共用的无 VB 顶点生成）
// 里程碑：M4（04 篇 IntegrateBrdf 首个消费方；05 篇 ToneMap 复用）
// 职责：由 SV_VertexID 0..2 生成覆盖全屏的大三角形（(-1,-1), (-1,3), (3,-1)），
//       输出裁剪位置与 0..1 的 UV（y 翻转对齐纹理行方向）。省去顶点缓冲与
//       输入布局——BRDF LUT / tone map 这类逐像素全屏 pass 的标准形态。
// 关联：docs/architecture/README.md「Fullscreen triangle」
//       shaders/d3d11/IntegrateBrdf.hlsl（VS 唯一调用方，05 篇为 ToneMap.hlsl）
// ============================================================================
#ifndef MINIENGINE_FULLSCREEN_TRIANGLE_HLSLI
#define MINIENGINE_FULLSCREEN_TRIANGLE_HLSLI

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenTriangleVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    // 0 → (-1,-1)；1 → (-1,3)；2 → (3,-1)：超屏三角形保证每像素恰好覆盖一次。
    const float2 position = float2(vertexId == 2 ? 3.0 : -1.0, vertexId == 1 ? 3.0 : -1.0);
    output.position = float4(position, 0.0, 1.0);
    // UV 与屏幕像素对齐：x 右增，y 下增（与 2D 纹理 v 轴同向，LUT 轴契约依赖此式）。
    output.uv = float2(position.x * 0.5 + 0.5, -position.y * 0.5 + 0.5);
    return output;
}

#endif
