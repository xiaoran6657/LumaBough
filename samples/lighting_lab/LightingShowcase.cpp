// 固定几何的原生 D3D12 离屏教学入口。GPU 完成后才发布图片/线性数据；result.json 最后写入。
// 不依赖测试程序、M10.5 服务、运行期 shader 编译或私有源码路径。
#include <MiniEngine/Assets/Sha256.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr std::uint32_t kShadowSize = 1024;
constexpr float kTanHalf = 1.6F;

void Check(HRESULT hr, std::string_view operation)
{
    if (FAILED(hr))
    {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(message.str());
    }
}

std::string Utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count == 0)
    {
        throw std::runtime_error("invalid Unicode text");
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), count, nullptr, nullptr) != 0 ? S_OK : E_FAIL,
        "WideCharToMultiByte");
    return output;
}

std::string Quote(std::string_view value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char ch : value)
    {
        if (ch == '\\' || ch == '"')
        {
            output << '\\' << static_cast<char>(ch);
        }
        else if (ch < 32)
        {
            output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch);
        }
        else
        {
            output << static_cast<char>(ch);
        }
    }
    output << '"';
    return output.str();
}

std::string UtcNow()
{
    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::ostringstream value;
    value << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-'
          << std::setw(2) << now.wDay << 'T' << std::setw(2) << now.wHour << ':' << std::setw(2)
          << now.wMinute << ':' << std::setw(2) << now.wSecond << '.' << std::setw(3)
          << now.wMilliseconds << 'Z';
    return value.str();
}

std::vector<std::byte> ReadBytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error("cannot read " + Utf8(path.wstring()));
    }
    const auto length = stream.tellg();
    if (length <= 0)
    {
        throw std::runtime_error("empty input " + Utf8(path.wstring()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream)
    {
        throw std::runtime_error("input truncated");
    }
    return bytes;
}

std::string Hash(const fs::path& path)
{
    return MiniEngine::Assets::ToHexDigest(MiniEngine::Assets::Sha256(ReadBytes(path)));
}

void WriteBytes(const fs::path& path, const void* bytes, std::size_t length)
{
    if (fs::exists(path))
    {
        throw std::runtime_error("refusing to overwrite output");
    }
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(length));
    output.close();
    if (!output)
    {
        throw std::runtime_error("output write failed");
    }
}

void WriteText(const fs::path& path, const std::string& value)
{
    WriteBytes(path, value.data(), value.size());
}

struct Float4
{
    float x = 0, y = 0, z = 0, w = 0;
};

Float4 Subtract(Float4 left, Float4 right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z, 0};
}

Float4 Normalize(Float4 value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    return {value.x / length, value.y / length, value.z / length, value.w};
}

Float4 Cross(Float4 a, Float4 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0};
}

struct Box
{
    Float4 minimum, maximum, albedo;
};

struct Constants
{
    std::array<std::uint32_t, 4> dimensions{};
    Float4 shadowParams, renderParams, cameraPosition, cameraForward, cameraRight, cameraUp, lightPosition;
    std::array<Box, 16> boxes{};
};
static_assert(sizeof(Constants) == 896);

struct Options
{
    std::string scene = "shadow-bench";
    std::string shadow = "pcss";
    std::string indirect = "direct";
    float lightSize = 0.8F;
    float bias = 0.008F;
    std::uint32_t samples = 32;
    std::uint32_t width = 960;
    std::uint32_t height = 600;
    fs::path output;
    bool debug = false;
};

float ParseFloat(const std::string& text, float low, float high)
{
    std::size_t consumed = 0;
    const float value = std::stof(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < low || value > high)
    {
        throw std::runtime_error("floating argument is outside the supported range");
    }
    return value;
}

std::uint32_t ParseInteger(const std::string& text, std::uint32_t low, std::uint32_t high)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    {
        throw std::runtime_error("integer argument expected");
    }
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value < low || value > high)
    {
        throw std::runtime_error("integer argument is outside the supported range");
    }
    return static_cast<std::uint32_t>(value);
}

Options ParseOptions(int argc, wchar_t** argv)
{
    Options options;
    std::set<std::string> seen;
    for (int index = 1; index < argc; ++index)
    {
        const std::string name = Utf8(argv[index]);
        if (!seen.insert(name).second)
        {
            throw std::runtime_error("duplicate argument " + name);
        }
        if (name == "--debug")
        {
            options.debug = true;
            continue;
        }
        if (index + 1 >= argc)
        {
            throw std::runtime_error("missing value for " + name);
        }
        const std::wstring wideValue = argv[++index];
        const std::string value = Utf8(wideValue);
        if (name == "--scene") options.scene = value;
        else if (name == "--shadow") options.shadow = value;
        else if (name == "--indirect") options.indirect = value;
        else if (name == "--light-size") options.lightSize = ParseFloat(value, 0.01F, 2.5F);
        else if (name == "--bias") options.bias = ParseFloat(value, 0.0F, 0.5F);
        else if (name == "--samples") options.samples = ParseInteger(value, 4, 128);
        else if (name == "--width") options.width = ParseInteger(value, 64, 3840);
        else if (name == "--height") options.height = ParseInteger(value, 64, 2160);
        else if (name == "--output") options.output = wideValue;
        else throw std::runtime_error("unknown argument " + name);
    }
    if ((options.scene != "shadow-bench" && options.scene != "cornell") ||
        (options.shadow != "hard" && options.shadow != "pcf" && options.shadow != "pcss") ||
        (options.indirect != "direct" && options.indirect != "bounce"))
    {
        throw std::runtime_error("unknown scene/shadow/indirect mode");
    }
    if (options.indirect == "bounce" && options.scene != "cornell")
    {
        throw std::runtime_error("one-bounce mode is supported only by cornell");
    }
    if (options.output.empty())
    {
        throw std::runtime_error("--output must name a new directory");
    }
    return options;
}

struct Scene
{
    Constants constants;
    Float4 target;
    float verticalFov = 45;
};

Scene MakeScene(const Options& options)
{
    Scene scene;
    auto& c = scene.constants;
    c.dimensions = {options.width, options.height, kShadowSize, 0};
    c.shadowParams = {options.lightSize, options.bias, static_cast<float>(options.samples),
        options.shadow == "hard" ? 0.0F : options.shadow == "pcf" ? 1.0F : 2.0F};
    c.renderParams = {options.indirect == "bounce" ? 1.0F : 0.0F, 1.0F, 20.0F, kTanHalf};
    const auto addBox = [&](Float4 minimum, Float4 maximum, Float4 albedo, int material)
    {
        minimum.w = static_cast<float>(material);
        c.boxes[c.dimensions[3]++] = {minimum, maximum, albedo};
    };
    if (options.scene == "shadow-bench")
    {
        c.cameraPosition = {7.0F, 5.6F, 8.0F, 0};
        scene.target = {0, 0.45F, 0, 0};
        c.lightPosition = {-1.8F, 6.0F, -2.5F, 110.0F};
        addBox({-4.5F, -0.2F, -4.5F}, {4.5F, 0, 4.5F}, {.66F, .68F, .72F}, 0);
        addBox({-1.75F, 0, -.85F}, {-.70F, 1.7F, .20F}, {.72F, .45F, .18F}, 1);
        addBox({.50F, 0, -.65F}, {1.55F, .7F, .4F}, {.23F, .50F, .65F}, 2);
        addBox({-.2F, 1.7F, 1.4F}, {.85F, 1.85F, 2.4F}, {.72F, .72F, .72F}, 3);
        addBox({.10F, 0, -2.1F}, {.20F, 1.3F, -1.0F}, {.62F, .36F, .29F}, 4);
    }
    else
    {
        c.cameraPosition = {0, 2.65F, 8.2F, 0};
        scene.target = {0, 1.5F, -.35F, 0};
        c.lightPosition = {-.45F, 4.8F, -.65F, 70.0F};
        scene.verticalFov = 42;
        addBox({-2.08F, -.12F, -2.08F}, {2.08F, 0, 2.08F}, {.74F, .74F, .74F}, 0);
        addBox({-2.08F, 0, -2.08F}, {-2, 3.4F, 2.08F}, {.64F, .07F, .05F}, 1);
        addBox({2, 0, -2.08F}, {2.08F, 3.4F, 2.08F}, {.07F, .48F, .11F}, 2);
        addBox({-2, 0, -2.08F}, {2, 3.4F, -2}, {.74F, .74F, .74F}, 3);
        addBox({-1.15F, 0, -.2F}, {-.2F, 1.15F, .9F}, {.76F, .76F, .76F}, 4);
        addBox({.35F, 0, -1.2F}, {1.25F, 2, -.25F}, {.76F, .76F, .76F}, 5);
    }
    float highest = 0;
    for (std::uint32_t index = 0; index < c.dimensions[3]; ++index)
    {
        highest = std::max(highest, c.boxes[index].maximum.y);
    }
    c.renderParams.y = std::max(0.01F, c.lightPosition.y - highest);
    c.cameraForward = Normalize(Subtract(scene.target, c.cameraPosition));
    c.cameraForward.w = static_cast<float>(options.width) / static_cast<float>(options.height);
    c.cameraRight = Normalize(Cross(c.cameraForward, {0, 1, 0, 0}));
    c.cameraUp = Normalize(Cross(c.cameraRight, c.cameraForward));
    c.cameraPosition.w = std::tan(scene.verticalFov * 3.14159265359F / 360.0F);
    return scene;
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value != nullptr) CloseHandle(value); }
};

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, std::uint64_t bytes, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_STATES initial, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
        initial, nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource");
    return resource;
}

struct GpuResult
{
    std::vector<Float4> color, diagnostics;
    std::vector<float> shadowDepth;
    std::string adapter, debugMessages;
    std::uint32_t vendor = 0, device = 0;
    std::uint32_t warnings = 0, errors = 0;
    std::array<std::uint64_t, 3> ticks{};
    std::uint64_t timestampFrequency = 0;
};

GpuResult Render(const Options& options, const Constants& constants, const std::vector<std::byte>& shader)
{
    GpuResult result;
    ComPtr<ID3D12Debug> debug;
    if (options.debug)
    {
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface (--debug requires Graphics Tools)");
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(options.debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT index = 0; ; ++index)
    {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT enumeration = factory->EnumAdapterByGpuPreference(index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (enumeration == DXGI_ERROR_NOT_FOUND) break;
        Check(enumeration, "EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 description{};
        Check(candidate->GetDesc1(&description), "GetDesc1");
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            adapter = candidate;
            result.adapter = Utf8(description.Description);
            result.vendor = description.VendorId;
            result.device = description.DeviceId;
            break;
        }
    }
    if (!device) throw std::runtime_error("no hardware D3D12 adapter; software fallback is disabled");
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
    Check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)), "Shader Model 6 support");
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_0) throw std::runtime_error("Shader Model 6 required");
    ComPtr<ID3D12InfoQueue> info;
    if (options.debug) Check(device.As(&info), "ID3D12InfoQueue");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    Check(queue->GetTimestampFrequency(&result.timestampFrequency), "GetTimestampFrequency");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");

    std::array<D3D12_ROOT_PARAMETER, 5> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = 1;
    for (UINT index = 0; index < 3; ++index)
    {
        parameters[index + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[index + 2].Descriptor.ShaderRegister = index;
    }
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(parameters.size());
    rootDesc.pParameters = parameters.data();
    ComPtr<ID3DBlob> rootBytes, rootErrors;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &rootErrors), "SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "CreateComputePipelineState");

    const std::uint64_t pixelCount = static_cast<std::uint64_t>(options.width) * options.height;
    const std::uint64_t colorBytes = pixelCount * sizeof(Float4);
    const std::uint64_t shadowBytes = static_cast<std::uint64_t>(kShadowSize) * kShadowSize * sizeof(float);
    const std::uint64_t tickOffset = colorBytes * 2 + shadowBytes;
    auto constantBuffer = Buffer(device.Get(), 1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE emptyRange{0, 0};
    Check(constantBuffer->Map(0, &emptyRange, &mapped), "Map constants");
    std::memcpy(mapped, &constants, sizeof(constants));
    constantBuffer->Unmap(0, nullptr);
    auto shadow = Buffer(device.Get(), shadowBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto color = Buffer(device.Get(), colorBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto diagnostics = Buffer(device.Get(), colorBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = Buffer(device.Get(), tickOffset + sizeof(result.ticks), D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = 3;
    ComPtr<ID3D12QueryHeap> queries;
    Check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "CreateQueryHeap");
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, shadow->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, color->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, diagnostics->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(1, 0, 0);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->Dispatch(kShadowSize / 8, kShadowSize / 8, 1);
    D3D12_RESOURCE_BARRIER shadowBarrier{};
    shadowBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    shadowBarrier.UAV.pResource = shadow.Get();
    list->ResourceBarrier(1, &shadowBarrier);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->SetComputeRoot32BitConstant(1, 1, 0);
    list->Dispatch((options.width + 7) / 8, (options.height + 7) / 8, 1);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    std::array<ID3D12Resource*, 3> resources{color.Get(), diagnostics.Get(), shadow.Get()};
    std::array<D3D12_RESOURCE_BARRIER, 3> copies{};
    for (std::size_t index = 0; index < resources.size(); ++index)
    {
        copies[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copies[index].Transition = {resources[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    list->ResourceBarrier(static_cast<UINT>(copies.size()), copies.data());
    list->CopyBufferRegion(readback.Get(), 0, color.Get(), 0, colorBytes);
    list->CopyBufferRegion(readback.Get(), colorBytes, diagnostics.Get(), 0, colorBytes);
    list->CopyBufferRegion(readback.Get(), colorBytes * 2, shadow.Get(), 0, shadowBytes);
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback.Get(), tickOffset);
    Check(list->Close(), "Close command list");
    ID3D12CommandList* submission[] = {list.Get()};
    queue->ExecuteCommandLists(1, submission);
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    Handle complete{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (complete.value == nullptr) throw std::runtime_error("CreateEvent failed");
    Check(queue->Signal(fence.Get(), 1), "Signal");
    Check(fence->SetEventOnCompletion(1, complete.value), "SetEventOnCompletion");
    if (WaitForSingleObject(complete.value, 60000) != WAIT_OBJECT_0)
    {
        throw std::runtime_error("GPU fence did not complete within 60 seconds");
    }
    Check(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(tickOffset + sizeof(result.ticks))};
    Check(readback->Map(0, &readRange, &mapped), "Map readback");
    result.color.resize(static_cast<std::size_t>(pixelCount));
    result.diagnostics.resize(static_cast<std::size_t>(pixelCount));
    result.shadowDepth.resize(static_cast<std::size_t>(kShadowSize) * kShadowSize);
    const auto* bytes = static_cast<const std::byte*>(mapped);
    std::memcpy(result.color.data(), bytes, static_cast<std::size_t>(colorBytes));
    std::memcpy(result.diagnostics.data(), bytes + colorBytes, static_cast<std::size_t>(colorBytes));
    std::memcpy(result.shadowDepth.data(), bytes + colorBytes * 2, static_cast<std::size_t>(shadowBytes));
    std::memcpy(result.ticks.data(), bytes + tickOffset, sizeof(result.ticks));
    readback->Unmap(0, &emptyRange);
    if (result.timestampFrequency == 0 || result.ticks[0] == 0 || result.ticks[1] <= result.ticks[0] ||
        result.ticks[2] <= result.ticks[1])
    {
        throw std::runtime_error("invalid GPU timestamp sequence");
    }
    if (info)
    {
        std::ostringstream messages;
        const UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        messages << "D3D12 Debug Layer enabled; stored messages=" << count << '\n';
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size = 0;
            Check(info->GetMessage(index, nullptr, &size), "GetMessage size");
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(index, message, &size), "GetMessage");
            messages << "id=" << message->ID << " severity=" << message->Severity << ' '
                     << message->pDescription << '\n';
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++result.errors;
            if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++result.warnings;
        }
        result.debugMessages = messages.str();
    }
    else result.debugMessages = "D3D12 Debug Layer disabled by invocation; no validation claim.\n";
    return result;
}

std::uint8_t UnitByte(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::uint8_t ColorByte(float value)
{
    const float positive = std::max(0.0F, value);
    const float mapped = positive / (1.0F + positive);
    const float srgb = mapped <= 0.0031308F ? mapped * 12.92F : 1.055F * std::pow(mapped, 1.0F / 2.4F) - .055F;
    return UnitByte(srgb);
}

void WritePng(const fs::path& path, std::uint32_t width, std::uint32_t height,
    const std::vector<std::uint8_t>& rgba)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "WIC CreateStream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "WIC output");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "WIC PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "WIC Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&frame, &properties), "WIC CreateNewFrame");
    Check(frame->Initialize(properties.Get()), "WIC frame Initialize");
    Check(frame->SetSize(width, height), "WIC SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    Check(frame->SetPixelFormat(&format), "WIC SetPixelFormat");
    std::vector<std::uint8_t> converted;
    const std::uint8_t* source = rgba.data();
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA) == FALSE)
    {
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) == FALSE)
            throw std::runtime_error("WIC changed to unsupported pixel format");
        converted = rgba;
        for (std::size_t index = 0; index < converted.size(); index += 4) std::swap(converted[index], converted[index + 2]);
        source = converted.data();
    }
    Check(frame->WritePixels(height, width * 4, static_cast<UINT>(rgba.size()),
        const_cast<BYTE*>(source)), "WIC WritePixels");
    Check(frame->Commit(), "WIC frame Commit");
    Check(encoder->Commit(), "WIC encoder Commit");
    Check(stream->Commit(STGC_DEFAULT), "WIC stream Commit");
}

std::string VectorJson(Float4 value)
{
    std::ostringstream output;
    output << std::setprecision(9) << '[' << value.x << ',' << value.y << ',' << value.z << ']';
    return output.str();
}

std::string Artifact(const fs::path& root, std::string_view relative, bool absolutePath = false)
{
    const fs::path path = root / relative;
    std::ostringstream output;
    output << "{\"path\":" << (absolutePath ? Quote(Utf8(fs::absolute(path).wstring())) : Quote(relative))
           << ",\"bytes\":" << fs::file_size(path)
           << ",\"sha256\":" << Quote(Hash(path)) << '}';
    return output.str();
}

void Publish(const Options& options, const Scene& scene, const GpuResult& gpu,
    const fs::path& executable, const std::string& started, const std::string& runId,
    double cpuTotalMs)
{
    WriteText(options.output / "debug-layer.txt", gpu.debugMessages);
    if (gpu.errors != 0 || gpu.warnings != 0)
        throw std::runtime_error("D3D12 validation emitted warning/error; inspect debug-layer.txt");
    const std::size_t count = gpu.color.size();
    std::vector<std::uint8_t> color(count * 4), visibility(count * 4), diagnostic(count * 4);
    std::vector<float> visibilityRaw(count);
    std::size_t geometryPixels = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto c = gpu.color[index];
        const auto d = gpu.diagnostics[index];
        const std::array<float, 8> all{c.x,c.y,c.z,c.w,d.x,d.y,d.z,d.w};
        for (float value : all)
            if (!std::isfinite(value)) throw std::runtime_error("non-finite GPU output");
        if (d.x < 0 || d.x > 1 || c.x < 0 || c.y < 0 || c.z < 0 || d.y < 0 || d.z < 0)
            throw std::runtime_error("GPU output outside contract");
        if (d.w >= 0) ++geometryPixels;
        color[index * 4] = ColorByte(c.x);
        color[index * 4 + 1] = ColorByte(c.y);
        color[index * 4 + 2] = ColorByte(c.z);
        color[index * 4 + 3] = 255;
        for (std::size_t channel = 0; channel < 3; ++channel) visibility[index * 4 + channel] = UnitByte(d.x);
        visibility[index * 4 + 3] = 255;
        diagnostic[index * 4] = UnitByte(d.y / 8.0F);
        diagnostic[index * 4 + 1] = UnitByte(d.z * 50.0F);
        diagnostic[index * 4 + 2] = UnitByte(1.0F - d.x);
        diagnostic[index * 4 + 3] = 255;
        visibilityRaw[index] = d.x;
    }
    if (geometryPixels < count / 10) throw std::runtime_error("insufficient scene coverage");
    for (float depth : gpu.shadowDepth)
        if (!std::isfinite(depth) || depth < 0 || depth > 20) throw std::runtime_error("invalid shadow depth");
    WritePng(options.output / "color.png", options.width, options.height, color);
    WritePng(options.output / "visibility.png", options.width, options.height, visibility);
    WritePng(options.output / "diagnostic.png", options.width, options.height, diagnostic);
    WriteBytes(options.output / "linear-hdr.rgba32f", gpu.color.data(), gpu.color.size() * sizeof(Float4));
    WriteBytes(options.output / "diagnostics.rgba32f", gpu.diagnostics.data(), gpu.diagnostics.size() * sizeof(Float4));
    WriteBytes(options.output / "visibility.r32f", visibilityRaw.data(), visibilityRaw.size() * sizeof(float));
    WriteBytes(options.output / "shadow-depth.r32f", gpu.shadowDepth.data(), gpu.shadowDepth.size() * sizeof(float));
    const auto& c = scene.constants;
    std::ostringstream json;
    json << std::setprecision(10) << "{\n  \"schemaVersion\":1,\"status\":\"complete\",\"backend\":\"D3D12-native\",\n"
        << "  \"runId\":" << Quote(runId) << ",\"startedUtc\":" << Quote(started)
        << ",\"finishedUtc\":" << Quote(UtcNow()) << ",\"buildConfig\":" << Quote(ME_BUILD_CONFIG) << ",\n"
        << "  \"params\":{\"scene\":" << Quote(options.scene) << ",\"shadow\":" << Quote(options.shadow)
        << ",\"lightSize\":" << options.lightSize << ",\"bias\":" << options.bias << ",\"samples\":" << options.samples
        << ",\"indirect\":" << Quote(options.indirect) << ",\"width\":" << options.width << ",\"height\":" << options.height << "},\n"
        << "  \"adapter\":{\"description\":" << Quote(gpu.adapter) << ",\"software\":false,\"vendorId\":" << gpu.vendor
        << ",\"deviceId\":" << gpu.device << "},\n"
        << "  \"debugLayer\":{\"enabled\":" << (options.debug ? "true" : "false") << ",\"errorCount\":" << gpu.errors
        << ",\"warningCount\":" << gpu.warnings << "},\n"
        << "  \"identity\":{\"executable\":{\"path\":" << Quote(Utf8(executable.wstring())) << ",\"sha256\":" << Quote(Hash(executable))
        << "},\"shaderSource\":" << Artifact(executable.parent_path(), "shaders/LightingShowcase.hlsl", true)
        << ",\"shaderBinary\":" << Artifact(executable.parent_path(), "shaders/LightingShowcase.CSMain.cs.dxil", true)
        << ",\"shaderInclude\":" << Artifact(executable.parent_path(), "shaders/PbrCommon.hlsli", true) << "},\n"
        << "  \"camera\":{\"position\":" << VectorJson(c.cameraPosition) << ",\"target\":" << VectorJson(scene.target)
        << ",\"up\":[0,1,0],\"verticalFovDegrees\":" << scene.verticalFov << "},\n"
        << "  \"light\":{\"position\":" << VectorJson(c.lightPosition) << ",\"intensity\":" << c.lightPosition.w
        << ",\"shape\":\"square-visibility-point-radiance-proxy\",\"sizeMeaning\":\"full side length in world units\"},\n"
        << "  \"shadowMap\":{\"size\":" << kShadowSize << ",\"tanHalfFov\":" << kTanHalf
        << ",\"farDepth\":20,\"minimumCasterDepth\":" << c.renderParams.y
        << ",\"depthConvention\":\"light.y-world.y, linear world units\",\"uvConvention\":\"0.5+(world.xz-light.xz)/(2*depth*tanHalfFov)\"},\n"
        << "  \"geometry\":{\"kind\":\"procedural-axis-aligned-boxes\",\"openTop\":true,\"boxes\":[";
    for (std::uint32_t index = 0; index < c.dimensions[3]; ++index)
    {
        if (index != 0) json << ',';
        json << "{\"min\":" << VectorJson(c.boxes[index].minimum) << ",\"max\":" << VectorJson(c.boxes[index].maximum)
            << ",\"albedo\":" << VectorJson(c.boxes[index].albedo) << ",\"materialId\":" << static_cast<int>(c.boxes[index].minimum.w) << '}';
    }
    const double shadowMs = static_cast<double>(gpu.ticks[1] - gpu.ticks[0]) * 1000.0 / static_cast<double>(gpu.timestampFrequency);
    const double shadeMs = static_cast<double>(gpu.ticks[2] - gpu.ticks[1]) * 1000.0 / static_cast<double>(gpu.timestampFrequency);
    json << "]},\n  \"timing\":{\"timestampFrequency\":" << gpu.timestampFrequency
        << ",\"gpuTicks\":[" << gpu.ticks[0] << ',' << gpu.ticks[1] << ',' << gpu.ticks[2]
        << "],\"shadowMapGpuMs\":" << shadowMs << ",\"shadeGpuMs\":" << shadeMs << ",\"totalGpuMs\":" << shadowMs + shadeMs
        << ",\"cpuBeforePublishMs\":" << cpuTotalMs
        << ",\"scope\":\"single cold dispatch: analytic shadow-depth generation + camera shading; excludes initialization, readback and PNG; not a benchmark\"},\n"
        << "  \"outputs\":{\"color\":" << Artifact(options.output, "color.png")
        << ",\"visibility\":" << Artifact(options.output, "visibility.png")
        << ",\"diagnostic\":" << Artifact(options.output, "diagnostic.png")
        << ",\"linearHdr\":" << Artifact(options.output, "linear-hdr.rgba32f")
        << ",\"diagnostics\":" << Artifact(options.output, "diagnostics.rgba32f")
        << ",\"visibilityRaw\":" << Artifact(options.output, "visibility.r32f")
        << ",\"shadowDepth\":" << Artifact(options.output, "shadow-depth.r32f")
        << ",\"debugLog\":" << Artifact(options.output, "debug-layer.txt") << "},\n"
        << "  \"formats\":{\"binary\":\"little-endian IEEE754 float32, tightly packed row-major, top-left origin\","
        << "\"linearHdr\":\"RGBA32F linear radiance; alpha=1\",\"diagnostics\":\"RGBA32F: visibility,meanBlockerDepth,filterUvRadius,materialId (-1 background)\","
        << "\"color\":\"exposure 1; max(x,0)/(1+max(x,0)); IEC sRGB; round to 8bit\","
        << "\"diagnostic\":\"R=blocker/8,G=filterUvRadius*50,B=1-visibility; clamp and round 8bit\"},\n"
        << "  \"limitations\":[\"GPU analytic AABB shadow-map generation, not generic mesh rasterization\","
        << "\"PCSS is a planar blocker approximation with finite square samples\","
        << "\"direct radiance uses center point proxy; light size controls shadow visibility only\","
        << "\"Cornell-style open-top scene, not calibrated Cornell Box data\","
        << "\"receiver-plane depth correction is applied at each sampled shadow texel before explicit bias\","
        << "\"one bounce: 256 fixed cosine directions, analytic hard q-to-light visibility; no higher bounces\"]\n}\n";
    WriteText(options.output / "result.json.tmp", json.str());
    fs::rename(options.output / "result.json.tmp", options.output / "result.json");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help")
    {
        std::cout << "LightingShowcase --scene shadow-bench|cornell --shadow hard|pcf|pcss\n"
            "  --light-size [0.01,2.5] --bias [0,0.5] --samples [4,128]\n"
            "  --indirect direct|bounce --width [64,3840] --height [64,2160]\n"
            "  --output NEW-DIRECTORY [--debug]\n";
        return 0;
    }
    fs::path claimedOutput;
    try
    {
        const auto cpuStart = std::chrono::steady_clock::now();
        const std::string started = UtcNow();
        const Options options = ParseOptions(argc, argv);
        if (fs::exists(options.output)) throw std::runtime_error("output directory already exists");
        if (!options.output.parent_path().empty()) fs::create_directories(options.output.parent_path());
        if (!fs::create_directory(options.output)) throw std::runtime_error("cannot claim fresh output directory");
        claimedOutput = options.output;
        std::array<wchar_t, 32768> module{};
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0 || length >= module.size()) throw std::runtime_error("cannot locate executable");
        const fs::path executable(std::wstring(module.data(), length));
        const fs::path shaderPath = executable.parent_path() / "shaders/LightingShowcase.CSMain.cs.dxil";
        const auto shader = ReadBytes(shaderPath);
        const Scene scene = MakeScene(options);
        const GpuResult gpu = Render(options, scene.constants, shader);
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        const auto cpuEnd = std::chrono::steady_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
        const std::string runId = started + "-pid" + std::to_string(GetCurrentProcessId()) + "-qpc" +
            std::to_string(cpuStart.time_since_epoch().count());
        Publish(options, scene, gpu, executable, started, runId, cpuMs);
        CoUninitialize();
        std::cout << "complete: " << Utf8((options.output / "result.json").wstring()) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "LightingShowcase: " << error.what() << '\n';
        if (!claimedOutput.empty())
        {
            try { WriteText(claimedOutput / "failure.json", "{\"status\":\"failed\",\"error\":" + Quote(error.what()) + "}\n"); }
            catch (...) { }
        }
        return 1;
    }
}
