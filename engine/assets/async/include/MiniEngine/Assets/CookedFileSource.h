// ============================================================================
// CookedFileSource.h — I/O 阶段的字节来源抽象（M7-06）
// 里程碑：M7-06（异步资产加载流水线）
// 职责：把"阻塞读取一个 cooked 产物"收敛到一个接口：
//         * 生产实现 DirectoryCookedFileSource：cooked 输出根 + 路径沙箱 + 尺寸上限；
//         * 测试实现（tests/assets）：脚本化字节、故障注入与可控延迟。
//       I/O 线程是唯一调用者；实现必须是阻塞的（第一版基线：一个专用 I/O 线程），
//       禁止在 compute worker 或 render thread 上读取文件。
// 路径契约（沿用 M3 Registry 的沙箱规则）：相对 outputRoot、不含 '\' 与 ':'、
//       不含空段与 "."/".." 段、不允许绝对路径。运行时禁止读取 glTF source。
// 关联：docs/architecture/README.md「第 2 步：专用 I/O 线程」
//       engine/assets/src/AssetRegistry.cpp（IsValidArtifactRelativePath 同规则）
// ============================================================================

#pragma once

#include <MiniEngine/Assets/AssetLoadRequest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace MiniEngine::Assets
{
// 一次读取的结果：字节或错误（两者互斥）。
struct CookedFileRead final
{
    std::vector<std::byte> bytes;
    AssetLoadErrorCode error = AssetLoadErrorCode::None;
    std::string errorText;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return error == AssetLoadErrorCode::None;
    }
};

// 阻塞字节来源。实现只在 I/O 线程被调用（见 AsyncAssetLoader 的线程契约）。
class CookedFileSource
{
  public:
    CookedFileSource() = default;
    virtual ~CookedFileSource() = default;

    CookedFileSource(const CookedFileSource&) = delete;
    CookedFileSource& operator=(const CookedFileSource&) = delete;

    // 读取一个 cooked 产物（阻塞）。
    //
    // 参数：
    //   cookedRelativePath —— registry 解析出的相对路径（仍必须由实现复核沙箱）
    //   maxBytes           —— 单产物字节上限；超过即 AssetTooLarge，且**不分配**缓冲
    // 返回：成功为字节；失败为分类错误 + 可读文本（不含绝对路径以外的敏感信息）。
    [[nodiscard]] virtual CookedFileRead Read(const std::string& cookedRelativePath, std::size_t maxBytes) = 0;
};

// 生产实现：目录根 + 路径沙箱 + 尺寸上限（先探测文件尺寸再分配）。
class DirectoryCookedFileSource final : public CookedFileSource
{
  public:
    explicit DirectoryCookedFileSource(std::filesystem::path outputRoot);

    [[nodiscard]] CookedFileRead Read(const std::string& cookedRelativePath, std::size_t maxBytes) override;

    // 供诊断与测试断言（不参与控制流）。
    [[nodiscard]] const std::filesystem::path& Root() const noexcept
    {
        return m_root;
    }

  private:
    std::filesystem::path m_root;
};
} // namespace MiniEngine::Assets
