#include "War3MapCore/Game/WorldEditStringLoader.h"

#include "War3MapCore/Game/GameDataSource.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace War3Map::Game {
namespace {

std::wstring LowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// 逐段查找子项：优先精确名，其次不区分大小写（在区分大小写的文件系统上也能找到 UI 与 ui）。
std::optional<std::filesystem::path> FindChild(const std::filesystem::path& directory, const std::wstring& name) {
    std::error_code error;
    const auto exact = directory / name;
    if (std::filesystem::exists(exact, error)) return exact;
    const std::wstring lowered = LowerWide(name);
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (LowerWide(entry.path().filename().wstring()) == lowered) return entry.path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindInside(const std::filesystem::path& root, std::string_view gamePath) {
    std::filesystem::path current = root;
    std::size_t start = 0;
    bool any = false;
    while (start <= gamePath.size()) {
        auto end = gamePath.find_first_of("\\/", start);
        if (end == std::string_view::npos) end = gamePath.size();
        const auto segment = gamePath.substr(start, end - start);
        start = end + 1;
        if (segment.empty()) {
            if (end == gamePath.size()) break;
            continue; // 容忍重复分隔符（ui\\WarVK\\icons 形式）
        }
        if (segment == "." || segment == ".." || segment.find(':') != std::string_view::npos) return std::nullopt;
        const auto wide = std::filesystem::path(std::u8string(segment.begin(), segment.end())).wstring();
        const auto child = FindChild(current, wide);
        if (!child) return std::nullopt;
        current = *child;
        any = true;
    }
    if (!any) return std::nullopt;
    // 符号链接或目录联接可能指到根外：规范化后必须仍在根内。
    std::error_code error;
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error) return std::nullopt;
    const auto target = std::filesystem::weakly_canonical(current, error);
    if (error) return std::nullopt;
    const auto relative = target.lexically_relative(base);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return std::nullopt;
    return current;
}

void AddRoot(std::vector<std::filesystem::path>& roots, const std::filesystem::path& candidate) {
    std::error_code error;
    if (!std::filesystem::is_directory(candidate, error)) return;
    if (std::find(roots.begin(), roots.end(), candidate) == roots.end()) roots.push_back(candidate);
}

std::vector<std::filesystem::path> ToolRoots(const std::filesystem::path& toolRoot) {
    std::vector<std::filesystem::path> result;
    if (toolRoot.empty()) return result;
    result.push_back(toolRoot);
    // 选的是 ui 目录时，share 在其上一级。
    if (LowerWide(toolRoot.filename().wstring()) == L"ui" && toolRoot.has_parent_path()) result.push_back(toolRoot.parent_path());
    return result;
}

std::vector<std::filesystem::path> ShareRoots(const std::filesystem::path& toolRoot, std::string_view language) {
    std::vector<std::filesystem::path> roots;
    for (const auto& root : ToolRoots(toolRoot)) {
        if (const auto share = FindChild(root, L"share")) {
            const auto wide = std::filesystem::path(std::u8string(language.begin(), language.end())).wstring();
            if (const auto lang = FindChild(*share, wide)) {
                if (const auto mpq = FindChild(*lang, L"mpq")) AddRoot(roots, *mpq);
            }
        }
    }
    return roots;
}

bool IsUtf8(std::span<const std::byte> bytes) {
    return bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

} // namespace

std::vector<std::filesystem::path> YdweOverlayRoots(const std::filesystem::path& toolRoot, std::string_view language) {
    auto roots = ShareRoots(toolRoot, language);
    if (language != "zh-CN") {
        for (auto& root : ShareRoots(toolRoot, "zh-CN")) AddRoot(roots, root);
    }
    return roots;
}

std::vector<std::filesystem::path> YdweRootDirectories(const std::filesystem::path& toolRoot) {
    std::vector<std::filesystem::path> roots;
    for (const auto& root : ToolRoots(toolRoot)) AddRoot(roots, root);
    return roots;
}

std::vector<std::filesystem::path> YdweEnglishRoots(const std::filesystem::path& toolRoot) {
    return ShareRoots(toolRoot, "en-US");
}

std::optional<std::vector<std::byte>> ReadOverlayFile(
    std::span<const std::filesystem::path> roots, std::string_view gamePath, std::size_t maximumBytes) {
    for (const auto& root : roots) {
        const auto path = FindInside(root, gamePath);
        if (!path) continue;
        std::error_code error;
        if (!std::filesystem::is_regular_file(*path, error)) continue;
        const auto size = std::filesystem::file_size(*path, error);
        if (error || size > maximumBytes) continue;
        std::ifstream input(*path, std::ios::binary);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input || bytes.empty()) return bytes;
    }
    return std::nullopt;
}

std::string DecodeGameText(std::span<const std::byte> bytes) {
    if (bytes.size() >= 3 && bytes[0] == std::byte{0xEF} && bytes[1] == std::byte{0xBB} && bytes[2] == std::byte{0xBF}) {
        bytes = bytes.subspan(3);
    }
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    if (IsUtf8(bytes)) return {data, bytes.size()};
    // GB18030 不可用的系统（精简系统或兼容层）退回 GBK；两者对常用汉字编码相同。
    UINT codePage = 54936;
    int wideSize = MultiByteToWideChar(codePage, 0, data, static_cast<int>(bytes.size()), nullptr, 0);
    if (wideSize <= 0) {
        codePage = 936;
        wideSize = MultiByteToWideChar(codePage, 0, data, static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (wideSize <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
    MultiByteToWideChar(codePage, 0, data, static_cast<int>(bytes.size()), wide.data(), wideSize);
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, text.data(), size, nullptr, nullptr);
    return text;
}

WorldEditStrings LoadWorldEditStrings(const WorldEditStringSources& sources) {
    WorldEditStrings strings;
    const auto addOverlay = [&](const std::vector<std::filesystem::path>& roots, std::string_view label) {
        for (const auto& root : roots) {
            const std::span<const std::filesystem::path> single(&root, 1);
            for (const auto& file : sources.files) {
                if (const auto bytes = ReadOverlayFile(single, file)) {
                    const auto source = std::string(label) + ":" + file;
                    strings.AddText(source, DecodeGameText(*bytes));
                }
            }
        }
    };
    addOverlay(YdweOverlayRoots(sources.toolRoot, sources.language), "ydwe");
    if (sources.game != nullptr) {
        for (const auto& file : sources.files) {
            const auto bytes = sources.game->ReadFile(file);
            if (bytes.Succeeded() && bytes.value) strings.AddText("game:" + file, DecodeGameText(*bytes.value));
        }
    }
    if (sources.language != "en-US") addOverlay(YdweEnglishRoots(sources.toolRoot), "ydwe-en");
    return strings;
}

} // namespace War3Map::Game
