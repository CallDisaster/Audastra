#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace War3Map::Jass {

// 只检查路径拼写，不探测或打开文件。归档成员仍由归档层作不透明字节保留。
[[nodiscard]] inline bool IsAiScriptTextSource(const std::filesystem::path& path) {
    auto name = path.generic_u8string();
    std::replace(name.begin(), name.end(), u8'\\', u8'/');
    auto normalized = std::filesystem::path(name).lexically_normal().filename().u8string();
    // Windows 数据流别名仍属于同一个宿主文件，不能借流名改变扩展名判定。
    if (const auto stream = normalized.find(u8':'); stream != std::u8string::npos) normalized.resize(stream);
    while (!normalized.empty() && (normalized.back() == u8'.' || normalized.back() == u8' ')) normalized.pop_back();
    const auto dot = normalized.rfind(u8'.');
    if (dot == std::u8string::npos) return false;
    auto extension = normalized.substr(dot);
    for (auto& c : extension) if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
    return extension == u8".ai";
}

inline constexpr std::string_view kAiScriptTextCode = "SCRIPT_INCLUDE_AI_UNSUPPORTED";
inline constexpr std::string_view kAiScriptTextMessage = "AI 文件按不透明成员保留，不作为文本包含。";

} // namespace War3Map::Jass
