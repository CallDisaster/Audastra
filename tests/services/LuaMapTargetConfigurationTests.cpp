#include "War3MapCore/Services/MapScriptTargetSelection.h"

#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Services;
using Loaded = Result<LuaMapTargetRequest>;

void Require(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error(reason);
}

bool Code(const Loaded& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void Rejected(const Loaded& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value && Code(result, code), "expected rejected configuration: " + std::string(code));
}

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string UpperAscii(std::string text) {
    for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}

void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(output), "write original configuration fixture");
}

std::string Hash(std::string_view text) {
    const auto hash = Sha256Text(text);
    Require(hash.Succeeded() && hash.value, "hash original fixture");
    return *hash.value;
}

std::string Join(const std::vector<std::string>& lines, std::string_view newline = "\n") {
    std::string text;
    for (const auto& line : lines) { text += line; text += newline; }
    return text;
}

class Fixture {
public:
    std::filesystem::path root, configuration, profile, checker;
    std::vector<std::filesystem::path> runtimes;
    const std::string profileText = "forge-lua-target-v1\nidentity\toriginal-配置档案\n"
        "type\tunit\thandle\napi\tOriginalNative\tnothing\tunit\n"
        "constant\tORIGINAL_CONSTANT\tinteger\ncallback\tOriginalStart\t17\n";
    std::string checkerHash;

    Fixture() {
        root = std::filesystem::temp_directory_path() / ("forge-lua-map-configuration-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(root), "create private configuration fixture");
        root = std::filesystem::canonical(root);
        configuration = root / L"中文 配置" / L"地图目标.tsv";
        profile = root / L"中文 档案" / L"显式 API.tsv";
        checker = root / L"中文 检查器" / L"仅身份 不执行.exe";
        Write(profile, profileText);
        std::filesystem::create_directories(checker.parent_path());
        std::wstring executable(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        Require(size != 0 && size < executable.size(), "locate test executable identity");
        executable.resize(size);
        Require(std::filesystem::copy_file(executable, checker), "copy test executable for identity only");
        const auto hash = Sha256File(checker);
        Require(hash.Succeeded() && hash.value, "hash test executable identity");
        checkerHash = *hash.value;
        for (unsigned index = 0; index < 8; ++index) {
            runtimes.push_back(root / ("runtime " + std::to_string(index)));
            Require(std::filesystem::create_directory(runtimes.back()), "create explicit runtime directory fixture");
        }
    }

    std::vector<std::string> Lines() const {
        return {"schema\tforge-lua-map-save-v1", "profile\t" + Utf8(profile) + "\t" + Hash(profileText),
            "checker\t" + Utf8(checker) + "\t" + checkerHash, "player_layout\tclassic12", "language\tpreserve"};
    }

    Loaded LoadText(std::string_view text) const {
        Write(configuration, text);
        return LoadLuaMapTargetConfiguration(configuration);
    }

    Loaded Load(const std::vector<std::string>& lines, std::string_view newline = "\n") const {
        return LoadText(Join(lines, newline));
    }

    void RemoveSuccessfulFixture() const {
        const auto checked = std::filesystem::canonical(root);
        Require(checked == root && root.filename().wstring().starts_with(L"forge-lua-map-configuration-"),
            "verify exact private cleanup root");
        std::filesystem::remove_all(root);
    }
};

void AcceptsExplicitOwnedInputs(Fixture& fixture) {
    for (const auto* newline : {"\n", "\r\n", "\r"}) {
        auto lines = fixture.Lines();
        lines.insert(lines.begin(), {"# whole-line comment", ""});
        const auto loaded = fixture.Load(lines, newline);
        Require(loaded.Succeeded() && loaded.value && loaded.value->checker.Identity(), "LF/CRLF/CR capture succeeds");
        const auto& request = *loaded.value;
        Require(request.profile.target.identity == "original-配置档案"
            && request.profile.target.apis.size() == 1 && request.profile.target.apis[0].name == "OriginalNative"
            && request.profile.target.apis[0].parameters == std::vector<std::string>{"unit"}
            && request.profile.callbacks.size() == 1 && request.profile.callbacks[0].symbol == "OriginalStart"
            && request.profile.callbacks[0].triggerId == 17 && !request.changeLanguageToLua
            && request.playerLayout == Script::MapInitializationPlayerLayout::Classic12,
            "shared parser preserves declared APIs, callback, language and player layout");
        Require(request.checker.Identity()->executable == fixture.checker
            && request.checker.Identity()->sha256 == fixture.checkerHash
            && request.checker.Identity()->runtimeDirectories.empty(), "checker has captured identity, no inferred runtime roots");
    }
    auto lines = fixture.Lines();
    lines[3] = "player_layout\textended24";
    lines[4] = "language\tselect-lua";
    lines[1] = "profile\t" + Utf8(fixture.profile) + "\t" + UpperAscii(Hash(fixture.profileText));
    lines[2] = "checker\t" + Utf8(fixture.checker) + "\t" + UpperAscii(fixture.checkerHash);
    for (const auto& path : fixture.runtimes) lines.push_back("runtime\t" + Utf8(path));
    const auto extended = fixture.LoadText("\xef\xbb\xbf" + Join(lines));
    Require(extended.Succeeded() && extended.value->changeLanguageToLua
        && extended.value->playerLayout == Script::MapInitializationPlayerLayout::Extended24
        && extended.value->checker.Identity()->runtimeDirectories == fixture.runtimes,
        "optional leading UTF8 BOM, uppercase explicit digests, both enum choices and eight ordered runtimes work");
}

void RejectsMalformedRecords(Fixture& fixture) {
    const auto baseline = fixture.Lines();
    for (std::size_t index = 0; index < baseline.size(); ++index) {
        auto lines = baseline;
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
        Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_REQUIRED_FIELD_MISSING");
        lines = baseline;
        lines.push_back(lines[index]);
        Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FIELD_DUPLICATE");
    }
    for (const auto& replacement : {"schema\tforge-lua-map-save-v2", "schema", "schema\t", "schema\tforge-lua-map-save-v1\textra",
            "schema\tforge-lua-map-save-v1\textra\tmore", "schema\t forge-lua-map-save-v1"}) {
        auto lines = baseline; lines[0] = replacement;
        Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FORMAT");
    }
    auto lines = baseline;
    lines.push_back("execute\tanything");
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FIELD_UNKNOWN");
    lines.back() = " # indented comments are not records";
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FIELD_UNKNOWN");
    lines = baseline; lines[4] = "language\tselect-lua # no inline comment syntax";
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FORMAT");
    lines[4] = "language\tjass";
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FORMAT");
    lines = baseline; lines[3] = "player_layout\tauto";
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_FORMAT");
    for (const auto* hash : {"", "abcd", "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"}) {
        lines = baseline; lines[1] = "profile\t" + Utf8(fixture.profile) + "\t" + hash;
        Rejected(fixture.Load(lines), *hash ? "LUA_MAP_CONFIGURATION_SHA_INVALID" : "LUA_MAP_CONFIGURATION_FORMAT");
    }
    lines = baseline; lines.push_back("runtime\t" + Utf8(fixture.runtimes[0]));
    lines.push_back("runtime\t" + UpperAscii(Utf8(fixture.runtimes[0] / ".")));
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_RUNTIME_DUPLICATE");
    lines = baseline;
    for (const auto& path : fixture.runtimes) lines.push_back("runtime\t" + Utf8(path));
    lines.push_back("runtime\t" + Utf8(fixture.root / "ninth runtime"));
    Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_RUNTIME_LIMIT");

    const auto text = "\xef\xbb\xbf" + Join(baseline, "\r\n") + "unknown\tvalue\r\n";
    const auto bad = fixture.LoadText(text);
    Rejected(bad, "LUA_MAP_CONFIGURATION_FIELD_UNKNOWN");
    Require(bad.diagnostics[0].origin && bad.diagnostics[0].origin->source == fixture.configuration
        && bad.diagnostics[0].origin->offset == text.find("unknown\tvalue")
        && bad.diagnostics[0].origin->length == 13, "diagnostic position is original byte offset, not a line number");
}

void RejectsTextAndAmbiguousPaths(Fixture& fixture) {
    for (const auto& invalid : {std::string("\xff"), std::string("\xc0\xaf"), std::string("\xe4\xb8"),
            std::string("\xed\xa0\x80"), std::string(1, '\0'), std::string(1, '\x1a')})
        Rejected(fixture.LoadText(Join(fixture.Lines()) + "#" + invalid), "LUA_MAP_CONFIGURATION_UTF8_INVALID");
    Rejected(LoadLuaMapTargetConfiguration("relative.tsv"), "LUA_MAP_CONFIGURATION_PATH_INVALID");
    Rejected(LoadLuaMapTargetConfiguration(fixture.root / ".." / "target.tsv"), "LUA_MAP_CONFIGURATION_PATH_INVALID");
    Rejected(LoadLuaMapTargetConfiguration(std::filesystem::path(fixture.configuration.native() + L'\0')),
        "LUA_MAP_CONFIGURATION_PATH_INVALID");
    std::vector<std::string> invalidPaths{"relative.tsv", "C:relative.tsv", "\\rooted.tsv",
        Utf8(fixture.root / ".." / "parent.tsv"), Utf8(fixture.root / "trailing."), Utf8(fixture.root / "trailing "),
        Utf8(fixture.root / "NUL.txt"), Utf8(fixture.root / "COM1"), Utf8(fixture.profile) + ":stream",
        "\\\\?\\" + Utf8(fixture.profile), "//?/" + Utf8(fixture.profile)};
    for (const auto& path : invalidPaths) {
        for (unsigned kind = 0; kind != 3; ++kind) {
            auto lines = fixture.Lines();
            if (kind == 0) lines[1] = "profile\t" + path + "\t" + Hash(fixture.profileText);
            else if (kind == 1) lines[2] = "checker\t" + path + "\t" + fixture.checkerHash;
            else lines.push_back("runtime\t" + path);
            Rejected(fixture.Load(lines), "LUA_MAP_CONFIGURATION_PATH_INVALID");
        }
    }
}

void EnforcesBudgetsAndBindings(Fixture& fixture) {
    auto exact = Join(fixture.Lines()) + "#";
    exact.resize(64u * 1024u, 'x');
    Require(fixture.LoadText(exact).Succeeded(), "64 KiB configuration fits inclusive budget");
    exact.push_back('x');
    Rejected(fixture.LoadText(exact), "SCRIPT_USER_SOURCE_BINDING_BUDGET");
    const std::string hugeProfile(1024u * 1024u + 1u, 'x');
    Write(fixture.profile, hugeProfile);
    auto lines = fixture.Lines(); lines[1] = "profile\t" + Utf8(fixture.profile) + "\t" + Hash(hugeProfile);
    Rejected(fixture.Load(lines), "VERIFIED_FILE_BUDGET_EXCEEDED");
    Write(fixture.profile, fixture.profileText);
    lines = fixture.Lines(); lines[1] = "profile\t" + Utf8(fixture.profile) + "\t" + std::string(64, '0');
    Rejected(fixture.Load(lines), "VERIFIED_FILE_SHA_MISMATCH");
    lines = fixture.Lines(); lines[2] = "checker\t" + Utf8(fixture.checker) + "\t" + std::string(64, '0');
    Rejected(fixture.Load(lines), "LUA_CHECKER_SHA_MISMATCH");
    lines = fixture.Lines(); lines.push_back("runtime\t" + Utf8(fixture.profile));
    Rejected(fixture.Load(lines), "LUA_CHECKER_RUNTIME_DIRECTORY_INVALID");
    lines.back() = "runtime\t" + Utf8(fixture.root / "missing-runtime");
    Rejected(fixture.Load(lines), "LUA_CHECKER_RUNTIME_DIRECTORY_INVALID");
    for (const auto& invalid : {std::string("forge-lua-target-v2\nidentity\tbad\n"),
            std::string("forge-lua-target-v1\nidentity\t\xff\n")}) {
        Write(fixture.profile, invalid);
        lines = fixture.Lines(); lines[1] = "profile\t" + Utf8(fixture.profile) + "\t" + Hash(invalid);
        Rejected(fixture.Load(lines), invalid.find('\xff') == invalid.npos ? "LUA_PROFILE_FORMAT" : "LUA_MAP_CONFIGURATION_PROFILE_UTF8_INVALID");
    }
    Write(fixture.profile, fixture.profileText);
    const auto sourceHash = Sha256File(fixture.profile);
    const auto loaded = fixture.Load(fixture.Lines());
    Require(loaded.Succeeded() && Sha256File(fixture.profile).value == sourceHash.value
        && Sha256File(fixture.checker).value == std::optional<std::string>{fixture.checkerHash},
        "loading reads configuration and profile without modifying sources or checker");
    Write(fixture.configuration, "changed\n");
    Write(fixture.profile, "changed profile\n");
    Write(fixture.checker, "changed identity-only checker\n");
    Require(loaded.value->profile.target.identity == "original-配置档案"
        && loaded.value->profile.callbacks[0].symbol == "OriginalStart"
        && loaded.value->checker.Identity()->sha256 == fixture.checkerHash
        && loaded.value->checker.Identity()->bytes > 1000,
        "returned parsed profile and checker identity do not reread later disk changes");
}

} // namespace

void ClassicConfigurationUsesEmbeddedChecker(Fixture& fixture) {
    auto lines = fixture.Lines();
    std::erase_if(lines, [](const auto& line) { return line.starts_with("checker\t") || line.starts_with("runtime\t"); });
    lines.push_back("dialect\tclassic-127-ydwe");
    const auto loaded = fixture.Load(lines);
    Require(loaded.Succeeded() && loaded.value
        && loaded.value->dialect == Script::LuaMapSaveDialect::Classic127YDWE
        && !loaded.value->checker.Identity(), "classic configuration cannot depend on an external Lua checker");
    auto invalid = fixture.Lines(); invalid.push_back("dialect\tclassic-127-ydwe");
    Rejected(fixture.Load(invalid), "LUA_MAP_CONFIGURATION_CLASSIC_CHECKER_CONFLICT");
    invalid = lines; invalid.push_back("runtime\t" + Utf8(fixture.runtimes.front()));
    Rejected(fixture.Load(invalid), "LUA_MAP_CONFIGURATION_CLASSIC_CHECKER_CONFLICT");
    invalid = lines;
    for (auto& line : invalid) if (line.starts_with("language\t")) line = "language\tselect-lua";
    Rejected(fixture.Load(invalid), "LUA_MAP_CONFIGURATION_CLASSIC_CONFLICT");
    invalid = lines; invalid.push_back("dialect\tnative-v31");
    Rejected(fixture.Load(invalid), "LUA_MAP_CONFIGURATION_FIELD_DUPLICATE");
}

int main() {
    try {
        Fixture fixture;
        AcceptsExplicitOwnedInputs(fixture);
        RejectsMalformedRecords(fixture);
        RejectsTextAndAmbiguousPaths(fixture);
        ClassicConfigurationUsesEmbeddedChecker(fixture);
        EnforcesBudgetsAndBindings(fixture);
        fixture.RemoveSuccessfulFixture();
        std::cout << "Lua map configuration: strict UTF8 TSV, normal newlines, explicit paths/SHA, budgets and owned identity passed; no checker executed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua map configuration failure: " << error.what() << '\n';
        return 1;
    }
}
