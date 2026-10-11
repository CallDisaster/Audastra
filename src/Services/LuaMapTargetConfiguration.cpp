#include "War3MapCore/Services/MapScriptTargetSelection.h"

#include "War3MapCore/VerifiedFile.h"
#include "../ScriptBuild/ScriptSourceBinding.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <new>
#include <string_view>

namespace War3Map::Services {
namespace {

using LoadResult = Result<LuaMapTargetRequest>;
constexpr std::uint64_t kConfigurationBytes = 64u * 1024u;
constexpr std::uint64_t kProfileBytes = 1024u * 1024u;

LoadResult Fail(const std::filesystem::path& source, std::string code, std::string message,
    std::uint64_t offset = 0, std::uint64_t length = 0) {
    auto result = LoadResult::Failure("Script.LuaMapConfiguration", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{source, offset, length};
    return result;
}

LoadResult NestedFailure(const std::filesystem::path& source, std::string code, std::string message,
    std::vector<Diagnostic> diagnostics) {
    auto result = Fail(source, std::move(code), std::move(message));
    for (auto& diagnostic : diagnostics) {
        if (!diagnostic.origin) diagnostic.origin = OriginSpan{source};
        result.diagnostics.push_back(std::move(diagnostic));
    }
    return result;
}

bool ValidText(std::string_view text) {
    if (text.empty()) return true;
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) {
            return (c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7f;
        })) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0) != 0;
}

bool Digest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool ReservedComponent(std::wstring_view component) {
    component = component.substr(0, component.find(L'.'));
    while (component.ends_with(L' ')) component.remove_suffix(1);
    std::wstring upper(component);
    for (auto& c : upper) if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL"
        || upper == L"CONIN$" || upper == L"CONOUT$") return true;
    return upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT"))
        && ((upper[3] >= L'1' && upper[3] <= L'9') || upper[3] == L'\u00b9'
            || upper[3] == L'\u00b2' || upper[3] == L'\u00b3');
}

bool NormalizePath(const std::filesystem::path& input, std::filesystem::path& output) {
    auto native = input.native();
    if (std::any_of(native.begin(), native.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; })) return false;
    std::replace(native.begin(), native.end(), L'/', L'\\');
    const std::filesystem::path windowsPath(native);
    if (!Script::Detail::NormalizeSourcePath(windowsPath, output)) return false;
    for (const auto& component : windowsPath.relative_path())
        if (ReservedComponent(component.native())) return false;
    return true;
}

std::filesystem::path Utf8Path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string OwnedText(const std::vector<std::byte>& bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

Result<LuaMapTargetRequest> LoadLuaMapTargetConfiguration(const std::filesystem::path& configurationFile) {
    try {
        std::filesystem::path configuration;
        if (!NormalizePath(configurationFile, configuration))
            return Fail(configurationFile, "LUA_MAP_CONFIGURATION_PATH_INVALID", "配置必须是无歧义的普通绝对路径。");
        // 复用有界来源捕获：持只读租约先核大小，再取SHA并由Core ReadVerifiedFile取得owned字节。
        // 这是逐文件捕获，不是整套运行库或保存期间的长租约。
        const auto captured = Script::Detail::CaptureSourceBinding(configuration,
            Script::ScriptToolchainBindingKind::RegularFile, true, kConfigurationBytes);
        if (!captured.Succeeded() || !captured.value)
            return NestedFailure(configuration, "LUA_MAP_CONFIGURATION_CAPTURE_FAILED",
                "无法捕获不超过64 KiB的配置文件。", captured.diagnostics);
        const auto text = OwnedText(captured.value->bytes);
        if (!ValidText(text)) return Fail(configuration, "LUA_MAP_CONFIGURATION_UTF8_INVALID",
            "配置必须为有效UTF-8，且不能含NUL或其他非换行控制字符。");

        LuaMapTargetRequest request;
        std::filesystem::path profilePath, checkerPath;
        std::string profileHash, checkerHash;
        std::vector<std::filesystem::path> runtimeDirectories;
        constexpr std::array<std::string_view, 5> required{"schema", "profile", "checker", "player_layout", "language"};
        std::array<bool, required.size()> present{};
        bool dialectPresent = false;
        std::size_t cursor = text.starts_with("\xef\xbb\xbf") ? 3 : 0;
        std::size_t lineNumber = 0;
        while (cursor < text.size()) {
            const auto start = cursor;
            const auto end = text.find_first_of("\r\n", start);
            const auto line = std::string_view(text).substr(start, end == text.npos ? text.size() - start : end - start);
            cursor = end == text.npos ? text.size() : end + 1;
            if (end != text.npos && text[end] == '\r' && cursor < text.size() && text[cursor] == '\n') ++cursor;
            ++lineNumber;
            if (line.empty() || line.starts_with('#')) continue;
            const auto fail = [&](const char* code, const char* detail) {
                return Fail(configuration, code, "配置第" + std::to_string(lineNumber) + "行：" + detail, start, line.size());
            };
            std::array<std::string_view, 3> columns;
            std::size_t columnCount = 0, position = 0;
            for (;;) {
                const auto tab = line.find('\t', position);
                const auto field = line.substr(position, tab == line.npos ? line.size() - position : tab - position);
                if (field.empty() || columnCount == columns.size())
                    return fail("LUA_MAP_CONFIGURATION_FORMAT", "存在空列或多余列；字段不自动裁剪。");
                columns[columnCount++] = field;
                if (tab == line.npos) break;
                position = tab + 1;
            }
            const auto key = columns[0];
            const auto found = std::find(required.begin(), required.end(), key);
            if (found == required.end() && key != "runtime" && key != "dialect")
                return fail("LUA_MAP_CONFIGURATION_FIELD_UNKNOWN", "未知配置字段。");
            if (found != required.end()) {
                const auto index = static_cast<std::size_t>(found - required.begin());
                if (present[index]) return fail("LUA_MAP_CONFIGURATION_FIELD_DUPLICATE", "唯一字段重复。");
                present[index] = true;
            }
            if (key == "dialect") {
                if (dialectPresent) return fail("LUA_MAP_CONFIGURATION_FIELD_DUPLICATE", "唯一字段重复。");
                dialectPresent = true;
            }
            if (columnCount != (key == "profile" || key == "checker" ? 3u : 2u))
                return fail("LUA_MAP_CONFIGURATION_FORMAT", "字段列数不匹配。");
            if (key == "schema") {
                if (columns[1] != "forge-lua-map-save-v1")
                    return fail("LUA_MAP_CONFIGURATION_FORMAT", "schema不匹配。");
            } else if (key == "player_layout") {
                if (columns[1] == "classic12") request.playerLayout = Script::MapInitializationPlayerLayout::Classic12;
                else if (columns[1] == "extended24") request.playerLayout = Script::MapInitializationPlayerLayout::Extended24;
                else return fail("LUA_MAP_CONFIGURATION_FORMAT", "player_layout必须为classic12或extended24。");
            } else if (key == "language") {
                if (columns[1] == "preserve") request.changeLanguageToLua = false;
                else if (columns[1] == "select-lua") request.changeLanguageToLua = true;
                else return fail("LUA_MAP_CONFIGURATION_FORMAT", "language必须为preserve或select-lua。");
            } else if (key == "dialect") {
                if (columns[1] == "native-v31") request.dialect = Script::LuaMapSaveDialect::NativeV31;
                else if (columns[1] == "classic-127-ydwe") request.dialect = Script::LuaMapSaveDialect::Classic127YDWE;
                else return fail("LUA_MAP_CONFIGURATION_FORMAT", "dialect必须为native-v31或classic-127-ydwe。");
            } else {
                std::filesystem::path path;
                if (!NormalizePath(Utf8Path(columns[1]), path))
                    return fail("LUA_MAP_CONFIGURATION_PATH_INVALID", "来源必须是无歧义的普通绝对路径。");
                if (key == "runtime") {
                    if (runtimeDirectories.size() >= 8)
                        return fail("LUA_MAP_CONFIGURATION_RUNTIME_LIMIT", "运行时目录最多8项。");
                    if (std::any_of(runtimeDirectories.begin(), runtimeDirectories.end(),
                            [&](const auto& existing) { return SamePath(existing, path); }))
                        return fail("LUA_MAP_CONFIGURATION_RUNTIME_DUPLICATE", "运行时目录重复（按Windows路径忽略大小写）。");
                    runtimeDirectories.push_back(std::move(path));
                } else {
                    if (!Digest(columns[2]))
                        return fail("LUA_MAP_CONFIGURATION_SHA_INVALID", "来源SHA256必须是64位十六进制摘要。");
                    if (key == "profile") { profilePath = std::move(path); profileHash = columns[2]; }
                    else { checkerPath = std::move(path); checkerHash = columns[2]; }
                }
            }
        }
        if (!present[0] || !present[1] || !present[3] || !present[4]
            || (request.dialect == Script::LuaMapSaveDialect::NativeV31 && !present[2]))
            return Fail(configuration, "LUA_MAP_CONFIGURATION_REQUIRED_FIELD_MISSING",
                "配置缺少唯一必填字段；原生v31还需checker，经典档案使用内置Lua5.3检查。");
        if (request.dialect == Script::LuaMapSaveDialect::Classic127YDWE
            && (request.changeLanguageToLua || request.playerLayout != Script::MapInitializationPlayerLayout::Classic12))
            return Fail(configuration, "LUA_MAP_CONFIGURATION_CLASSIC_CONFLICT",
                "经典Lua档案必须保留W3I语言并使用classic12。");

        const auto profileBytes = ReadVerifiedFile(profilePath, profileHash, kProfileBytes);
        if (!profileBytes.Succeeded() || !profileBytes.value)
            return NestedFailure(profilePath, "LUA_MAP_CONFIGURATION_PROFILE_CAPTURE_FAILED",
                "档案未通过指定SHA256及1 MiB读取预算。", profileBytes.diagnostics);
        const auto profileText = OwnedText(*profileBytes.value);
        if (!ValidText(profileText)) return Fail(profilePath, "LUA_MAP_CONFIGURATION_PROFILE_UTF8_INVALID",
            "目标档案必须是有效UTF-8，且不能含NUL或其他非换行控制字符。");
        auto profile = ParseLuaCandidateProfile(profileText);
        if (!profile.Succeeded() || !profile.value)
            return NestedFailure(profilePath, "LUA_MAP_CONFIGURATION_PROFILE_INVALID",
                "目标档案格式无效。", std::move(profile.diagnostics));
        request.profile = std::move(*profile.value);
        if (request.dialect == Script::LuaMapSaveDialect::Classic127YDWE) {
            if (present[2] || !runtimeDirectories.empty())
                return Fail(configuration, "LUA_MAP_CONFIGURATION_CLASSIC_CHECKER_CONFLICT",
                    "经典档案使用内置Lua5.3检查，不接受外部checker或runtime字段。");
        } else {
            auto checker = Script::CaptureLuaSyntaxChecker(checkerPath, checkerHash, runtimeDirectories);
            if (!checker.Succeeded() || !checker.value)
                return NestedFailure(checkerPath, "LUA_MAP_CONFIGURATION_CHECKER_INVALID",
                    "检查器或运行时目录未通过身份捕获。", std::move(checker.diagnostics));
            request.checker = std::move(*checker.value);
        }
        // 捕获身份不执行配置、档案或检查器，也不证明所选文件确实是Lua解释器。
        return LoadResult::Success(std::move(request));
    } catch (const std::bad_alloc&) {
        return Fail(configurationFile, "LUA_MAP_CONFIGURATION_ALLOCATION", "无法分配Lua地图目标的独立快照。");
    } catch (const std::exception&) {
        return Fail(configurationFile, "LUA_MAP_CONFIGURATION_FAILED", "Lua地图配置读取失败，未返回部分目标。");
    }
}

} // namespace War3Map::Services
