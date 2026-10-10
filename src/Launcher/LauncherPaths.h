#pragma once
// 根目录引导程序的纯文本规则；不依赖 Windows API，便于单元测试。
#include <string_view>

namespace Audastra::Launcher {

// 返回命令行中程序名之后的原样参数文本。程序名按 Windows 对 argv[0] 的规则切分：
// 以引号开头时到下一个引号为止（引号内不处理转义），否则到第一个空白为止。
// 参数部分不重新解析、不重新加引号，原样交给编辑器，避免改变路径里的空格和引号。
[[nodiscard]] inline std::wstring_view ArgumentsAfterProgram(std::wstring_view line) noexcept {
    std::size_t at = 0;
    if (!line.empty() && line.front() == L'"') {
        at = line.find(L'"', 1);
        at = at == std::wstring_view::npos ? line.size() : at + 1;
    } else {
        while (at < line.size() && line[at] != L' ' && line[at] != L'\t') ++at;
    }
    while (at < line.size() && (line[at] == L' ' || line[at] == L'\t')) ++at;
    return line.substr(at);
}

// 发布目录的“最新版本.txt”中 version_dir=<目录名> 行的值；可带 UTF-8 BOM、CRLF。
// 只接受单级目录名（字母、数字、- _ .，且不是 . 或 ..），其余情况返回空，调用方报错而不是猜路径。
[[nodiscard]] inline std::string_view LatestVersionDirectory(std::string_view text) noexcept {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    constexpr std::string_view key = "version_dir=";
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (!line.starts_with(key)) continue;
        const auto value = line.substr(key.size());
        if (value.empty() || value.size() > 128 || value == "." || value == "..") return {};
        for (const char c : value) {
            const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || c == '-' || c == '_' || c == '.';
            if (!allowed) return {};
        }
        return value;
    }
    return {};
}

} // namespace Audastra::Launcher
