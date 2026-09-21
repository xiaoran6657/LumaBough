// ============================================================================
// D3D12PsoFactory.cpp — PSO desc 构造、缓存与热重载事务的实现
// 里程碑：M5（08 篇 DXC、PSO 与 shader 迁移；手抄清单第 3 条）
// 职责：实现 D3D12PsoFactory.h。所有固定状态都在这里显式给出：
//   - input layout：PbrOpaque = position/normal/tangent/uv0（48B stride，与 M4 的
//     PbrVertex 一致）；Shadow 与 Skybox = position-only（stride 分别为 48B / 12B，
//     与 M4 的 shadow/cube 顶点流一致）；ToneMap = 空布局（SV_VertexID 全屏三角形）。
//   - 拓扑：triangle list（PSO 的 PrimitiveTopologyType；命令列表里不再 Set 拓扑）。
//   - rasterizer：Fill=SOLID、Cull 按 pass 分类（ToneMap/Skybox = NONE，与绕序解耦；
//     其余 = BACK）、FrontCounterClockwise = mirrored（镜像实例用另一个 PSO，而不是
//     在 draw 前改状态——D3D12 没有动态 rasterizer state）。
//   - depth：Shadow = LESS_EQUAL + 写深度；PBR = LESS_EQUAL + 写深度；Skybox =
//     LESS_EQUAL + **DepthWriteMask=ZERO**（远深度技巧，不污染深度）；ToneMap = 关深度。
//   - blend：全部 opaque（不透明；后续篇目引入透明时再扩展 key）。
// 失败诊断：D3D12 没有 PSO 序列化错误 blob；失败时把 HRESULT + pass 名 + mirrored +
//   布局名写进异常文本，细节由调试层（InfoQueue）给出。
// 关联：docs/architecture/README.md（format 表 / PSO desc 清单）
// ============================================================================
#include "D3D12PsoFactory.h"

#include <MiniEngine/Core/Log.h>
#include <MiniEngine/Rhi/D3D12/D3D12Common.h>

#include "D3D12Diagnostics.h"

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>

namespace MiniEngine::Rhi::D3D12
{
namespace
{
// 窄 → 宽：`ID3D12Object::SetName` 要宽字符（调试名按仓库惯例以窄字符拼装）。
std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
// ---- input layout 存储：desc 里的指针必须在 CreateGraphicsPipelineState 调用期间有效 ----
struct InputLayoutStorage final
{
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    D3D12_INPUT_LAYOUT_DESC description{};

    void Publish()
    {
        description.pInputElementDescs = elements.empty() ? nullptr : elements.data();
        description.NumElements = static_cast<UINT>(elements.size());
    }
};

InputLayoutStorage BuildInputLayout(const PassKind pass)
{
    InputLayoutStorage storage;
    switch (pass)
    {
    case PassKind::PbrOpaque:
        // 与 M4 PbrVertex 一致：position(0) normal(12) tangent(24) uv0(40)，stride 48。
        storage.elements = {
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
            {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
            {"TANGENT", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 24U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
            {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 40U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        };
        break;
    case PassKind::Shadow:
        // shadow caster 用同一份 48B 顶点流，只取 position（stride 由命令列表给出）。
        storage.elements = {
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        };
        break;
    case PassKind::Skybox:
        // 单位立方体位置（stride 12）。
        storage.elements = {
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        };
        break;
    case PassKind::TriangleSmoke:
        // 与 shaders/d3d12/TriangleSmoke.hlsl 的 VertexInput 逐字段对应：
        // position(0) color(12)，stride 24。颜色是顶点属性而非常量——它让"顶点流真的
        // 被读到了"这件事在像素上可见（三个顶点三种颜色，插值错误会立刻看出来）。
        storage.elements = {
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
            {"COLOR", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        };
        break;
    case PassKind::ToneMap:
    case PassKind::BrdfLut:
        // 全屏三角形：无顶点缓冲（SV_VertexID）。
        break;
    default:
        // IBL bake pass 的几何随 09 篇接线（cube 面几何 = position-only）。
        storage.elements = {
            {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        };
        break;
    }
    storage.Publish();
    return storage;
}

D3D12_RASTERIZER_DESC BuildRasterizerState(const PassKind pass, const bool mirrored)
{
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = CullModeForPass(pass);
    // 镜像实例的手性反转靠 winding 取反，而不是动态改状态（08 篇规定）。
    // 取值口径见 FrontCounterClockwiseIsFront 的注释（与 M4 一致：非镜像 = CCW 正面）。
    rasterizer.FrontCounterClockwise = FrontCounterClockwiseIsFront(mirrored) ? TRUE : FALSE;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    // M4 固定场景校准终值：DepthBias=0，SlopeBias=0.1，Clamp=0。
    rasterizer.SlopeScaledDepthBias = pass == PassKind::Shadow ? 0.1F : 0.0F;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0U;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return rasterizer;
}

D3D12_BLEND_DESC BuildOpaqueBlendState()
{
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget)
    {
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return blend;
}

D3D12_DEPTH_STENCIL_DESC BuildDepthState(const PassKind pass)
{
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.StencilEnable = FALSE;
    switch (pass)
    {
    case PassKind::ToneMap:
    case PassKind::BrdfLut:
    case PassKind::EquirectToCube:
    case PassKind::Irradiance:
    case PassKind::Prefilter:
    case PassKind::EnvironmentDownsample:
        // 全屏/IBL bake pass：无深度测试、无深度写入。
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        break;
    case PassKind::Skybox:
        // 远深度技巧：只在 depth==1 处通过，且不写深度（否则天空会遮住后续几何）。
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        break;
    default:
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        break;
    }
    return depth;
}
} // namespace

const char* PassKindName(const PassKind pass) noexcept
{
    switch (pass)
    {
    case PassKind::Shadow:
        return "Shadow";
    case PassKind::PbrOpaque:
        return "PbrOpaque";
    case PassKind::Skybox:
        return "Skybox";
    case PassKind::ToneMap:
        return "ToneMap";
    case PassKind::EquirectToCube:
        return "EquirectToCube";
    case PassKind::Irradiance:
        return "Irradiance";
    case PassKind::Prefilter:
        return "Prefilter";
    case PassKind::BrdfLut:
        return "BrdfLut";
    case PassKind::TriangleSmoke:
        return "TriangleSmoke";
    case PassKind::EnvironmentDownsample:
        return "EnvironmentDownsample";
    default:
        return "Unknown";
    }
}

bool FrontCounterClockwiseIsFront(const bool mirrored) noexcept
{
    // 与 M4/D3D11 的唯一取齐点（见头注释）：M4 非镜像用 `FrontCounterClockwise = TRUE`
    // （烘焙网格 CCW 正面），镜像用 FALSE。这里保持同一语义，镜像只是取反。
    return !mirrored;
}

D3D12_CULL_MODE CullModeForPass(const PassKind pass) noexcept
{
    // 与 M4/D3D11 的唯一取齐点：noCull 光栅化覆盖"绕序不携带语义"的两类 pass
    // （tone map 的超屏三角形、skybox 的内表面，见 D3D11Renderer.cpp 的 noCullDesc）。
    // 其余 pass 走 CULL_BACK，镜像手性由 FrontCounterClockwiseIsFront 表达。
    // 注：IBL 生成四 pass（EquirectToCube 等）随第 7 步接入时再归类——它们不在
    // baseline PSO 集里，当前不被任何路径消费。
    switch (pass)
    {
    case PassKind::ToneMap:
    case PassKind::Skybox:
    case PassKind::EquirectToCube:
    case PassKind::Irradiance:
    case PassKind::Prefilter:
    case PassKind::EnvironmentDownsample:
    case PassKind::BrdfLut:
        return D3D12_CULL_MODE_NONE;
    default: // Shadow / PbrOpaque / TriangleSmoke（以及防御性的 Unknown）：正常几何。
        return D3D12_CULL_MODE_BACK;
    }
}

PassFormatEntry PassFormats(const PassKind pass) noexcept
{
    // 08 篇「格式必须匹配」表：这是与 PSO desc 同源的一处定义（测试直接断言它）。
    PassFormatEntry entry;
    switch (pass)
    {
    case PassKind::Shadow:
        entry.renderTargetCount = 0U; // depth-only
        entry.renderTargetFormat = DXGI_FORMAT_UNKNOWN;
        entry.depthStencilFormat = DXGI_FORMAT_D32_FLOAT;
        break;
    case PassKind::PbrOpaque:
    case PassKind::Skybox:
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        entry.depthStencilFormat = kMainDepthFormat;
        break;
    case PassKind::TriangleSmoke:
        // 09 篇迁移顺序第 1 步：HDR target 尚未落地（缺口③），因此固定三角形直接写
        // **后备缓冲**（R8G8B8A8_UNORM，与 04 篇交换链一致）；深度用主深度格式，
        // 让本步同时把"主深度资源可用"这件事证出来（09 篇「M4 resource profile 保持」
        // 要求 main depth 与 M4 相同）。HDR + tone map 是第 4 步，届时本 pass 才改指 HDR。
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        entry.depthStencilFormat = kMainDepthFormat;
        break;
    case PassKind::ToneMap:
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        entry.depthStencilFormat = DXGI_FORMAT_UNKNOWN;
        break;
    case PassKind::BrdfLut:
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R16G16_FLOAT;
        entry.depthStencilFormat = DXGI_FORMAT_UNKNOWN;
        break;
    case PassKind::EquirectToCube:
    case PassKind::Irradiance:
    case PassKind::Prefilter:
    case PassKind::EnvironmentDownsample:
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        entry.depthStencilFormat = DXGI_FORMAT_UNKNOWN;
        break;
    default:
        entry.renderTargetCount = 1U;
        entry.renderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        entry.depthStencilFormat = DXGI_FORMAT_UNKNOWN;
        break;
    }
    return entry;
}

void D3D12PsoFactory::Initialize(ID3D12Device& device, ID3D12RootSignature& rootSignature,
                                 const std::uint64_t rootSignatureRevision)
{
    if (m_device != nullptr)
    {
        throw std::logic_error{"D3D12PsoFactory::Initialize called twice"};
    }
    m_device = &device;
    m_rootSignature = &rootSignature;
    m_rootSignatureRevision = rootSignatureRevision;
}

std::string D3D12PsoFactory::MakePsoName(const PsoKey& key)
{
    // 名字里带上 key 的全部维度（PIX 里一眼能分辨 mirrored 与 revision）。
    return std::format("M5.D3D12.Pso.{}.{}.s{}.r{}", PassKindName(key.pass), key.mirrored ? "mirrored" : "normal",
                       key.shaderRevision, key.rootSignatureRevision);
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> D3D12PsoFactory::CreatePipelineState(const PsoKey& key,
                                                                                 const PsoShaderSet& shaders,
                                                                                 std::string& outDebugName)
{
    if (shaders.vertexShader.empty())
    {
        throw std::invalid_argument{"PSO requires vertex shader bytecode"};
    }

    InputLayoutStorage inputLayout = BuildInputLayout(key.pass);
    const PassFormatEntry formats = PassFormats(key.pass);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature;
    description.VS = {shaders.vertexShader.data(), shaders.vertexShader.size()};
    description.PS = shaders.pixelShader.empty()
                         ? D3D12_SHADER_BYTECODE{}
                         : D3D12_SHADER_BYTECODE{shaders.pixelShader.data(), shaders.pixelShader.size()};
    description.BlendState = BuildOpaqueBlendState();
    description.SampleMask = UINT_MAX;
    description.RasterizerState = BuildRasterizerState(key.pass, key.mirrored);
    description.DepthStencilState = BuildDepthState(key.pass);
    description.InputLayout = inputLayout.description;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = formats.renderTargetCount;
    description.RTVFormats[0] = formats.renderTargetFormat;
    description.DSVFormat = formats.depthStencilFormat;
    description.SampleDesc.Count = 1U; // baseline：无 MSAA（08 篇固定）

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    const HRESULT hr = m_device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipelineState));
    if (FAILED(hr))
    {
        ++m_stats.failedCreations;
        // D3D12 没有 PSO 序列化错误 blob：异常文本给出定位用的全部维度，细节看调试层。
        throw std::runtime_error{"CreateGraphicsPipelineState failed (HRESULT " + std::to_string(hr) + ") for pass=" +
                                 PassKindName(key.pass) + " mirrored=" + (key.mirrored ? "true" : "false") +
                                 " rtv=" + std::to_string(static_cast<int>(formats.renderTargetFormat)) +
                                 " dsv=" + std::to_string(static_cast<int>(formats.depthStencilFormat)) +
                                 " inputElements=" + std::to_string(inputLayout.elements.size())};
    }

    // 调试名必须真的设到 COM 对象上：PIX/调试层按它显示 PSO（05 篇的命名约定
    // `M5.D3D12.*`，名字里带 pass/mirrored/revision 四个维度）。只在日志里打印名字
    // 等于没命名——PIX 里会退回 "Pipeline State" 这类无法分辨的显示。
    outDebugName = MakePsoName(key);
    Internal::SetDebugName(pipelineState.Get(), Widen(outDebugName));
    return pipelineState;
}

ID3D12PipelineState& D3D12PsoFactory::GetOrCreate(const PsoKey& key, const PsoShaderSet& shaders)
{
    if (m_device == nullptr)
    {
        throw std::logic_error{"D3D12PsoFactory::GetOrCreate before Initialize"};
    }

    std::map<PsoKey, Entry>& target = m_hotReloadPending ? m_pending : m_current;
    if (const auto existing = target.find(key); existing != target.end())
    {
        ++m_stats.cacheHits;
        return *existing->second.pipelineState.Get();
    }
    // 热重载事务期间若当前集已有同名 key，仍创建新对象（它是"替换集"）；
    // 非事务期间命中缓存即返回（上面已处理）。

    const auto start = std::chrono::steady_clock::now();
    std::string debugName;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState = CreatePipelineState(key, shaders, debugName);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    m_stats.created += 1U;
    m_stats.creationMicroseconds += static_cast<std::uint64_t>(elapsed.count());

    Entry entry;
    entry.pipelineState = pipelineState;
    entry.formats = PassFormats(key.pass);
    entry.debugName = debugName;
    const auto [iterator, inserted] = target.emplace(key, std::move(entry));
    static_cast<void>(inserted);

    MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 pso created: " + debugName +
                                                         " vs=" + std::to_string(shaders.vertexShader.size()) +
                                                         " ps=" + std::to_string(shaders.pixelShader.size()) +
                                                         " creationUs=" + std::to_string(elapsed.count()));
    return *iterator->second.pipelineState.Get();
}

void D3D12PsoFactory::BeginHotReload(const std::uint64_t nextShaderRevision)
{
    if (m_hotReloadPending)
    {
        throw std::logic_error{"hot reload already pending"};
    }
    m_hotReloadPending = true;
    m_pending.clear();
    static_cast<void>(nextShaderRevision); // revision 由调用方放进 PsoKey（单一真源）
}

void D3D12PsoFactory::CommitHotReload(const std::uint64_t retireFenceValue)
{
    if (!m_hotReloadPending)
    {
        throw std::logic_error{"CommitHotReload without BeginHotReload"};
    }

    // 空替换集是调用方错误，必须显式失败（二次审查 N-1）。
    //
    // 语义收敛为"Commit 后当前集 == 替换集"之后，`Begin` 之后**不创建任何 PSO** 就
    // Commit 会把当前集**清空**（`CachedPsoCount() == 0`）——修复前该情形留下
    // baselineCount 个空条目，同样是坏状态，但"看起来还有 6 个"更难被 API 使用者察觉。
    // 替换集永远不该为空（生产路径由 `CreateBaselineSetForRevision` 保证至少 6 个），
    // 因此这里直接拒绝，而不是安静地把渲染器变成"没有任何 PSO"的状态。
    //
    // 抛出发生在任何状态变更之前：事务仍处于 pending，调用方可以补齐替换集后再 Commit，
    // 或直接 Abort（旧集始终可用）。
    if (m_pending.empty())
    {
        throw std::logic_error{"CommitHotReload with an empty replacement set"};
    }

    // 旧集**全部**按最后引用 fence 延迟释放，不止"新集没有的 key"。
    //
    // 原实现（审查 P3-2）跳过了 pending 将覆盖的同名 key——但覆盖时
    // `m_current[key] = std::move(entry)` 会**立即析构**旧 Entry 的 ComPtr，
    // 而旧 PSO 可能仍被在飞命令列表引用，违反 08 篇"旧 PSO 以最后引用 fence 释放"。
    // 生产路径（revision 严格递增 → key 全不重叠）不会触发这条，但 API 必须两条路径
    // 都安全：同名 key 的新 PSO 是**另一个对象**，旧对象照样要等 fence。
    for (auto& [key, entry] : m_current)
    {
        static_cast<void>(key);
        Microsoft::WRL::ComPtr<ID3D12PipelineState> retired = std::move(entry.pipelineState);
        m_deferred.Retire(retireFenceValue,
                          [retired, name = entry.debugName]() mutable
                          {
                              MiniEngine::WriteLog(MiniEngine::LogLevel::Info, "d3d12 pso retired: " + name);
                              retired.Reset();
                          });
    }

    // 旧集必须**整体移除**，不能只把 pending 里出现的 key 覆盖掉（审查 F-1）。
    //
    // 上面的 retire 循环已经把每个 entry 的 ComPtr 移进延迟释放队列，条目本身留在 map 里；
    // 若发布循环只做 `m_current[key] = std::move(entry)`，旧 key 就会以
    // `pipelineState == nullptr` 的"空死条目"形式永久残留，后果有三：
    //   1) `CachedPsoCount()`（头文件把它文档化为"当前生效集"）随热重载线性增长
    //      （6 → 12 → 18），不变式当场破裂；
    //   2) 第 2 次及以后 Commit 会对已经是 null 的条目再 Retire 一次 → 每轮多出
    //      `baselineCount` 条虚假 "pso retired" 日志与延迟释放项；
    //   3) `GetOrCreate` 命中这种条目会返回 `*nullptr`（未定义行为）。
    // 生产路径 revision 严格递增、key 全不重叠，因此 (3) 当前不可达——但 (1)(2) 立刻可见，
    // 且 API 必须两条路径都安全。
    //
    // 语义由此收敛为"Commit 后当前集 == 替换集"，与上面"旧集全部 Retire"一致：
    // 既然旧集被整体退休，未被 pending 覆盖的 key 就不可能继续有效，留在 map 里只会是空条目。
    m_current.clear();
    for (auto& [key, entry] : m_pending)
    {
        m_current.emplace(key, std::move(entry));
    }
    m_pending.clear();
    m_hotReloadPending = false;
}

void D3D12PsoFactory::AbortHotReload() noexcept
{
    m_pending.clear();
    m_hotReloadPending = false;
}

bool D3D12PsoFactory::IsHotReloadPending() const noexcept
{
    return m_hotReloadPending;
}

std::size_t D3D12PsoFactory::PendingPsoCount() const noexcept
{
    return m_pending.size();
}

std::size_t D3D12PsoFactory::CachedPsoCount() const noexcept
{
    return m_current.size();
}

std::size_t D3D12PsoFactory::RetiredPendingCount() const noexcept
{
    return m_deferred.PendingCount();
}

const PsoFactoryStats& D3D12PsoFactory::Stats() const noexcept
{
    return m_stats;
}

D3D12DeferredRelease& D3D12PsoFactory::DeferredRelease() noexcept
{
    return m_deferred;
}

void D3D12PsoFactory::LogPsoInventory() const
{
    for (const auto& [key, entry] : m_current)
    {
        MiniEngine::WriteLog(MiniEngine::LogLevel::Info,
                             std::format("d3d12 pso inventory: {} pass={} rtv={} dsv={}", entry.debugName,
                                         PassKindName(key.pass), static_cast<int>(entry.formats.renderTargetFormat),
                                         static_cast<int>(entry.formats.depthStencilFormat)));
    }
}
} // namespace MiniEngine::Rhi::D3D12

namespace MiniEngine::Rhi::D3D12
{
std::shared_ptr<AdaptedPso> D3D12PsoFactory::AcquireAdapted(std::string key,
                                                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    if (!m_device || key.empty() || !desc.pRootSignature || !desc.VS.pShaderBytecode || !desc.VS.BytecodeLength)
        throw std::invalid_argument("adapted PSO requires initialized factory and complete descriptor/key");
    for (auto it = m_adapted.begin(); it != m_adapted.end();)
    {
        if (it->second.expired())
            it = m_adapted.erase(it);
        else
            ++it;
    }
    if (const auto found = m_adapted.find(key); found != m_adapted.end())
    {
        if (auto existing = found->second.lock())
        {
            ++m_stats.cacheHits;
            return existing;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto candidate = std::make_shared<AdaptedPso>();
    try
    {
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&candidate->pipeline)),
                      "ID3D12Device::CreateGraphicsPipelineState(adapted)");
    }
    catch (...)
    {
        ++m_stats.failedCreations;
        throw;
    }
    ++m_stats.created;
    m_stats.creationMicroseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    m_adapted.emplace(std::move(key), candidate);
    return candidate;
}
} // namespace MiniEngine::Rhi::D3D12
