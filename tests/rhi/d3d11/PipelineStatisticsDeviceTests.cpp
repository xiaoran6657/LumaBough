// ============================================================================
// PipelineStatisticsDeviceTests.cpp — D3D11_QUERY_PIPELINE_STATISTICS 设备级交叉证据
// 里程碑：M4-09（08 篇「计数器」的遗留项 G-2；M4-09 审查要求补齐）
// 职责：在裸设备上对**已知几何量**的 draw 采集 pipeline statistics，断言
//       IAVertices/IAPrimitives/VSInvocations/GSPrimitives 等与提交的顶点/图元数
//       精确一致。这是 08 篇"可在诊断 run 中验证 primitive/pixel shader
//       invocation"的单元化形态：
//         - 证明渲染器报出的 draw/triangle 计数（06 篇 CullingStats、benchmark
//           报告的 drawsMedian）与 GPU 实际执行的输入装配量同源；
//         - PSInvocations 由驱动按实际覆盖像素计，只做下界断言（≥ 图元数），
//           不做精确契约（它不是应用可固定的量）；
//         - 未绑定的可选阶段（HS/DS/GS）invocation 与 GS 原语必须为 0。
// 关联：docs/architecture/README.md「计数器」
//       docs/architecture/DECISIONS.md §1 缺口 G-2
// ============================================================================

#include <gtest/gtest.h>

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

// 调试层优先，SDK 缺失时回退 retail（与 GpuTimestampDeviceTests 同款口径）。
ComPtr<ID3D11Device> CreateTestDevice(ComPtr<ID3D11DeviceContext>& context)
{
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevel{};
    constexpr UINT kDebugFlags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kDebugFlags, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &featureLevel, &context);
    }
    return device;
}

// 最小着色器：VS 透传 clip 位置，PS 输出常量色。内嵌字符串而非引用仓库 shader，
// 保证统计契约不随正式管线变化（本测试只关心输入装配与 invocation 语义）。
constexpr char kVertexShader[] = R"(float4 VSMain(float4 position : POSITION) : SV_Position
{
    return position;
})";

constexpr char kPixelShader[] = R"(float4 PSMain() : SV_Target
{
    return float4(0.0, 0.0, 0.0, 1.0);
})";

ComPtr<ID3D11VertexShader> CompileVertexShader(ID3D11Device& device, ComPtr<ID3DBlob>& vsBlob)
{
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT result =
        D3DCompile(kVertexShader, sizeof(kVertexShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                   vsBlob.ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    EXPECT_EQ(result, S_OK) << "D3DCompile(vs_5_0) failed";
    if (FAILED(result))
    {
        return {};
    }
    ComPtr<ID3D11VertexShader> shader;
    EXPECT_EQ(device.CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                        shader.ReleaseAndGetAddressOf()),
              S_OK);
    return shader;
}

ComPtr<ID3D11PixelShader> CompilePixelShader(ID3D11Device& device)
{
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT result = D3DCompile(kPixelShader, sizeof(kPixelShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
                                      0, 0, psBlob.ReleaseAndGetAddressOf(), errorBlob.ReleaseAndGetAddressOf());
    EXPECT_EQ(result, S_OK) << "D3DCompile(ps_5_0) failed";
    if (FAILED(result))
    {
        return {};
    }
    ComPtr<ID3D11PixelShader> shader;
    EXPECT_EQ(device.CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                       shader.ReleaseAndGetAddressOf()),
              S_OK);
    return shader;
}

// 采集一次 draw 的 pipeline statistics：Begin → draw → End → 阻塞读取。
// 统计查询不是逐帧路径（08 篇：不要默认在 baseline frame 上启用），这里的
// 阻塞等待是诊断语义，与 D3D11GpuTimer 的"绝不 busy-wait"约定不冲突。
bool CollectStatistics(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11VertexShader& vertexShader,
                       ID3D11PixelShader& pixelShader, const std::vector<float>& vertices, const UINT vertexCount,
                       D3D11_QUERY_DATA_PIPELINE_STATISTICS& stats)
{
    context.VSSetShader(&vertexShader, nullptr, 0);
    context.PSSetShader(&pixelShader, nullptr, 0);
    ComPtr<ID3D11Buffer> vertexBuffer;
    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDescription.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(float));
    bufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initialData{vertices.data(), 0, 0};
    if (FAILED(device.CreateBuffer(&bufferDescription, &initialData, vertexBuffer.ReleaseAndGetAddressOf())))
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = 8;
    textureDescription.Height = 8;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device.CreateTexture2D(&textureDescription, nullptr, texture.ReleaseAndGetAddressOf())))
    {
        return false;
    }
    ComPtr<ID3D11RenderTargetView> renderTarget;
    if (FAILED(device.CreateRenderTargetView(texture.Get(), nullptr, renderTarget.ReleaseAndGetAddressOf())))
    {
        return false;
    }

    ComPtr<ID3D11Query> query;
    D3D11_QUERY_DESC queryDescription{};
    queryDescription.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    if (FAILED(device.CreateQuery(&queryDescription, query.ReleaseAndGetAddressOf())))
    {
        return false;
    }

    context.OMSetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
    // D3D11 没有默认视口：不设 viewport 时光栅化覆盖 0 像素 → PSInvocations=0。
    const D3D11_VIEWPORT viewport{0.0F, 0.0F, 8.0F, 8.0F, 0.0F, 1.0F};
    context.RSSetViewports(1, &viewport);
    // CULL_NONE：本测试只关心统计与提交量的同源性，绕序方向（受视口 Y 翻转影响）
    // 不应参与断言。
    ComPtr<ID3D11RasterizerState> rasterizer;
    D3D11_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    if (FAILED(device.CreateRasterizerState(&rasterizerDescription, rasterizer.ReleaseAndGetAddressOf())))
    {
        return false;
    }
    context.RSSetState(rasterizer.Get());
    const UINT stride = 16U; // float4 POSITION
    UINT offset = 0;
    context.IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context.Begin(query.Get());
    context.Draw(vertexCount, 0);
    context.End(query.Get());
    context.Flush();

    // 阻塞读取：诊断一次性查询，等待完成（GetData 带 flush 旗标）。
    while (true)
    {
        const HRESULT result = context.GetData(query.Get(), &stats, sizeof(stats), 0);
        if (result == S_OK)
        {
            return true;
        }
        if (FAILED(result))
        {
            return false;
        }
        Sleep(0);
    }
}

// 公共装配：设备 + 最小管线（VS/PS/InputLayout）。
struct StatisticsRig final
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
};

[[nodiscard]] bool CreateRig(StatisticsRig& rig)
{
    rig.context = nullptr;
    rig.device = CreateTestDevice(rig.context);
    if (rig.device == nullptr || rig.context == nullptr)
    {
        return false;
    }
    ComPtr<ID3DBlob> vsBlob;
    rig.vertexShader = CompileVertexShader(*rig.device.Get(), vsBlob);
    rig.pixelShader = CompilePixelShader(*rig.device.Get());
    if (rig.vertexShader == nullptr || rig.pixelShader == nullptr || vsBlob == nullptr)
    {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> inputLayout;
    if (FAILED(rig.device->CreateInputLayout(layout, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                             inputLayout.ReleaseAndGetAddressOf())))
    {
        return false;
    }
    rig.context->IASetInputLayout(inputLayout.Get());
    rig.context->VSSetShader(rig.vertexShader.Get(), nullptr, 0);
    rig.context->PSSetShader(rig.pixelShader.Get(), nullptr, 0);
    return true;
}
} // namespace

// 已知几何量 → 统计精确匹配：2 个三角形（6 顶点）。
TEST(PipelineStatisticsDeviceTests, StatisticsMatchSubmittedGeometry)
{
    StatisticsRig rig;
    ASSERT_TRUE(CreateRig(rig));

    // 两个位于 clip 空间中部的三角形。
    const std::vector<float> vertices{
        // clang-format off
        -0.5F, -0.5F, 0.0F, 1.0F,   0.5F, -0.5F, 0.0F, 1.0F,   0.0F,  0.5F, 0.0F, 1.0F,
         0.5F,  0.5F, 0.0F, 1.0F,  -0.5F,  0.5F, 0.0F, 1.0F,   0.0F, -0.5F, 0.0F, 1.0F,
        // clang-format on
    };

    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    ASSERT_TRUE(CollectStatistics(*rig.device.Get(), *rig.context.Get(), *rig.vertexShader.Get(),
                                  *rig.pixelShader.Get(), vertices, 6U, stats));

    // 输入装配：提交 6 顶点 / 2 三角形 / TRIANGLELIST；VS 每个后装配顶点调用一次。
    EXPECT_EQ(stats.IAVertices, 6ULL);
    EXPECT_EQ(stats.IAPrimitives, 2ULL);
    EXPECT_EQ(stats.VSInvocations, 6ULL);
    // 未绑定可选阶段：GS/HS/DS 的 invocation 与 GS 输出原语必须为 0。
    EXPECT_EQ(stats.GSInvocations, 0ULL);
    EXPECT_EQ(stats.GSPrimitives, 0ULL);
    EXPECT_EQ(stats.HSInvocations, 0ULL);
    EXPECT_EQ(stats.DSInvocations, 0ULL);
    // PS 按实际覆盖像素调用（驱动定义），只断言下界：至少每三角形一次。
    EXPECT_GE(stats.PSInvocations, 2ULL);
}

// 不同几何量产出不同的统计——证明查询跟踪的是真实提交，而不是恒定噪声。
TEST(PipelineStatisticsDeviceTests, StatisticsDistinguishDifferentDrawSizes)
{
    StatisticsRig rig;
    ASSERT_TRUE(CreateRig(rig));

    const std::vector<float> twoTriangles{
        // clang-format off
        -0.5F, -0.5F, 0.0F, 1.0F,   0.5F, -0.5F, 0.0F, 1.0F,   0.0F,  0.5F, 0.0F, 1.0F,
         0.5F,  0.5F, 0.0F, 1.0F,  -0.5F,  0.5F, 0.0F, 1.0F,   0.0F, -0.5F, 0.0F, 1.0F,
        // clang-format on
    };
    const std::vector<float> oneTriangle{
        // clang-format off
        -0.5F, -0.5F, 0.0F, 1.0F,   0.5F, -0.5F, 0.0F, 1.0F,   0.0F,  0.5F, 0.0F, 1.0F,
        // clang-format on
    };

    D3D11_QUERY_DATA_PIPELINE_STATISTICS two{};
    D3D11_QUERY_DATA_PIPELINE_STATISTICS one{};
    ASSERT_TRUE(CollectStatistics(*rig.device.Get(), *rig.context.Get(), *rig.vertexShader.Get(),
                                  *rig.pixelShader.Get(), twoTriangles, 6U, two));
    ASSERT_TRUE(CollectStatistics(*rig.device.Get(), *rig.context.Get(), *rig.vertexShader.Get(),
                                  *rig.pixelShader.Get(), oneTriangle, 3U, one));

    EXPECT_EQ(two.IAVertices, 6ULL);
    EXPECT_EQ(one.IAVertices, 3ULL);
    EXPECT_EQ(two.IAPrimitives, 2ULL);
    EXPECT_EQ(one.IAPrimitives, 1ULL);
    EXPECT_EQ(two.VSInvocations, 6ULL);
    EXPECT_EQ(one.VSInvocations, 3ULL);
}
