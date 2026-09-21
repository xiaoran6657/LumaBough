# M6-06：同一份 M5 native 机制由旧 renderer 与新 adapter 共用。
add_library(MiniEngineD3D12Runtime STATIC src/D3D12Common.cpp src/D3D12DescriptorAllocator.cpp src/D3D12DescriptorHeap.cpp src/D3D12Device.cpp src/D3D12Diagnostics.cpp src/D3D12Queue.cpp src/D3D12PsoFactory.cpp src/D3D12ResourceStateRegistry.cpp src/D3D12ResourceStateTracker.cpp src/D3D12RootSignature.cpp src/D3D12SwapChain.cpp src/D3D12TextureUpload.cpp src/D3D12UploadManager.cpp src/D3D12UploadRing.cpp src/D3D12UploadRingAllocator.cpp src/D3D12Capabilities.cpp)
target_include_directories(MiniEngineD3D12Runtime PUBLIC include PRIVATE src)
target_link_libraries(MiniEngineD3D12Runtime PUBLIC MiniEngineRhiPublic PRIVATE MiniEngine::Core d3d12 dxgi dxguid)
target_compile_definitions(MiniEngineD3D12Runtime PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(MiniEngineD3D12Runtime PUBLIC cxx_std_20)
set_target_properties(MiniEngineD3D12Runtime PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineD3D12Runtime)
include(MiniEnginePix)
miniengine_use_pix(MiniEngineD3D12Runtime)

# ============================================================================
# CMakeLists.txt — MiniEngineD3D12 具体渲染后端库
# 里程碑：M5（01 篇架构边界；02 篇环境、Device 与诊断基线）
# 职责：编译 D3D12 concrete 后端静态库，并与 D3D11 后端保持平行结构（同样位于
#   engine/rhi/ 下、同样只暴露 include/MiniEngine/Rhi/<API>/ 的公共头）。本阶段
#   落地：错误处理/对齐基元（01）、Device 创建顺序与诊断（02：DRED→Debug Layer→
#   GBV→factory→adapter→device→feature→InfoQueue）。后续 03—09 篇的 Queue、
#   FrameContext、Descriptor Heap、Upload Ring、State Tracker 与 PSO 会在此
#   逐个登记新增源文件。
# 依赖方向：上层模块（Core/Assets/World）不得反向依赖本库，只有 Sandbox 组合根
#   链接它。d3d12/dxgi/dxguid 为 PRIVATE：只有实现文件直接调用 ID3D12*/IDXGI*
#   接口；dxguid 提供接口 IID（IID_PPV_ARGS 展开的 __uuidof 需要 GUID 符号）。
#   Core 为 PRIVATE：诊断日志经 MiniEngine::WriteLog 输出（内部实现依赖）。
# 关联：docs/architecture/README.md（手抄清单第 1/2 条）
#       docs/architecture/DECISIONS.md
# ============================================================================

add_library(
    MiniEngineD3D12
    STATIC
        src/D3D12AssetCache.cpp
        src/D3D12Camera.cpp
        src/D3D12DepthBuffer.cpp
        src/D3D12Png.cpp
        src/D3D12FrameReadback.cpp
        src/D3D12Renderer.cpp
        src/D3D12RendererPasses.cpp
)

add_library(MiniEngine::D3D12 ALIAS MiniEngineD3D12)

# 公共头根目录：包含路径与 D3D11 后端同形（MiniEngine/Rhi/<API>/<File>.h），
# 使上层在 M6 之前无需为两个后端分别记忆不同的包含层次。
target_include_directories(
    MiniEngineD3D12
    PUBLIC
        include
)

target_compile_features(
    MiniEngineD3D12
    PUBLIC
        cxx_std_20
)

# Assets/World PUBLIC：09 篇起渲染器消费 World::RenderPacket、D3D12AssetCache
# 引用 AssetHandle/MeshAsset/TextureAsset，与 D3D11 后端的链接形态一致
# （依赖方向不变：World/Assets 仍不反向依赖任何 RHI）。D3D 库 PRIVATE。
target_link_libraries(
    MiniEngineD3D12
    PUBLIC
        MiniEngine::Assets
        MiniEngine::World
    PRIVATE
        MiniEngine::Core
        d3d12
        dxgi
        dxguid
)

# 私有定义与 D3D11 后端一致：Unicode 入口、最小化 Win32 头、取消 min/max 宏
# （min/max 宏会破坏 std::numeric_limits 等标准库调用）。
target_compile_definitions(
    MiniEngineD3D12
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(
    MiniEngineD3D12
    PROPERTIES
        CXX_EXTENSIONS OFF
)

miniengine_set_project_warnings(MiniEngineD3D12)

# M5-11：真实 PIX runtime 同时服务 CPU/GPU markers。
include(MiniEnginePix)
miniengine_use_pix(MiniEngineD3D12)

# M6-02：仅加入真实能力查询；旧 concrete 渲染路径不变。
target_link_libraries(MiniEngineD3D12 PRIVATE MiniEngineD3D12Runtime)
target_link_libraries(MiniEngineD3D12 PRIVATE MiniEngineRhiPublic)

# M6-06 生产 public RHI adapter，native include 不向上游传播。
add_library(MiniEngineD3D12RhiAdapter STATIC src/D3D12RhiBackend.cpp src/D3D12RhiDiagnostics.cpp src/D3D12RhiEnvironment.cpp src/D3D12GpuProfiler.cpp)
target_include_directories(MiniEngineD3D12RhiAdapter PRIVATE src include "../src")
# M7-02：GPU zone 通过 profiling 门面接入；Tracy 关闭时该库只提供返回 nullptr 的工厂。
target_link_libraries(MiniEngineD3D12RhiAdapter PUBLIC MiniEngineRhiPublic PRIVATE MiniEngineD3D12Runtime MiniEngine::Core MiniEngineProfiling)
target_compile_definitions(MiniEngineD3D12RhiAdapter PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(MiniEngineD3D12RhiAdapter PUBLIC cxx_std_20)
set_target_properties(MiniEngineD3D12RhiAdapter PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineD3D12RhiAdapter)
miniengine_use_pix(MiniEngineD3D12RhiAdapter)
