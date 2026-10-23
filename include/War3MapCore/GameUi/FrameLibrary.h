#pragma once

#include "War3MapCore/GameUi/FrameDefinition.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 界面定义库：按 toc 读入全部 fdf（含 IncludeFile），登记顶层模板与 StringList 文字。
// 只登记顶层定义作为模板，与游戏 INHERITS 只能继承顶层定义一致。
namespace War3Map::GameUi {

// 按归档内路径读取文件；由调用方决定来源（游戏 MPQ、地图、会话覆盖）与优先级。
using FrameFileReader = std::function<Result<std::vector<std::byte>>(const std::string& path)>;

struct FrameTemplateRef {
    const FdfDefinition* definition = nullptr;
    std::size_t fileIndex = 0;
};

struct FrameSourceFile {
    FdfFile file;
    std::string origin;  // 调用方给的来源标签，例如 "game"、"map"
};

class FrameLibrary {
public:
    FrameLibrary() = default;
    FrameLibrary(FrameLibrary&&) noexcept = default;
    FrameLibrary& operator=(FrameLibrary&&) noexcept = default;
    FrameLibrary(const FrameLibrary&) = delete;
    FrameLibrary& operator=(const FrameLibrary&) = delete;

    // 读取 toc 列出的每个 fdf。单个文件缺失或解析出错只记诊断，不中断其余文件。
    void LoadToc(const FrameFileReader& reader, const std::string& tocPath, const std::string& origin);
    // 读取单个 fdf 及其 IncludeFile；同一路径（不区分大小写）只读一次。
    void LoadFile(const FrameFileReader& reader, const std::string& fdfPath, const std::string& origin);
    // 直接加入已解析的文件（测试与地图内嵌文本使用）。
    void AddParsed(FdfFile file, const std::string& origin);

    // 顶层模板，名字不区分大小写；重名时后加载者生效，并记一条 Info 诊断。
    [[nodiscard]] std::optional<FrameTemplateRef> FindTemplate(std::string_view name) const;
    // StringList 文字；同键后加载者生效。
    [[nodiscard]] std::optional<std::string> FindString(std::string_view key) const;

    [[nodiscard]] const std::vector<std::unique_ptr<FrameSourceFile>>& Files() const noexcept { return files_; }
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::size_t TemplateCount() const noexcept { return templates_.size(); }
    [[nodiscard]] std::size_t StringCount() const noexcept { return strings_.size(); }

private:
    void LoadFileAt(const FrameFileReader& reader, const std::string& fdfPath, const std::string& origin, int depth);
    void Register(std::size_t fileIndex);

    std::vector<std::unique_ptr<FrameSourceFile>> files_;
    std::unordered_map<std::string, FrameTemplateRef> templates_;
    std::unordered_map<std::string, std::string> strings_;
    std::unordered_map<std::string, bool> loadedPaths_;
    std::vector<Diagnostic> diagnostics_;
};

// 游戏读取 fdf/toc 时的文本：去 BOM；字节按 UTF-8 原样保留，不做编码转换。
[[nodiscard]] std::string BytesToText(const std::vector<std::byte>& bytes);

} // namespace War3Map::GameUi
