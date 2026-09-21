// ============================================================================
// TexturedCube.hlsl — 纹理立方体的 SM5 顶点/像素着色器
// 里程碑：M2 / M3
// 职责：VSMain 变换立方体顶点到裁剪空间并透传世界法线与 UV，PSMain 采样棋盘纹理并按
//   基础方向光（0.2 环境项 + 0.8 Lambert 漫反射）着色。寄存器绑定 b0/t0/s0 与 C++ 端
//   的 SceneConstants、Checker SRV、LinearWrap 采样器一一对应。
// 关联：docs/architecture/README.md
// ============================================================================
// 矩阵约定：CPU 侧以 XMMatrixTranspose 后按行主序上传（D3D 经典行向量约定），
// 因此本 shader 使用 mul(vector, matrix)。布局与 C++ 的 SceneConstants
// （144 字节、16 字节对齐）一一对应。
cbuffer SceneConstants : register(b0)
{
    float4x4 worldViewProjection;
    float4x4 world;
    float4 cameraPositionAndTime;
};

// 每对象 tint（M3：glTF baseColorFactor，白 = 不改变）。独立小 CB 避免与
// 大矩阵 cbuffer 的打包偏移耦合。
cbuffer ObjectConstants : register(b1)
{
    float4 baseColorFactor;
};

// 纹理与采样器绑定：t0 对应 C++ 的 Checker Texture SRV，s0 对应 LinearWrap Sampler。
Texture2D baseColorTexture : register(t0);
SamplerState linearWrapSampler : register(s0);

// 顶点着色器输入，字段名与语义对应 C++ 输入布局（POSITION/NORMAL/TEXCOORD0）。
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

// 顶点着色器输出，向像素着色器传递裁剪空间位置、世界空间法线与 UV。
struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    // 行向量约定：mul(vector, matrix)；位置经 WVP 进入裁剪空间。
    output.position = mul(float4(input.position, 1.0F), worldViewProjection);
    // 法线用 world 矩阵的线性部分变换并归一化，w=0 消除平移影响。
    output.worldNormal = normalize(mul(float4(input.normal, 0.0F), world).xyz);
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // 基础方向光：固定朝向（斜向下），光照 = 0.2 环境项 + 0.8 Lambert 漫反射。
    const float3 lightDirection = normalize(float3(-0.4F, -0.8F, 0.3F));
    const float diffuse = saturate(dot(normalize(input.worldNormal), -lightDirection));
    const float lighting = 0.2F + 0.8F * diffuse;
    // 采样纹理（sRGB SRV 由硬件解码到线性），乘每对象 baseColorFactor 与光照。
    const float4 baseColor = baseColorTexture.Sample(linearWrapSampler, input.uv);
    return float4(baseColor.rgb * baseColorFactor.rgb * lighting, baseColor.a * baseColorFactor.a);
}
