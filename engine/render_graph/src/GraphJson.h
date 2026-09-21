#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace MiniEngine::RenderGraph::Detail
{
class JsonText final
{
  public:
    explicit JsonText(std::size_t reserve = 4096)
    {
        text.reserve(reserve);
    }
    void Raw(std::string_view value)
    {
        text.append(value);
    }
    void Number(std::uint64_t value)
    {
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        text.append(buffer, result.ptr);
    }
    void Float(float value)
    {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value == 0.0F ? 0.0F : value);
        text.append(buffer, result.ptr);
    }
    void Index(std::uint32_t value)
    {
        if (value == 0xFFFFFFFFU)
            Raw("null");
        else
            Number(value);
    }
    void Bool(bool value)
    {
        Raw(value ? "true" : "false");
    }
    void String(std::string_view value)
    {
        constexpr char hex[] = "0123456789abcdef";
        Raw("\"");
        for (const unsigned char c : value)
        {
            switch (c)
            {
            case '"':
                Raw("\\\"");
                break;
            case '\\':
                Raw("\\\\");
                break;
            case '\n':
                Raw("\\n");
                break;
            case '\r':
                Raw("\\r");
                break;
            case '\t':
                Raw("\\t");
                break;
            default:
                if (c < 0x20)
                {
                    Raw("\\u00");
                    text.push_back(hex[c >> 4]);
                    text.push_back(hex[c & 15]);
                }
                else
                    text.push_back(static_cast<char>(c));
            }
        }
        Raw("\"");
    }
    std::string text;
};
inline std::string HexHash(std::uint64_t value)
{
    char digits[16];
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 16; ++i)
    {
        digits[15 - i] = hex[value & 15];
        value >>= 4;
    }
    return std::string(digits, 16);
}
inline std::uint64_t PlanHash(std::string_view bytes)
{
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char c : bytes)
    {
        value ^= c;
        value *= 1099511628211ULL;
    }
    return value;
}
} // namespace MiniEngine::RenderGraph::Detail
