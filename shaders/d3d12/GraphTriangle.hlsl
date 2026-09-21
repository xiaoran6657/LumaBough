// M6-10 固定三角形；两种离线变体消费完全相同的无资源输入。
struct PixelInput { float4 position : SV_POSITION; float3 color : COLOR0; };
PixelInput VSMain(uint id : SV_VertexID)
{
    const float2 positions[3] = { float2(-0.75, -0.75), float2(0, 0.75), float2(0.75, -0.75) };
    const float3 colors[3] = { float3(1,0,0), float3(0,1,0), float3(0,0,1) };
    PixelInput result;
    result.position = float4(positions[id], 0.5, 1);
    result.color = colors[id];
    return result;
}
float4 PSMain(PixelInput input) : SV_TARGET { return float4(input.color, 1); }
