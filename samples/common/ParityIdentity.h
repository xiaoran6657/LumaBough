// ============================================================================
// ParityIdentity.h — D3D11/D3D12 语义身份与相机/光照身份序列化辅助
// 只依赖 CPU 文件读取和引擎 SHA-256；缺失 shader 直接失败，不生成占位哈希。
// ============================================================================
#pragma once
#include <MiniEngine/Assets/Sha256.h>
#include <MiniEngine/World/RenderPacket.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace MiniEngine::Samples
{
namespace Detail
{
inline std::string ReadText(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error{"shader missing: " + p.string()};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
inline std::string Strip(std::string_view s)
{
    std::string o;
    bool line = false, block = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i], n = i + 1 < s.size() ? s[i + 1] : '\0';
        if (!line && !block && c == '/' && n == '/')
        {
            line = true;
            o += ' ';
            ++i;
            continue;
        }
        if (!line && !block && c == '/' && n == '*')
        {
            block = true;
            o += ' ';
            ++i;
            continue;
        }
        if (line)
        {
            if (c == '\n')
            {
                line = false;
                o += '\n';
            }
            continue;
        }
        if (block)
        {
            if (c == '*' && n == '/')
            {
                block = false;
                o += ' ';
                ++i;
            }
            else if (c == '\n')
                o += '\n';
            continue;
        }
        o += c;
    }
    return o;
}
inline std::string Tokens(std::string_view s, const std::map<std::string, std::string>& m)
{
    std::string filtered;
    std::istringstream lines(Strip(s));
    std::vector<std::string> sourceLines;
    std::string line;
    while (std::getline(lines, line))
        sourceLines.push_back(line);
    std::size_t first = 0;
    while (first < sourceLines.size() && sourceLines[first].find_first_not_of(" \t\r") == std::string::npos)
        ++first;
    std::size_t last = sourceLines.size();
    while (last > first && sourceLines[last - 1].find_first_not_of(" \t\r") == std::string::npos)
        --last;
    std::string guard;
    if (first < last)
    {
        std::istringstream w(sourceLines[first]);
        std::string a, b;
        if (w >> a >> b && a == "#ifndef" && b.rfind("MINIENGINE_", 0) == 0)
            guard = b;
    }
    for (std::size_t index = 0; index < sourceLines.size(); ++index)
    {
        line = sourceLines[index];
        std::istringstream w(line);
        std::string a, b;
        if (w >> a >> b && a == "#include")
            continue;
        if (index == first && !guard.empty())
            continue;
        if (index == first + 1U && !guard.empty() && a == "#define" && b == guard)
            continue;
        if (index + 1U == last && !guard.empty() && a == "#endif")
            continue;
        filtered += line + '\n';
    }
    std::string o, t;
    auto flush = [&]
    {
        if (!t.empty())
        {
            if (!o.empty())
                o += ' ';
            auto i = m.find(t);
            o += i == m.end() ? t : i->second;
            t.clear();
        }
    };
    for (unsigned char c : filtered)
    {
        if (std::isalnum(c) || c == '_')
        {
            t += char(c);
            continue;
        }
        flush();
        if (!std::isspace(c))
        {
            if (!o.empty())
                o += ' ';
            o += char(c);
        }
    }
    flush();
    return o;
}
inline std::vector<std::string> Includes(std::string_view s)
{
    std::vector<std::string> r;
    std::istringstream ls(Strip(s));
    std::string l;
    while (std::getline(ls, l))
    {
        auto p = l.find("#include");
        if (p == std::string::npos)
            continue;
        auto b = l.find_first_of("\"<", p + 8);
        if (b == std::string::npos)
            continue;
        char q = l[b] == '"' ? '"' : '>';
        auto e = l.find(q, b + 1);
        if (e == std::string::npos)
            throw std::runtime_error{"malformed shader include"};
        r.push_back(l.substr(b + 1, e - b - 1));
    }
    return r;
}
} // namespace Detail
inline std::string ShaderSemanticHash(const std::filesystem::path& root, const std::string& backend)
{
    auto d = root / backend;
    if (!std::filesystem::is_directory(d))
        d = root / "shaders" / backend;
    if (!std::filesystem::is_directory(d))
        throw std::runtime_error{"shader backend missing: " + d.string()};
    std::map<std::string, std::string> mm;
    auto c = d / "BindingContract.hlsli";
    if (std::filesystem::exists(c))
    {
        std::istringstream ls(Detail::Strip(Detail::ReadText(c)));
        std::string l, a, b, v;
        while (std::getline(ls, l))
        {
            std::istringstream w(l);
            if (w >> a >> b >> v && a == "#define" && b.rfind("ME_", 0) == 0)
                mm[b] = v;
        }
    }
    std::vector<std::string> q{"PbrForward.hlsl",
                               "PbrCommon.hlsli",
                               "ShadowDepth.hlsl",
                               "ShadowSampling.hlsli",
                               "Skybox.hlsl",
                               "ToneMap.hlsl",
                               "EquirectToCube.hlsl",
                               "IrradianceConvolution.hlsl",
                               "PrefilterEnvironment.hlsl",
                               "IntegrateBrdf.hlsl",
                               "FullscreenTriangle.hlsli"};
    if (std::filesystem::exists(d / "ColorSpace.hlsli"))
        q.push_back("ColorSpace.hlsli");
    std::map<std::string, std::string> fs;
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (fs.contains(q[i]))
            continue;
        auto s = Detail::ReadText(d / q[i]);
        fs[q[i]] = Detail::Tokens(s, mm);
        for (auto& x : Detail::Includes(s))
            if (x != "BindingContract.hlsli")
                q.push_back(x);
    }
    MiniEngine::Assets::Sha256Builder b;
    for (auto& [n, t] : fs)
    {
        auto x = n + "\n" + t + "\n";
        if (!b.Append(std::as_bytes(std::span(x.data(), x.size()))))
            throw std::runtime_error{"shader hash append failed"};
    }
    MiniEngine::Assets::Sha256Digest h{};
    if (!b.Finish(h))
        throw std::runtime_error{"shader hash failed"};
    return MiniEngine::Assets::ToHexDigest(h);
}
inline std::string CameraIdentity(const MiniEngine::World::RenderPacket& p)
{
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(std::numeric_limits<double>::max_digits10);
    o << "view=";
    for (float v : p.view.values)
        o << double(v) << ',';
    o << ";projection=";
    for (float v : p.projection.values)
        o << double(v) << ',';
    o << ";position=";
    o << double(p.cameraWorldPosition.x) << ',' << double(p.cameraWorldPosition.y) << ','
      << double(p.cameraWorldPosition.z) << ',';
    return o.str();
}
inline std::string LightIdentity(const MiniEngine::World::RenderPacket& p)
{
    auto& l = p.directionalLight;
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(std::numeric_limits<double>::max_digits10);
    o << "direction=";
    for (float v : l.directionToLight)
        o << double(v) << ',';
    o << ";color=";
    for (float v : l.colorLinear)
        o << double(v) << ',';
    o << ";illuminance=" << double(l.illuminanceScale) << ";shadow=" << double(l.shadowLeft) << ','
      << double(l.shadowRight) << ',' << double(l.shadowBottom) << ',' << double(l.shadowTop) << ','
      << double(l.shadowNear) << ',' << double(l.shadowFar);
    return o.str();
}
} // namespace MiniEngine::Samples
