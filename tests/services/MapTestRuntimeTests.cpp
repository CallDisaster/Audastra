#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/ApplicationPaths.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Services/ClassicLuaMapSources.h"
#include "War3MapCore/Runtime/LuaModuleNames.h"
#include "../MapDataFixtures.h"
#include "../../src/Core/Mpq/MpqCrypt.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
template<class T> void Success(const Result<T>& result, const char* message) {
    if (result.Succeeded()) return;
    std::string text(message); for (const auto& diagnostic : result.diagnostics) text += "\n" + diagnostic.code + ": " + diagnostic.message;
    throw std::runtime_error(text);
}
template<class T> bool Code(const Result<T>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
template<class T> void FailureCode(const Result<T>& result, std::string_view code, const char* message) {
    if (!result.Succeeded() && Code(result, code)) return;
    std::string text(message);
    for (const auto& diagnostic : result.diagnostics) text += "\n" + diagnostic.code + ": " + diagnostic.message;
    throw std::runtime_error(text);
}
std::string_view BoundedFirstDiagnostic(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return {};
    const auto& text = diagnostics.front().message;
    std::size_t end = std::min<std::size_t>(text.size(), 320);
    if (end < text.size()) {
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0u) == 0x80u) --end;
    }
    return std::string_view(text).substr(0, end);
}
void Write(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    Require(IO::WriteFileBytes(path, bytes).Succeeded(), "write only owned synthetic fixture file");
}

// 原创微型 MPQ：脚本存在于真实名字 hash，但不会列入 listfile；仅修改脚本块标志。
std::vector<std::byte> AnonymousScriptArchive(std::uint32_t scriptFlags, bool withListfile = true) {
    std::vector<MapArchiveMember> members{{"war3map.j", Bytes("function config takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\n    call DebugMessage(\"anonymous script\")\nendfunction\n")},
        {"resource/marker.bin", Bytes("owned marker")}};
    if (withListfile) members.push_back({"(listfile)", Bytes("resource/marker.bin\r\n")});
    constexpr std::uint32_t hashCount = 16;
    const auto blockCount = static_cast<std::uint32_t>(members.size());
    std::vector<std::byte> bytes(32);
    std::vector<std::uint32_t> positions;
    for (const auto& member : members) {
        positions.push_back(static_cast<std::uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), member.data.begin(), member.data.end());
    }
    const auto hashPosition = static_cast<std::uint32_t>(bytes.size()), blockPosition = hashPosition + hashCount * 16;
    bytes.resize(blockPosition + blockCount * 16);
    const auto put = [&](std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put(0, 0x1a51504d); put(4, 32); put(8, static_cast<std::uint32_t>(bytes.size()));
    put(12, 0x00030000); put(16, hashPosition); put(20, blockPosition); put(24, hashCount); put(28, blockCount);
    std::fill(bytes.begin() + hashPosition, bytes.begin() + blockPosition, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    for (std::uint32_t index = 0; index < blockCount; ++index) {
        const auto& member = members[index];
        auto bucket = crypt.HashString(member.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (bytes[hashPosition + bucket * 16 + 12] != std::byte{0xff}) bucket = (bucket + 1) & (hashCount - 1);
        const auto hash = hashPosition + bucket * 16, block = blockPosition + index * 16;
        put(hash, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameA));
        put(hash + 4, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameB)); put(hash + 8, 0); put(hash + 12, index);
        put(block, positions[index]); put(block + 4, static_cast<std::uint32_t>(member.data.size()));
        put(block + 8, static_cast<std::uint32_t>(member.data.size()));
        put(block + 12, index == 0 ? scriptFlags : Mpq::kFileExists | Mpq::kFileSingleUnit);
    }
    crypt.EncryptBlock(bytes.data() + hashPosition, hashCount * 16,
        crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockPosition, blockCount * 16,
        crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    return bytes;
}

// 原创最小 PE，仅作静态架构/导出表验证；本测试绝不执行这些字节。
std::vector<std::byte> Pe(bool dll, bool initialize = false, std::uint16_t machine = 0x014c, bool lua54 = false) {
    std::vector<std::byte> bytes(0x600);
    const auto u16 = [&](std::size_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::byte>(value & 255); bytes[offset + 1] = static_cast<std::byte>(value >> 8);
    };
    const auto u32 = [&](std::size_t offset, std::uint32_t value) {
        u16(offset, static_cast<std::uint16_t>(value & 65535)); u16(offset + 2, static_cast<std::uint16_t>(value >> 16));
    };
    u16(0, 0x5a4d); u32(60, 0x80); u32(0x80, 0x00004550);
    u16(0x84, machine); u16(0x86, 1); u16(0x94, 224); u16(0x96, static_cast<std::uint16_t>(0x0002 | (dll ? 0x2000 : 0)));
    constexpr std::size_t optional = 0x98, section = optional + 224;
    u16(optional, 0x010b); u32(optional + 16, 0x1000); u32(optional + 28, 0x10000000);
    u32(optional + 32, 0x1000); u32(optional + 36, 0x200); u32(optional + 56, 0x2000);
    u32(optional + 60, 0x200); u16(optional + 68, 3); u32(optional + 92, 16);
    std::memcpy(bytes.data() + section, ".text", 5); u32(section + 8, 0x400); u32(section + 12, 0x1000);
    u32(section + 16, 0x400); u32(section + 20, 0x200); u32(section + 36, 0x60000020);
    bytes[0x200] = std::byte{0xc3};
    if (initialize) {
        u32(optional + 96, 0x1100); u32(optional + 100, 0x100);
        u32(0x300 + 20, 1); u32(0x300 + 24, 1); u32(0x300 + 28, 0x1140);
        u32(0x300 + 32, 0x1144); u32(0x300 + 36, 0x1148);
        u32(0x340, 0x1000); u32(0x344, 0x1160); u16(0x348, 0);
        std::memcpy(bytes.data() + 0x360, "Initialize", 11);
    }
    if (lua54) {
        u32(optional + 104, 0x1200); u32(optional + 108, 40);
        u32(0x400 + 12, 0x1240); u32(0x400 + 16, 0x1250);
        std::memcpy(bytes.data() + 0x440, "lua54.dll", 10);
    }
    return bytes;
}
constexpr std::string_view kCommon = R"JASS(native DebugMessage takes string message returns nothing
native GetRandomInt takes integer low, integer high returns integer
native ScheduleCallback takes code callback returns nothing
)JASS";
constexpr std::string_view kBlizzard = R"JASS(function BJDebugMsg takes string message returns nothing
    call DebugMessage(message)
endfunction
)JASS";
std::string Script(std::string_view body, std::string_view declarations = {}) {
    return std::string(declarations) + "function config takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\n" + std::string(body) + "\nendfunction\n";
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-runtime-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path game = root / "game", runtime = root / "runtime", helper = root / "helpers";
    unsigned serial = 0;
    Fixture(std::string_view common = kCommon, std::string_view blizzard = kBlizzard) {
        Require(std::filesystem::create_directory(root), "create fresh runtime fixture root");
        std::filesystem::create_directory(game);
        const std::vector<MapArchiveMember> base{{"scripts/common.j", Bytes(common)}, {"scripts/blizzard.j", Bytes(blizzard)}};
        Success(WriteVerifiedArchive(game / "war3.mpq", base), "write synthetic selected game scripts");
        const std::vector<MapArchiveMember> patch{{"fixture-patch.txt", Bytes("owned tiny fixture")}};
        Success(WriteVerifiedArchive(game / "war3patch.mpq", patch), "write tiny readable patch MPQ");
        Write(game / "war3.exe", Pe(false)); Write(game / "Game.dll", Pe(true)); Write(game / "storm.dll", Bytes("ordinary presence fixture"));
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    Services::MapTestArtifact Artifact(const std::vector<MapArchiveMember>& members, const std::filesystem::path& ydwe = {}) {
        const auto name = std::to_string(++serial);
        const auto source = WriteVerifiedArchive(root / ("author-" + name + ".w3x"), members); Success(source, "write original map fixture");
        Services::MapTestRequest request;
        request.inputMap = source.value->path; request.sourceMap = source.value->path;
        request.expectedInputSha256 = source.value->sha256; request.gameRoot = game;
        request.runtimeYdweRoot = ydwe;
        request.workspace = root / ("prepared-" + name);
        const auto artifact = Services::PrepareMapTest(request); Success(artifact, "freeze independent map fixture");
        return *artifact.value;
    }
    Services::MapTestArtifact Artifact(std::string_view script, const std::filesystem::path& ydwe = {}) {
        return Artifact(std::vector<MapArchiveMember>{{"war3map.j", Bytes(script)}, {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}}, ydwe);
    }
    void Runtime(Services::MapTestArtifact& artifact,
        Services::MapTestRuntimeLayout layout = Services::MapTestRuntimeLayout::Flat, bool automatic = false) {
        std::size_t index = 0;
        for (const auto* name : {"ydbase.dll", "lua53.dll", "ydwar3.dll", "yd_loader.dll", "yd_jass_api.dll", "yd_lua_engine.dll"}) {
            auto directory = runtime;
            if (layout == Services::MapTestRuntimeLayout::YdweTree) directory /= index < 3 ? "bin" : "plugin/warcraft3";
            ++index;
            Write(directory / name, Pe(true, std::string_view(name) == "yd_jass_api.dll" || std::string_view(name) == "yd_lua_engine.dll"));
        }
        Write(helper / "audastra-warcraft-launcher.exe", Pe(false)); Write(helper / "audastra-warcraft-bootstrap.dll", Pe(true));
        artifact.runtimeRoot = automatic ? std::filesystem::path{} : runtime;
        artifact.runtimeYdweRoot = automatic ? runtime : std::filesystem::path{};
        artifact.runtimeLayout = automatic ? Services::MapTestRuntimeLayout::Flat : layout;
        artifact.runtimeBridge = helper / "audastra-warcraft-launcher.exe";
        artifact.runtimeBootstrap = helper / "audastra-warcraft-bootstrap.dll";
    }
    void Unchanged(const Services::MapTestArtifact& artifact) {
        Require(*Sha256File(artifact.sourceMap).value == artifact.inputSha256, "runtime planning preserves original map byte identity");
        Require(!std::filesystem::exists(game / "Maps"), "runtime planning never copies maps into game or launches processes");
    }
};

void ExternalDependencyCatalog() {
    Fixture fixture;
    const auto directory = Services::ExternalDependenciesDirectory(fixture.root, {});
    Require(Services::ValidateExternalDependenciesDirectory(directory / L"").Succeeded(),
        "trailing separator is accepted");
    Require(!Services::ValidateExternalDependenciesDirectory(fixture.root / L"", fixture.root / "test-maps").Succeeded() &&
        Services::ValidateExternalDependenciesDirectory(directory, fixture.root / "test-maps").Succeeded(),
        "test workspace ancestor refused but a sibling directory is permitted");
    auto status = Services::QueryExternalDependencies(directory);
    Success(status, "query an absent external dependency directory without creating it");
    Require(!status.value->exists && !std::filesystem::exists(directory), "status is read-only");
    Require(Services::EnsureExternalDependenciesDirectory(directory).Succeeded(), "create empty external dependency directory on request");
    const auto provider = directory / "original";
    Write(directory / "unlisted.dll", Pe(true, true));
    Write(provider / "original.dll", Pe(true, true));
    Write(provider / "provider.json", Bytes(R"json({"schema":1,"id":"original","abi":"audastra-native-provider-v1","modules":[
        {"file":"original.dll","entry":"Initialize","natives":{"AudastraFixtureAdd":"(I)I"}}]})json"));
    status = Services::QueryExternalDependencies(directory);
    Success(status, "read external dependency status");
    Require(status.value->providers.size() == 1 && status.value->providers.front().usable &&
        status.value->undescribedDlls.size() == 1 && status.value->undescribedDlls.front().filename() == "unlisted.dll",
        "only a described PE is eligible");
    const auto selected = Services::SelectExternalDependencyFiles(*status.value, {{"AudastraFixtureAdd", "(I)I"}});
    Success(selected, "resolve an exact native signature");
    Require(selected.value->size() == 1 && selected.value->front().path == provider / "original.dll" &&
        selected.value->front().sha256 == *Sha256File(provider / "original.dll").value, "plan binds only the expected file SHA");
    const auto args = Services::BuildMapTestProviderArguments(*selected.value, false);
    Success(args, "plan command fragment uses only covered modules without requiring a real Game.dll");
    Require(args.value->size() == 6 && args.value->at(0) == L"--runtime-mode" &&
        args.value->at(1) == L"external-providers" && args.value->at(2) == L"--provider-module" &&
        args.value->at(3).ends_with((provider / "original.dll").wstring()) &&
        args.value->at(4) == L"--provider-native" && args.value->at(5) == L"0|AudastraFixtureAdd|(I)I",
        "mode, module and native arguments match the default CTest fixture");
    FailureCode(Services::SelectExternalDependencyFiles(*status.value, {{"AudastraFixtureAdd", "(S)I"}}),
        "EXTERNAL_DEPENDENCY_NATIVE_SIGNATURE", "map signature mismatch stops selection");
    const auto missing = Services::SelectExternalDependencyFiles(*status.value, {{"MissingAlpha", ""}, {"MissingBeta", ""}});
    FailureCode(missing, "EXTERNAL_DEPENDENCY_NATIVE_MISSING", "missing names stop selection");
    Require(missing.diagnostics.front().message.find("MissingAlpha") != std::string::npos &&
        missing.diagnostics.front().message.find("MissingBeta") != std::string::npos,
        "all missing names appear in the diagnostic");

    auto map = fixture.Artifact({{"war3map.j", Bytes(Script("call AudastraFixtureAdd(1)",
        "native AudastraFixtureAdd takes integer i returns integer\n"))}, {"scripts/original.dll", Pe(true, true)},
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}});
    map.externalDependenciesDirectory = directory;
    fixture.Runtime(map);
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "EXTERNAL_DEPENDENCY_GAME",
        "full coverage chooses provider path with the genuine game identity gate");
    auto partial = fixture.Artifact(Script("call AudastraFixtureAdd(1)",
        "native AudastraFixtureAdd takes integer i returns integer\nnative MissingOther takes nothing returns nothing\n"));
    partial.externalDependenciesDirectory = directory;
    fixture.Runtime(partial);
    const auto partialFallback = Script::BuildIndependentMapTestLaunchPlan(partial);
    Success(partialFallback, "partial coverage cannot suppress the existing explicit runtime");
    Require(partialFallback.value->runtimeRoot == fixture.runtime, "partial provider not loaded");
    partial.runtimeRoot.clear();
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(partial), "EXTERNAL_DEPENDENCY_NATIVE_MISSING",
        "partial coverage with no fallback reports the missing native");
    fixture.Unchanged(map);

    Write(provider / "provider.json", Bytes(R"json({"schema":1,"schema":1,"id":"original",
        "abi":"audastra-native-provider-v1","modules":[
        {"file":"original.dll","entry":"Initialize","natives":{"AudastraFixtureAdd":"(I)I"}}]})json"));
    status = Services::QueryExternalDependencies(directory);
    Success(status, "malformed provider is visible in status");
    Require(status.value->providers.size() == 1 && !status.value->providers.front().usable,
        "duplicate manifest keys are rejected");
    auto fallback = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(fallback, "broken descriptor leaves explicit old path intact");
    Require(fallback.value->runtimeRoot == fixture.runtime, "old runtime selected");
    const auto containsCode = [](const auto& diagnostics, std::string_view code) {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
    };
    Require(containsCode(fallback.diagnostics, "EXTERNAL_DEPENDENCY_MAP_DLL"), "embedded DLL is named in a nonblocking warning");
    map.externalDependenciesDirectory = L"/relative-root";
    fallback = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(fallback, "invalid directory is only a warning with the old path available");
    Require(containsCode(fallback.diagnostics, "EXTERNAL_DEPENDENCY_DIRECTORY_UNAVAILABLE"), "directory failure carried to launch");
    map.runtimeRoot.clear();
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "EXTERNAL_DEPENDENCY_DIRECTORY",
        "required map with no fallback reports why the external directory is unavailable");

    for (unsigned i = 0; i < 520; ++i) Write(directory / ("notes-" + std::to_string(i)), Bytes("unrelated"));
    Write(directory / "unrelated" / "nested.dll", Pe(true));
    auto known = map;
    fixture.Runtime(known, Services::MapTestRuntimeLayout::YdweTree);
    const auto builtin = directory / "legacy";
    for (const auto* relative : {"bin/ydbase.dll", "bin/lua53.dll", "bin/ydwar3.dll",
            "plugin/warcraft3/yd_loader.dll", "plugin/warcraft3/yd_jass_api.dll", "plugin/warcraft3/yd_lua_engine.dll"}) {
        auto bytes = Pe(true, std::string_view(relative).find("api.dll") != std::string_view::npos ||
            std::string_view(relative).find("engine.dll") != std::string_view::npos);
        const auto evidence = Bytes(std::string_view("\0EXFixtureTest\0", 15));
        bytes.insert(bytes.end(), evidence.begin(), evidence.end());
        Write(builtin / relative, bytes);
    }
    Write(directory / "kkwe" / L"插件" / "version.dll", Pe(true));
    Write(directory / "kkwe" / L"插件" / "dz_w3_plugin.dll", Pe(true));
    status = Services::QueryExternalDependencies(directory);
    Success(status, "unrelated files and directories do not exhaust provider budget");
    // 重复键已使原来的描述整体失效，因此原 DLL 与根目录未声明 DLL 都应报告。
    Require(status.value->providers.size() == 3 && status.value->undescribedDllCount == 2 &&
        std::find(status.value->unrecognizedDirectories.begin(), status.value->unrecognizedDirectories.end(),
            directory / "unrelated") != status.value->unrecognizedDirectories.end(), "known layouts and unscanned directories have separate status");
    const auto ydwe = std::find_if(status.value->providers.begin(), status.value->providers.end(),
        [](const auto& item) { return item.kind == Services::ExternalDependencyKind::Ydwe; });
    Require(ydwe != status.value->providers.end() && ydwe->usable && ydwe->ydweTree && ydwe->files.size() == 6 &&
        ydwe->files[4].candidateNatives == std::vector<std::string>{"EXFixtureTest"}, "recognize PE-verified YDWE and candidate strings");
    const auto treeStatus = Services::QueryExternalDependencies(builtin / L"");
    Success(treeStatus, "a YDWE installation can be selected directly");
    Require(treeStatus.value->providers.size() == 1 && treeStatus.value->unrecognizedDirectories.empty(),
        "bin and plugin belong to the single YDWE installation and are not rescanned");
    const auto dz = std::find_if(status.value->providers.begin(), status.value->providers.end(),
        [](const auto& item) { return item.kind == Services::ExternalDependencyKind::KkweDz; });
    Require(dz != status.value->providers.end() && !dz->usable && dz->files.size() == 2, "Dz status does not authorize loading");

    Write(provider / "provider.json", Bytes(R"json({"schema":1,"id":"original","abi":"audastra-native-provider-v1","modules":[
        {"file":"original.dll","entry":"Initialize","natives":{"AudastraFixtureAdd":"(I)I"}}]})json"));
    auto legacy = fixture.Artifact(Script("call EXFixtureTest()", "native EXFixtureTest takes nothing returns nothing\n"));
    fixture.Runtime(legacy, Services::MapTestRuntimeLayout::YdweTree, true);
    legacy.externalDependenciesDirectory = directory;
    auto legacyPlan = Script::BuildIndependentMapTestLaunchPlan(legacy);
    Success(legacyPlan, "an unrelated declared provider does not prevent legacy JASS on synthetic Game.dll");
    Require(legacyPlan.value->runtimeRoot == builtin && containsCode(legacyPlan.diagnostics, "EXTERNAL_DEPENDENCY_YDWE_SELECTED") &&
        std::find(legacyPlan.value->arguments.begin(), legacyPlan.value->arguments.end(), L"--provider-module") == legacyPlan.value->arguments.end(),
        "folder layout beats the derived UI location and uses the old runtime arguments");
    fixture.Runtime(legacy, Services::MapTestRuntimeLayout::YdweTree);
    legacyPlan = Script::BuildIndependentMapTestLaunchPlan(legacy);
    Success(legacyPlan, "explicit runtime takes precedence over the matching folder layout");
    Require(legacyPlan.value->runtimeRoot == fixture.runtime, "explicit runtime selected");
}

void CliAndHLuaDependencyPreferences() {
    Fixture fixture;
    struct RegistryScope {
        HKEY root = nullptr;
        bool overrideActive = false;
        std::wstring name;
        ~RegistryScope() {
            if (overrideActive) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (root) {
                RegCloseKey(root);
                RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str());
            }
        }
    } registry;
    registry.name = L"Software\\Audastra-Test-External-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64());
    DWORD disposition = 0;
    const auto opened = RegCreateKeyExW(HKEY_CURRENT_USER, registry.name.c_str(), 0, nullptr,
        REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &registry.root, &disposition);
    if (disposition != REG_CREATED_NEW_KEY && registry.root) {
        RegCloseKey(registry.root);
        registry.root = nullptr;
    }
    Require(opened == ERROR_SUCCESS && registry.root, "create exclusive transient registry fixture");
    Require(RegOverridePredefKey(HKEY_CURRENT_USER, registry.root) == ERROR_SUCCESS, "isolate native registry in this process");
    registry.overrideActive = true;
    const auto configured = fixture.root / "configured-dependencies";
    const auto value = L"  " + configured.wstring() + L"  ";
    struct SettingsKey { HKEY value = nullptr; ~SettingsKey() { if (value) RegCloseKey(value); } } settings;
    Require(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\CallDisaster\\Audastra\\mapTest", 0, nullptr,
        REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &settings.value, nullptr) == ERROR_SUCCESS,
        "create actual editor settings path in the isolated hive");
    Require(RegSetValueExW(settings.value, L"externalDependenciesDirectory", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS, "seed the isolated settings value");
    Require(Services::ResolveCliExternalDependenciesDirectory() == configured,
        "CLI map test/status and h-lua share the editor's native registry setting");
    const auto explicitDirectory = fixture.root / "explicit-dependencies";
    Require(Services::ResolveCliExternalDependenciesDirectory(explicitDirectory) == explicitDirectory,
        "an explicit CLI or h-lua path overrides editor settings");
    Require(RegDeleteValueW(settings.value, L"externalDependenciesDirectory") == ERROR_SUCCESS,
        "clear only the process-isolated test value");
    Require(Services::ResolveCliExternalDependenciesDirectory() ==
        Services::ExternalDependenciesDirectory(CurrentInstallationRoot(), {}), "absent setting uses installation default");
}

void CheckOriginalProviderPlan(const std::filesystem::path& originalGame) {
    Fixture fixture;
    std::filesystem::copy_file(originalGame / L"Game.dll", fixture.game / L"Game.dll",
        std::filesystem::copy_options::overwrite_existing);
    const auto dependency = fixture.root / "deps";
    Write(dependency / "original.dll", Pe(true, true));
    Write(dependency / "provider.json", Bytes(R"json({"schema":1,"id":"original","abi":"audastra-native-provider-v1","modules":[
        {"file":"original.dll","entry":"Initialize","natives":{"AudastraFixtureAdd":"(I)I"}}]})json"));
    auto artifact = fixture.Artifact(Script("call AudastraFixtureAdd(1)",
        "native AudastraFixtureAdd takes integer i returns integer\n"));
    fixture.Runtime(artifact);
    artifact.externalDependenciesDirectory = dependency;
    const auto launch = Script::BuildIndependentMapTestLaunchPlan(artifact);
    Success(launch, "provider overrides the old setting with a verified original Game.dll identity");
    const auto& arguments = launch.value->arguments;
    const auto argument = [&](std::wstring_view option) {
        const auto it = std::find(arguments.begin(), arguments.end(), option);
        Require(it != arguments.end() && std::next(it) != arguments.end(), "expected launch argument");
        return *std::next(it);
    };
    Require(argument(L"--runtime-mode") == L"external-providers" &&
        argument(L"--provider-native") == L"0|AudastraFixtureAdd|(I)I" &&
        argument(L"--provider-module").ends_with(L"|Initialize|original|" + (dependency / "original.dll").wstring()),
        "plan carries provider mode, ID, module, and exact native signature");
    Require(launch.value->runtimeRoot.empty(), "new provider does not mix old DLLs");
    fixture.Unchanged(artifact);
}

void OwnedLuaSourcesAndComponents() {
    Fixture fixture;
    const auto members = [](std::string_view lua = "return true\n", std::string jass = Script("")) {
        std::vector<MapArchiveMember> result{{"war3map.j", Bytes(jass)}};
        for (std::size_t index = 1; index + 1 < Services::kClassicLuaGeneratedMembers.size(); ++index)
            result.push_back({std::string(Services::kClassicLuaGeneratedMembers[index]), Bytes(lua)});
        std::string manifest = "audastra-classic-lua-v1\n";
        for (const auto& item : result) manifest += item.name + "\t" + *Sha256(item.data).value + "\n";
        result.push_back({std::string(Services::kClassicLuaGeneratedMembers.back()), Bytes(manifest)});
        result.push_back({"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes});
        return result;
    };
    const auto artifact = [&](const std::vector<MapArchiveMember>& contents) {
        auto map = fixture.Artifact(contents);
        map.useOwnedLuaRuntime = true;
        map.runtimeRoot = "unused-external-root"; map.runtimeYdweRoot = "unused-ydwe-root";
        map.runtimeBridge = fixture.helper / "launcher.exe";
        map.runtimeBootstrap = fixture.helper / "bootstrap.dll";
        Write(map.runtimeBridge, Pe(false)); Write(map.runtimeBootstrap, Pe(true));
        return map;
    };
    auto map = artifact(members());
    auto plan = Script::BuildIndependentMapTestLaunchPlan(map); Success(plan, "complete classic generated source contract");
    Require(plan.value->useOwnedLuaRuntime && !plan.value->enableLuaHotReload && plan.value->runtimeRoot.empty(),
        "owned preflight needs no external DLLs even with obsolete configured paths");
    Require(plan.value->runtimeBootstrap == map.runtimeBootstrap
        && plan.value->runtimeBootstrapSha256 == *Sha256File(map.runtimeBootstrap).value, "bootstrap identity frozen in launch plan");
    map.enableLuaHotReload = true;
    Success(Script::BuildIndependentMapTestLaunchPlan(map), "explicit reload survives owned preflight");
    auto changed = members(); changed[2].data = Bytes("return false\n");
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(changed)), "CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "changed module cannot reuse manifest");
    changed = members(); changed[6].data.push_back(std::byte{'\n'});
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(changed)), "CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "manifest must match exact bytes");
    changed = members(); changed.erase(changed.begin() + 3);
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(changed)), "CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "missing generated module refused");
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact({{"war3map.j", Bytes(Script(""))}})),
        "CLASSIC_LUA_MANIFEST_REQUIRED", "JASS-only source cannot silently choose owned Lua");
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(members("local broken = \n"))),
        "MAP_TEST_OWN_LUA_SYNTAX", "valid source manifest does not bypass Lua compiler");
    const auto maximumModule = "--" + std::string(4u * 1024u * 1024u - 2, ' ');
    Success(Script::BuildIndependentMapTestLaunchPlan(artifact(members(maximumModule))),
        "five valid four-MiB modules obey runtime limit and are not squeezed into JASS sixteen-MiB budget");
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(members(maximumModule + " "))),
        "MAP_TEST_SCRIPT_BUDGET", "module over actual four-MiB runtime reader limit refused before launch");
    changed = members(); changed.back().data[0] = std::byte{31};
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(artifact(changed)), "MAP_TEST_OWN_LUA_CLASSIC_REQUIRED", "newer or malformed settings cannot select classic runtime");
    auto generatedWithExtension = artifact(members("return true\n",
        Script("    call ExternalFixtureNative()", "native ExternalFixtureNative takes nothing returns nothing\n")));
    generatedWithExtension.externalDependenciesDirectory = fixture.root / "original-provider";
    Write(generatedWithExtension.externalDependenciesDirectory / "fixture.dll", Pe(true, true));
    Write(generatedWithExtension.externalDependenciesDirectory / "provider.json", Bytes(R"json(
        {"schema":1,"id":"original","abi":"audastra-native-provider-v1","modules":[
        {"file":"fixture.dll","entry":"Initialize","natives":{"ExternalFixtureNative":"()V"}}]})json"));
    const auto compatibleProvider = Services::QueryExternalDependencies(generatedWithExtension.externalDependenciesDirectory);
    Success(compatibleProvider, "inspect original fixture provider for the classic binding negative");
    Success(Services::SelectExternalDependencyFiles(*compatibleProvider.value, {{"ExternalFixtureNative", "()V"}}),
        "provider completely covers the classic map's declaration");
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(generatedWithExtension),
        "MAP_TEST_OWN_LUA_EXTENSION_UNSUPPORTED", "classic globals refuse external natives even with a matching provider");
    Write(map.runtimeBootstrap, Pe(true, false, 0x8664));
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_PE", "owned bootstrap must be x86");
    fixture.Unchanged(map);
}

void NativeMapsNeedNoToolsOrRuntime() {
    Fixture fixture;
    auto map = fixture.Artifact(Script("    call BJDebugMsg(\"ordinary map\")\n    call DebugMessage(\"direct common native\")"));
    const HANDLE writableBefore = CreateFileW(map.testMap.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(writableBefore != INVALID_HANDLE_VALUE, "owned frozen map is writable before runtime takes its lease");
    CloseHandle(writableBefore);
    const auto renamedProbe = map.workspace / "lease-probe-renamed.w3x";
    Require(!std::filesystem::exists(renamedProbe), "lease rename probe has a fresh owned destination");
    std::vector<std::string> stages;
    unsigned leaseProbes = 0;
    bool writeBlocked = false, renameBlocked = false;
    const auto result = Script::BuildIndependentMapTestLaunchPlan(map, [&](std::string_view stage) {
        stages.emplace_back(stage);
        if (stage != "read-scripts") return;
        ++leaseProbes;
        const HANDLE write = CreateFileW(map.testMap.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD writeError = write == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        writeBlocked = write == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION;
        if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
        const BOOL moved = MoveFileExW(map.testMap.c_str(), renamedProbe.c_str(), 0);
        const DWORD moveError = moved ? ERROR_SUCCESS : GetLastError();
        renameBlocked = !moved && (moveError == ERROR_SHARING_VIOLATION || moveError == ERROR_ACCESS_DENIED);
        if (moved) Require(MoveFileExW(renamedProbe.c_str(), map.testMap.c_str(), 0) != 0,
            "restore only own map if a broken lease unexpectedly allows rename");
    });
    Success(result, "native map uses selected game directly while runtime holds its readonly lease");
    Require(stages == std::vector<std::string>{"verify", "read-scripts", "check-script", "check-runtime"}
        && leaseProbes == 1 && writeBlocked && renameBlocked,
        "streamed runtime analysis reports stages and holds map against writing and renaming while reading scripts");
    const auto afterLease = Sha256File(map.testMap); Success(afterLease, "recheck frozen map after runtime lease ends");
    Require(*afterLease.value == map.testMapSha256 && !std::filesystem::exists(renamedProbe),
        "continuous readonly lease preserves the exact frozen map SHA and location");
    // Core 返回规范路径，临时目录可能使用 Windows 8.3 别名；比较文件身份并保留完整启动形状验证。
    Require(std::filesystem::equivalent(result.value->executable, fixture.game / "war3.exe")
        && std::filesystem::equivalent(result.value->workingDirectory, fixture.game)
        && result.value->runtimeRoot.empty() && result.value->runtimeReceipt.empty()
        && result.value->arguments.size() == 3 && result.value->arguments[0] == L"-window"
        && result.value->arguments[1] == L"-loadfile"
        && result.value->arguments[2] == result.value->gameMapCopy.lexically_relative(result.value->workingDirectory).wstring(),
        "native launch plan has no YDWE, runtime or registry");
    Require(!result.value->gameExecutableSha256.empty() && !result.value->gameModuleSha256.empty(), "selected game identity remains bound");
    fixture.Unchanged(map);
    map = fixture.Artifact(Script("    call DebugMessage(\"same native declared by map\")", "native DebugMessage takes string message returns nothing\n"));
    Success(Script::BuildIndependentMapTestLaunchPlan(map), "native already provided by game common does not imply JAPI");
}
void LexicalTextAndLocalPrefixesAreNotExtensions() {
    Fixture fixture;
    const auto map = fixture.Artifact(Script("    // call EXMissing()\n    // native EXNotDeclared takes nothing returns nothing\n"
        "    call BJDebugMsg(\"EXMissing() / native FakeNative takes nothing returns nothing\")\n"
        "    call EXLocal()\n    call ScheduleCallback(function EXLocal)",
        "function EXLocal takes nothing returns nothing\n    call DebugMessage(\"local EX prefix\")\nendfunction\n"));
    Success(Script::BuildIndependentMapTestLaunchPlan(map), "comments, strings and real local functions are not guessed as JAPI");
    fixture.Unchanged(map);
}
void CompiledJassOpaqueBytesPreserveRuntimeClassification() {
    Fixture fixture;
    // 同一最终脚本同时含 UTF-8 和 GBK 字节；它们仅在注释/字符串中，不参与代码名称绑定。
    const std::string gbk = "\xBB\xB7\xC8\xC6\xBC\xBC\xC4\xDC";
    const std::string body = std::string("    // UTF-8 中 EXFakeUtf8Comment()\n    // ") + gbk
        + " EXFakeGbkComment()\n    call BJDebugMsg(\"UTF-8 中 EXFakeUtf8String()\")\n    call BJDebugMsg(\""
        + gbk + " EXFakeGbkString() / native EXFakeDeclaration takes nothing returns nothing\")\n";
    auto map = fixture.Artifact(Script(body));
    const auto native = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(native, "mixed compiled UTF-8/GBK opaque bytes do not require one whole-file source encoding");
    Require(std::filesystem::equivalent(native.value->executable, fixture.game / "war3.exe")
        && native.value->runtimeRoot.empty() && native.value->runtimeReceipt.empty()
        && native.value->arguments.size() == 3 && native.value->arguments[1] == L"-loadfile",
        "fake interfaces inside mixed-byte comments and strings remain a direct native plan");
    fixture.Unchanged(map);
    map = fixture.Artifact(Script(body + "    call EXActual()\n", "native EXActual takes nothing returns nothing\n"));
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_REQUIRED",
        "real extension call after mixed opaque bytes still requires independent runtime");
    fixture.Unchanged(map);
}
void CompiledJassByteRawcodesAndMalformedBoundaries() {
    Fixture fixture;
    const std::string high(1, static_cast<char>(0xff));
    const std::string four = high + std::string{static_cast<char>(0xfe), static_cast<char>(0x80), static_cast<char>(0x81)};
    for (const auto& raw : {high, four}) {
        const auto map = fixture.Artifact(Script("    local integer value = '" + raw + "'\n    call DebugMessage(\"rawcode fixture\")"));
        const auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
        Success(plan, "one/four byte rawcodes are integer byte literals, not Unicode text");
        Require(std::filesystem::equivalent(plan.value->executable, fixture.game / "war3.exe") && plan.value->runtimeRoot.empty(),
            "legal high-byte rawcodes leave a native game plan");
        fixture.Unchanged(map);
    }
    for (const auto& raw : {high + "AB", high + "ABCD"}) {
        const auto map = fixture.Artifact(Script("    local integer value = '" + raw + "'"));
        FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_MALFORMED",
            "three/five byte rawcode payloads remain malformed compiled JASS");
        fixture.Unchanged(map);
    }
    const auto truncated = fixture.Artifact(Script("    local integer value = '" + high + "\n"));
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(truncated), "MAP_TEST_SCRIPT_MALFORMED",
        "high-byte rawcode without closing quote cannot launch");
    fixture.Unchanged(truncated);
    for (const auto& body : {std::string("    ") + high + "\n", std::string("    call EX") + high + "()\n"}) {
        const auto invalidCode = fixture.Artifact(Script(body));
        FailureCode(Script::BuildIndependentMapTestLaunchPlan(invalidCode), "MAP_TEST_SCRIPT_MALFORMED",
            "opaque literal byte compatibility never accepts high bytes in actual code names");
        fixture.Unchanged(invalidCode);
    }
    for (const char control : {char{0}, char{0x1a}}) {
        for (const auto& body : {std::string("    // control ") + control + "\n",
                 std::string("    call DebugMessage(\"control ") + control + "\")\n"}) {
            const auto invalidControl = fixture.Artifact(Script(body));
            FailureCode(Script::BuildIndependentMapTestLaunchPlan(invalidControl), "MAP_TEST_SCRIPT_MALFORMED",
                "NUL and Ctrl-Z remain rejected even inside opaque comments and strings");
            fixture.Unchanged(invalidControl);
        }
    }
}
void ExplicitAndUnknownInterfacesNeedRuntime() {
    Fixture fixture;
    for (const auto& script : {Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"),
             Script("    call EXUnknown()"), Script("    local integer value = MissingInteger()"),
             Script("    call ScheduleCallback(function MissingCallback)")}) {
        const auto map = fixture.Artifact(script);
        const auto result = Script::BuildIndependentMapTestLaunchPlan(map);
        Require(Code(result, "MAP_TEST_RUNTIME_REQUIRED"), "undeclared game interface requires explicit independent runtime");
        Require(result.diagnostics.front().message.find("无需启动 YDWE") != std::string::npos, "runtime message explains independent process launch and local library configuration");
        fixture.Unchanged(map);
    }
}
void RuntimeFilesAndProfileAreCheckedWithoutExecution() {
    Fixture fixture;
    auto map = fixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"));
    fixture.Runtime(map);
    auto plan = Script::BuildIndependentMapTestLaunchPlan(map); Success(plan, "synthetic independent x86 Lua53 profile passes static preflight");
    Require(plan.value->executable == map.runtimeBridge && plan.value->runtimeRoot == fixture.runtime
        && plan.value->arguments[0] == L"--game", "runtime plan uses Audastra independent bridge");
    Write(fixture.runtime / "yd_jass_api.dll", Pe(true));
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_EXPORT"), "JASS plugin must export executable Initialize");
    Write(fixture.runtime / "yd_jass_api.dll", Pe(true, true));
    Write(fixture.runtime / "yd_lua_engine.dll", Pe(true, true, 0x014c, true));
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_PROFILE"), "Lua54 import cannot pass declared Lua53 profile");
    Write(fixture.runtime / "yd_lua_engine.dll", Pe(true, true));
    Write(fixture.runtime / "yd_loader.dll", Pe(true, false, 0x8664));
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_PE"), "x64 plugin cannot pass x86 target preflight");
    Write(fixture.runtime / "yd_loader.dll", Bytes("damaged PE"));
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_PE"), "malformed runtime image is rejected statically");
    Require(std::filesystem::remove(fixture.runtime / "yd_loader.dll"), "remove only owned fixture DLL");
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_RUNTIME_MISSING"), "missing dependency cannot silently direct-launch extension map");
    fixture.Unchanged(map);
}
void YdweTreeFallbackIsReadonlyAndOnlyNeededForExtensions() {
    Fixture fixture;
    auto map = fixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"));
    fixture.Runtime(map, Services::MapTestRuntimeLayout::YdweTree, true);
    std::vector<std::pair<std::filesystem::path, std::string>> identities;
    for (const auto* relative : {"bin/ydbase.dll", "bin/lua53.dll", "bin/ydwar3.dll", "plugin/warcraft3/yd_loader.dll",
            "plugin/warcraft3/yd_jass_api.dll", "plugin/warcraft3/yd_lua_engine.dll"}) {
        const auto path = fixture.runtime / relative;
        identities.emplace_back(path, *Sha256File(path).value);
    }
    const auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "empty independent root discovers all six DLLs in the readonly configured YDWE tree");
    Require(map.runtimeRoot.empty() && plan.value->runtimeRoot == fixture.runtime
        && plan.value->runtimeLayout == Services::MapTestRuntimeLayout::YdweTree
        && plan.value->arguments.size() == 12 && plan.value->arguments[10] == L"--runtime-layout"
        && plan.value->arguments[11] == L"ydwe-tree", "discovered layout is explicit in the frozen launcher plan without changing user preferences");
    for (const auto& [path, sha] : identities) Require(*Sha256File(path).value == sha, "all local runtime DLL bytes remain unchanged");
    for (const auto& uiRoot : {fixture.runtime / "ui", fixture.runtime / "share/mpq"}) {
        map.runtimeYdweRoot = uiRoot;
        const auto subdirectoryPlan = Script::BuildIndependentMapTestLaunchPlan(map);
        Success(subdirectoryPlan, "existing UI settings may point to the fixed ui or share/mpq subdirectory");
        Require(subdirectoryPlan.value->runtimeRoot == fixture.runtime, "only known UI subdirectory shapes normalize to the installation root");
    }
    map.runtimeYdweRoot = fixture.runtime;
    Require(!std::filesystem::exists(fixture.runtime / "ydbase.dll") && !std::filesystem::exists(map.workspace / "bin"),
        "runtime discovery never flattens, copies, or stages the user-installed GPL libraries");
    // 使用实际搜索的两步路径拼接，Windows 保留的原分隔符形态也必须一致。
    const auto missingPath = fixture.runtime / "plugin/warcraft3" / "yd_loader.dll";
    Require(std::filesystem::remove(missingPath), "remove only original owned synthetic PE fixture");
    const auto missing = Script::BuildIndependentMapTestLaunchPlan(map);
    if (!(Code(missing, "MAP_TEST_RUNTIME_MISSING") && missing.diagnostics.front().message.find("yd_loader.dll") != std::string::npos
            && missing.diagnostics.front().message.find(IO::PathUtf8(missingPath)) != std::string::npos)) {
        std::string details = "missing two-level DLL diagnostic gives its exact filename and full searched path\nexpected path: "
            + IO::PathUtf8(missingPath);
        for (const auto& diagnostic : missing.diagnostics)
            details += "\nactual " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(details);
    }
    auto native = fixture.Artifact(Script("    call DebugMessage(\"native\")"));
    native.runtimeYdweRoot = fixture.root / "missing-unused-local-ydwe";
    const auto direct = Script::BuildIndependentMapTestLaunchPlan(native);
    Success(direct, "native map ignores unavailable configured YDWE libraries");
    Require(direct.value->runtimeRoot.empty() && direct.value->arguments.size() == 3,
        "ordinary maps keep direct native launch and never acquire an automatic runtime dependency");
    fixture.Unchanged(map); fixture.Unchanged(native);

    Fixture flatFixture;
    auto explicitMap = flatFixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"));
    flatFixture.Runtime(explicitMap);
    explicitMap.runtimeYdweRoot = flatFixture.root / "missing-ignored-ydwe";
    const auto explicitPlan = Script::BuildIndependentMapTestLaunchPlan(explicitMap);
    Success(explicitPlan, "explicit independent flat runtime wins over configured local YDWE fallback");
    Require(explicitPlan.value->runtimeLayout == Services::MapTestRuntimeLayout::Flat && explicitPlan.value->arguments.size() == 10,
        "existing flat runtime launcher arguments remain compatible");
    explicitMap.runtimeLayout = static_cast<Services::MapTestRuntimeLayout>(99);
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(explicitMap), "MAP_TEST_RUNTIME_LAYOUT"), "invalid layout cannot reach launcher");
    flatFixture.Unchanged(explicitMap);
}
void UnusedUiPreferencesCannotBlockNativeMaps() {
    Fixture fixture;
    const auto nativeScript = Script("    call DebugMessage(\"native\")");
    for (const auto& configured : {std::filesystem::path("relative/legacy-ui"), fixture.root / "missing-ui/share/mpq"}) {
        const auto map = fixture.Artifact(nativeScript, configured);
        const auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
        Success(plan, "prepare and runtime planning ignore unused legacy relative or missing UI settings for a native map");
        Require(plan.value->runtimeRoot.empty() && plan.value->arguments.size() == 3, "unused UI preference never activates a runtime");
        fixture.Unchanged(map);
    }
    const auto extension = fixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"), "relative/legacy-ui");
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(extension), "MAP_TEST_RUNTIME_PATH"),
        "a relative fallback is still rejected once an extension map actually needs it");
    const auto target = fixture.root / "owned-local-ydwe";
    const auto link = fixture.root / "owned-local-ydwe-link";
    Require(std::filesystem::create_directory(target), "create original owned directory-symlink target");
    BOOL linked = CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2);
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER) linked = CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY);
    if (linked) {
        const auto map = fixture.Artifact(nativeScript, link);
        Success(Script::BuildIndependentMapTestLaunchPlan(map), "unused reparse UI preference cannot block an ordinary native map");
        fixture.Unchanged(map);
        const auto required = fixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"), link);
        Require(Code(Script::BuildIndependentMapTestLaunchPlan(required), "MAP_TEST_RUNTIME_PATH"), "reparse runtime is rejected when actually selected for loading");
        Require(std::filesystem::remove(link), "remove only original owned directory symlink");
    } else std::cout << "Optional owned reparse UI preference fixture NOT EXECUTED; CreateSymbolicLink Win32=" << GetLastError() << '\n';
    fixture.Unchanged(extension);
}
void LuaMapsAreExplicitRuntimeInputs() {
    Fixture fixture;
    auto map = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.lua", Bytes("function config() end\nfunction main() end\n")}});
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_LUA_CLIENT_UNSUPPORTED"), "classic game explicitly refuses native Lua format");
    fixture.Runtime(map);
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_LUA_CLIENT_UNSUPPORTED"),
        "six valid independent runtime DLLs cannot turn modern native Lua format into classic compatibility");
    const auto nul = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.lua", {std::byte{0}}}});
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(nul), "MAP_TEST_SCRIPT_MALFORMED"), "Lua NUL source rejected before plugin startup");
    fixture.Unchanged(map);
}
// 原创手写 Lua 地图：初始化里 Cheat("exec-lua: \"main\"")（YDWE 约定），Lua 文件作为普通成员导入。
void HandwrittenLuaEntriesSelectRuntime() {
    const auto common = std::string(kCommon) + "native Cheat takes string cheatStr returns nothing\n";
    Fixture fixture(common);
    const auto entry = Script("    call Cheat(\"exec-lua: \\\"main\\\"\")");
    const auto members = [](std::string jass, std::vector<MapArchiveMember> lua) {
        std::vector<MapArchiveMember> result{{"war3map.j", Bytes(jass)}, {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
        result.insert(result.end(), lua.begin(), lua.end());
        return result;
    };
    const auto helpers = [&](Services::MapTestArtifact& map) {
        Write(fixture.helper / "audastra-warcraft-launcher.exe", Pe(false)); Write(fixture.helper / "audastra-warcraft-bootstrap.dll", Pe(true));
        map.runtimeBridge = fixture.helper / "audastra-warcraft-launcher.exe";
        map.runtimeBootstrap = fixture.helper / "audastra-warcraft-bootstrap.dll";
    };
    const auto hasCode = [](const auto& result, std::string_view code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; });
    };
    auto map = fixture.Artifact(members(entry, {{"main.lua", Bytes("local u = require 'lib.util'\nlocal japi=require 'jass.japi'\nif japi.DzGetMouseX then local x=japi.DzGetMouseX() end\nlocal id = |hfoo|\n")},
        {"lib\\util.lua", Bytes("return {}\n")}}));
    helpers(map);
    map.externalDependenciesDirectory = fixture.root / "empty-dependencies";
    auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "handwritten exec-lua map selects the built-in Lua runtime");
    Require(plan.value->useOwnedLuaRuntime && plan.value->enableLuaHotReload && plan.value->runtimeRoot.empty()
        && plan.value->luaEntryModules == std::vector<std::string>{"main"} && hasCode(plan, "MAP_TEST_LUA_OWNED"),
        "exec-lua entry is detected, hot reload follows the preference, and no YDWE DLL is needed");
    fixture.Runtime(map, Services::MapTestRuntimeLayout::YdweTree, true);
    map.handwrittenLuaHotReload = false;
    plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "handwritten Lua without hot reload");
    Require(plan.value->useOwnedLuaRuntime && !plan.value->enableLuaHotReload, "hot reload preference respected");
    Require(hasCode(plan,"MAP_TEST_LUA_OPTIONAL_EXTENSIONS"),"owned extension limit is reported only for the selected engine");
    Write(map.externalDependenciesDirectory / "optional.dll", Pe(true, true));
    Write(map.externalDependenciesDirectory / "provider.json", Bytes(R"json({"schema":1,"id":"optional","abi":"audastra-native-provider-v1","modules":[
        {"file":"optional.dll","entry":"Initialize","natives":{"DzGetMouseX":"()I"}}]})json"));
    plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "matching optional name alone never activates the declared provider");
    Require(plan.value->useOwnedLuaRuntime && plan.value->runtimeRoot.empty() &&
        hasCode(plan, "MAP_TEST_LUA_OPTIONAL_EXTENSIONS") && !hasCode(plan, "EXTERNAL_DEPENDENCY_SELECTED"),
        "no provider is loaded and owned Lua stays selected");

    std::string many = "local japi = require 'jass.japi'\nlocal value\n";
    for (unsigned i = 0; i < 256; ++i) many += "value = japi.Test" + std::to_string(i) + "\n";
    many += "value = japi.Test255\nvalue = japi['']\n";
    auto exactBudget = fixture.Artifact(members(entry, {{"main.lua", Bytes(many)}}));
    helpers(exactBudget);
    Success(Script::BuildIndependentMapTestLaunchPlan(exactBudget), "duplicate and empty key do not exhaust the distinct owned budget");
    many += "value = japi.Test256\n";
    auto overBudget = fixture.Artifact(members(entry, {{"main.lua", Bytes(many)}}));
    fixture.Runtime(overBudget);
    const auto warning = Script::BuildIndependentMapTestLaunchPlan(overBudget);
    Success(warning, "distinct optional lookup budget does not block startup");
    Require(warning.value->useOwnedLuaRuntime && hasCode(warning, "MAP_TEST_LUA_OPTIONAL_SCAN_INCOMPLETE"),
        "optional budget gives a warning");
    auto longKey = fixture.Artifact(members(entry, {{"main.lua", Bytes("local japi = require 'jass.japi'\nreturn japi['"
        + std::string(129, 'x') + "']\n")}}));
    helpers(longKey);
    const auto longKeyPlan = Script::BuildIndependentMapTestLaunchPlan(longKey);
    Success(longKeyPlan, "oversize optional key does not block startup");
    Require(hasCode(longKeyPlan, "MAP_TEST_LUA_OPTIONAL_SCAN_INCOMPLETE"), "long optional key reports incomplete scan");
    overBudget.handwrittenLuaEngine = Services::MapTestLuaEngine::Ydwe;
    Success(Script::BuildIndependentMapTestLaunchPlan(overBudget), "the YDWE engine does not use the owned lookup budget");

    auto missing = fixture.Artifact(members(entry, {}));
    helpers(missing);
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(missing), "MAP_TEST_LUA_ENTRY_MISSING", "missing entry file stops before launch");
    auto broken = fixture.Artifact(members(entry, {{"main.lua", Bytes("return 1\n")}, {"lib\\bad.lua", Bytes("local x = 1\nx = \n")}}));
    helpers(broken);
    const auto syntax = Script::BuildIndependentMapTestLaunchPlan(broken);
    FailureCode(syntax, "MAP_TEST_LUA_SYNTAX", "syntax error in any map Lua file stops before launch");
    Require(syntax.diagnostics.back().message.find("lib\\bad.lua") != std::string::npos, "syntax error names the file");
    auto invalid = fixture.Artifact(members(Script("    call Cheat(\"exec-lua: main\")"), {{"main.lua", Bytes("return 1\n")}}));
    helpers(invalid);
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(invalid), "MAP_TEST_LUA_ENTRY_INVALID", "unquoted module is reported with its line");

    auto ordinary = fixture.Artifact(members(Script("    call Cheat(\"greedisgood 500\")"), {{"main.lua", Bytes("return (\n")}}));
    plan = Script::BuildIndependentMapTestLaunchPlan(ordinary);
    Success(plan, "ordinary cheat command does not load Lua");
    Require(!plan.value->useOwnedLuaRuntime && plan.value->luaEntryModules.empty() && plan.value->runtimeRoot.empty(),
        "maps without exec-lua keep direct native launch and their .lua members are not interpreted");

    map.handwrittenLuaEngine = Services::MapTestLuaEngine::Ydwe;
    map.runtimeYdweRoot.clear();
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_LUA_RUNTIME_REQUIRED",
        "YDWE engine preference needs a configured YDWE instead of silently launching the plain game");
    fixture.Runtime(map, Services::MapTestRuntimeLayout::YdweTree, true);
    plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "YDWE engine preference uses the readonly configured YDWE tree");
    Require(!plan.value->useOwnedLuaRuntime && plan.value->runtimeRoot == fixture.runtime, "YDWE engine selected");
    Require(hasCode(plan,"MAP_TEST_LUA_YDWE") && !hasCode(plan,"MAP_TEST_LUA_OPTIONAL_EXTENSIONS"),
        "external engine selection is explained without the unrelated owned capability warning");

    auto extension = fixture.Artifact(members(Script("    call EXNative()\n    call Cheat(\"exec-lua: \\\"main\\\"\")",
        "native EXNative takes nothing returns nothing\n"), {{"main.lua", Bytes("return 1\n")}}));
    fixture.Runtime(extension, Services::MapTestRuntimeLayout::YdweTree, true);
    plan = Script::BuildIndependentMapTestLaunchPlan(extension);
    Success(plan, "extension natives fall back to the YDWE Lua engine");
    Require(!plan.value->useOwnedLuaRuntime && plan.value->runtimeRoot == fixture.runtime && hasCode(plan, "MAP_TEST_LUA_YDWE_FALLBACK"),
        "fallback is explained instead of silently dropping hot reload");
    for (const auto* name : {"yd_jass_api.dll", "yd_lua_engine.dll"}) {
        auto dll = Pe(true, true);
        const auto evidence = Bytes(std::string_view("\0EXNative\0", 10));
        dll.insert(dll.end(), evidence.begin(), evidence.end());
        Write(fixture.runtime / "plugin/warcraft3" / name, dll);
    }
    extension.runtimeYdweRoot.clear();
    extension.externalDependenciesDirectory = fixture.runtime;
    plan = Script::BuildIndependentMapTestLaunchPlan(extension);
    Success(plan, "folder YDWE supports the required native via the legacy Lua engine");
    Require(!plan.value->useOwnedLuaRuntime && plan.value->runtimeRoot == fixture.runtime &&
        hasCode(plan, "MAP_TEST_LUA_YDWE_FALLBACK") && hasCode(plan, "EXTERNAL_DEPENDENCY_YDWE_SELECTED"),
        "folder YDWE never joins owned Lua");
    auto optionalEx = fixture.Artifact(members(entry, {{"main.lua", Bytes(
        "local japi = require 'jass.japi'\nassert(japi.EXNative == nil)\n")}}));
    helpers(optionalEx);
    optionalEx.externalDependenciesDirectory = fixture.runtime;
    plan = Script::BuildIndependentMapTestLaunchPlan(optionalEx);
    Success(plan, "a statically recognized EX name is still optional without a map declaration");
    Require(plan.value->useOwnedLuaRuntime && plan.value->runtimeRoot.empty() &&
        !hasCode(plan, "EXTERNAL_DEPENDENCY_YDWE_SELECTED"), "optional EX lookup never starts the legacy chain");
    auto gbk = fixture.Artifact(members(entry, {{"main.lua", Bytes("-- \xB2\xE2\xCA\xD4\nreturn 1\n")}}));
    fixture.Runtime(gbk, Services::MapTestRuntimeLayout::YdweTree, true);
    plan = Script::BuildIndependentMapTestLaunchPlan(gbk);
    Success(plan, "non-UTF-8 Lua falls back to YDWE engine");
    Require(!plan.value->useOwnedLuaRuntime && hasCode(plan, "MAP_TEST_LUA_YDWE_FALLBACK"), "non-UTF-8 fallback explained");
    fixture.Unchanged(map); fixture.Unchanged(ordinary);
}
void MissingMalformedAndOversizeScriptsStopPlanning() {
    Fixture fixture;
    auto map = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}});
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_MISSING"), "missing runtime script is not native-success");
    map = fixture.Artifact("function main takes nothing returns nothing\n    call DebugMessage(\"unterminated)\nendfunction\n");
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_MALFORMED"), "truncated string has no unsafe lexer dereference");
    map = fixture.Artifact("function main takes nothing returns nothing\n    if true then\nendfunction\n");
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_MALFORMED"), "malformed function block cannot launch");
    map = fixture.Artifact("function main takes nothing returns nothing\nendfunction\n");
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_ENTRYPOINT"), "missing config entry cannot launch");
    map = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.j", std::vector<std::byte>(16u * 1024u * 1024u + 1, std::byte{' '})}});
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_BUDGET"), "oversize map script rejected before lexer allocation");
    const auto settings = MapDataFixtures::FullMapSettingsV25().bytes;
    auto almostFull = Script("");
    almostFull.resize(16u * 1024u * 1024u - settings.size() - kCommon.size(), ' ');
    map = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.j", Bytes(almostFull)}, {"war3map.w3i", settings}});
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_BUDGET"),
        "common consumes last remaining bytes and blizzard is refused before another full read");
    fixture.Unchanged(map);
    Fixture brokenGame("native Broken takes\n", kBlizzard);
    map = brokenGame.Artifact(Script("    call BJDebugMsg(\"map\")"));
    Require(Code(Script::BuildIndependentMapTestLaunchPlan(map), "MAP_TEST_SCRIPT_MALFORMED"), "damaged selected-game common is not silently treated as missing natives");
}

void AnonymousScriptFlagsAreCheckedBeforeDecoding() {
    Fixture fixture;
    const auto artifact = [&](std::uint32_t flags, bool withListfile = true) {
        const auto name = std::to_string(++fixture.serial);
        const auto source = fixture.root / ("anonymous-source-" + name + ".w3x");
        Write(source, AnonymousScriptArchive(flags, withListfile));
        const auto sha = Sha256File(source); Success(sha, "bind own anonymous-script fixture bytes");
        Services::MapTestRequest request;
        request.inputMap = source; request.sourceMap = source; request.expectedInputSha256 = *sha.value;
        request.gameRoot = fixture.game; request.workspace = fixture.root / ("anonymous-prepared-" + name);
        const auto frozen = Services::PrepareMapTest(request); Success(frozen, "freeze own anonymous-script fixture");
        return *frozen.value;
    };
    constexpr auto ordinary = Mpq::kFileExists | Mpq::kFileSingleUnit;
    const auto control = artifact(ordinary);
    {
        const auto archive = Mpq::MpqArchive::OpenReadOnly(control.testMap); Success(archive, "open legal anonymous-script control");
        Require(archive.value->HasFile("war3map.j"), "anonymous script is reachable by its actual name hash");
        FailureCode(archive.value->ListFilesStrict(), "ANONYMOUS_BLOCK_REJECTED", "control script is omitted from valid listfile");
        Success(archive.value->ListFilesPreservingOpaque(), "anonymous control has valid global MPQ structure");
    }
    const auto native = Script::BuildIndependentMapTestLaunchPlan(control);
    Success(native, "legal anonymous JASS map still produces independent native plan");
    Require(std::filesystem::equivalent(native.value->executable, fixture.game / "war3.exe")
        && native.value->runtimeRoot.empty() && native.value->runtimeReceipt.empty()
        && native.value->arguments.size() == 3 && native.value->arguments[1] == L"-loadfile",
        "anonymous storage itself must not classify ordinary JASS as requiring plugins");
    fixture.Unchanged(control);
    for (const auto flags : {ordinary | 0x00000001u, ordinary | Mpq::kFileFixKey,
            ordinary | Mpq::kFileImplode | Mpq::kFileCompress}) {
        const auto map = artifact(flags);
        FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "FILE_FLAGS_UNSUPPORTED",
            "anonymous script rejects unknown or ambiguous storage flags before decode");
        fixture.Unchanged(map);
        const auto sha = Sha256File(map.testMap); Success(sha, "recheck rejected anonymous-script test-map bytes");
        Require(*sha.value == map.testMapSha256, "anonymous storage rejection preserves frozen map SHA");
    }
    for (const auto flags : {ordinary | Mpq::kFilePatch, ordinary | Mpq::kFileSectorCrc}) {
        const auto map = artifact(flags);
        FailureCode(Script::BuildIndependentMapTestLaunchPlan(map), "EXACT_MEMBER_ENCODING_UNSUPPORTED",
            "anonymous script refuses patch or sector CRC encodings before decode");
        fixture.Unchanged(map);
    }
    const auto missingListfile = artifact(ordinary | 0x00000001u, false);
    FailureCode(Script::BuildIndependentMapTestLaunchPlan(missingListfile), "LISTFILE_MISSING",
        "archive without any listfile is explicitly rejected by the prior structural gate");
    fixture.Unchanged(missingListfile);
}

std::filesystem::path OptionalEnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    Require(size <= 32768, "optional real-map environment path exceeds Windows path budget");
    std::wstring text(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, text.data(), size);
    Require(written && written < size, "optional real-map environment path changed during bounded read");
    text.resize(written);
    return std::filesystem::path(text);
}

void OptionalRealYdweRuntimePlan() {
    const auto ydwe = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_YDWE_PREFLIGHT_ROOT");
    if (ydwe.empty()) {
        std::cout << "Optional real local-YDWE readonly preflight NOT EXECUTED; configure FORGE_INDEPENDENT_YDWE_PREFLIGHT_ROOT\n";
        return;
    }
    const auto game = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_NATIVE_PLAN_GAME_ROOT");
    const auto helpers = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_YDWE_PREFLIGHT_HELPERS");
    Require(ydwe.is_absolute() && game.is_absolute() && helpers.is_absolute(), "real YDWE preflight requires explicit absolute local installation, game and helper paths");
    Fixture fixture;
    auto map = fixture.Artifact(Script("    call EXNative()", "native EXNative takes nothing returns nothing\n"));
    map.gameRoot = game;
    map.runtimeYdweRoot = ydwe;
    map.runtimeBridge = helpers / "audastra-warcraft-launcher.exe";
    map.runtimeBootstrap = helpers / "audastra-warcraft-bootstrap.dll";
    std::vector<std::pair<std::filesystem::path, std::string>> identities;
    for (const auto* relative : {"bin/ydbase.dll", "bin/lua53.dll", "bin/ydwar3.dll", "plugin/warcraft3/yd_loader.dll",
            "plugin/warcraft3/yd_jass_api.dll", "plugin/warcraft3/yd_lua_engine.dll"}) {
        const auto file = ydwe / relative;
        const auto sha = Sha256File(file); Success(sha, "readonly bind actual user-installed local DLL bytes");
        identities.emplace_back(file, *sha.value);
    }
    const auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(plan, "actual local YDWE six-module PE/Lua53/Initialize-export preflight passes without execution");
    Require(plan.value->runtimeLayout == Services::MapTestRuntimeLayout::YdweTree && plan.value->runtimeRoot == ydwe,
        "real static preflight preserves the actual local installation path");
    map.cloakedWindow = true; map.hiddenDesktop = true; map.backgroundPlay = false;
    const auto cloaked = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(cloaked, "cloaked launch plan independently requires verified background capability");
    Require(cloaked.value->cloakedWindow && cloaked.value->backgroundPlay && !cloaked.value->hiddenDesktop,
        "cloaked request alone installs both capabilities on the caller desktop");
    const auto& arguments = cloaked.value->arguments;
    Require(std::find(arguments.begin(), arguments.end(), L"--background-play") != arguments.end()
        && std::find(arguments.begin(), arguments.end(), L"--window-cloak") != arguments.end(),
        "actual launcher receives background and cloak flags from the public launch planner");
    Require(!std::filesystem::exists(plan.value->gameMapCopy.parent_path()) && !std::filesystem::exists(plan.value->runtimeReceipt),
        "actual local preflight creates no game task, runtime copies or launcher receipt");
    for (std::size_t index = 0; index < identities.size(); ++index) {
        const auto& [file, sha] = identities[index];
        Require(*Sha256File(file).value == sha, "actual installed library bytes remain identical after readonly service preflight");
        std::cout << "Local YDWE module preflight: path=" << IO::PathUtf8(file) << "; SHA256=" << sha
            << "; PE=x86 DLL; LuaABI=5.3-compatible; InitializeExport=" << (index >= 4 ? "valid" : "not-required") << '\n';
    }
    Require(*Sha256File(map.sourceMap).value == map.inputSha256, "local preflight preserves original tiny test map byte identity");
    std::cout << "Actual local-YDWE service preflight passed; six DLLs read in place; no LoadLibrary/Initialize execution, Warcraft/YDWE launch, copying or installation writes\n";
}

void OptionalCapturedCompiledScriptPlan() {
    const auto source = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_COMPILED_SCRIPT");
    if (source.empty()) {
        std::cout << "Optional captured compiled-script compatibility gate NOT EXECUTED; configure FORGE_INDEPENDENT_COMPILED_SCRIPT\n";
        return;
    }
    Require(source.is_absolute(), "captured compiled-script gate requires an explicit absolute readonly file");
    constexpr std::uint64_t maxScriptBytes = 16ull * 1024 * 1024;
    auto captured = IO::ReadFileBytes(source, maxScriptBytes); Success(captured, "bounded readonly captured final JASS bytes");
    Require(!captured.value->empty(), "captured compiled JASS is nonempty");
    const auto sha = Sha256(*captured.value); Success(sha, "bind captured final JASS byte identity");
    Fixture fixture;
    // 仅将已经捕获的小脚本放入本轮 tiny MPQ；可只读挂载所选游戏基础脚本，不打开或复制作者的大地图。
    auto map = fixture.Artifact(std::vector<MapArchiveMember>{{"war3map.j", std::move(*captured.value)}});
    captured.value.reset();
    const auto selectedGame = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_NATIVE_PLAN_GAME_ROOT");
    if (!selectedGame.empty()) {
        Require(selectedGame.is_absolute(), "captured script selected game directory must be explicitly absolute");
        map.gameRoot = selectedGame;
    }
    Success(ReadVerifiedFile(source, *sha.value, maxScriptBytes), "captured final JASS remains unchanged after tiny MPQ preparation");
    const auto corePlan = Services::BuildMapTestLaunchPlan(map); Success(corePlan, "plan dedicated task path without writing selected game");
    const auto plannedTask = corePlan.value->gameMapCopy.parent_path();
    Require(!std::filesystem::exists(plannedTask) && !std::filesystem::exists(corePlan.value->gameMapCopy),
        "captured-script plan starts with a fresh absent game task, allowing an existing Maps/Audastra root");
    const auto plan = Script::BuildIndependentMapTestLaunchPlan(map);
    Success(ReadVerifiedFile(source, *sha.value, maxScriptBytes), "captured final JASS remains unchanged after readonly runtime plan");
    FailureCode(plan, "MAP_TEST_RUNTIME_REQUIRED",
        "actual compiled JAPI script reaches extension classification instead of final-byte lexer rejection");
    Require(!std::filesystem::exists(plannedTask) && !std::filesystem::exists(corePlan.value->gameMapCopy),
        "captured-script classification creates no task directory or map copy in selected game");
    fixture.Unchanged(map);
    std::cout << "Captured compiled-script compatibility gate passed; script=" << IO::PathUtf8(source)
        << "; SHA256=" << *sha.value << "; selected game=" << IO::PathUtf8(map.gameRoot)
        << "; next gate=" << plan.diagnostics.back().code
        << "; reason=" << BoundedFirstDiagnostic(plan.diagnostics)
        << "; tiny MPQ only, no author map read/copy, game writes or application launch\n";
}

void OptionalRealExtensionMapPlan() {
    const auto source = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_EXTENSION_MAP");
    if (source.empty()) {
        std::cout << "Optional full real extension-map plan gate NOT EXECUTED; configure FORGE_INDEPENDENT_EXTENSION_MAP\n";
        return;
    }
    const auto game = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_NATIVE_PLAN_GAME_ROOT");
    Require(source.is_absolute() && !game.empty() && game.is_absolute(),
        "full real extension-map gate requires explicit absolute source and selected real game paths");
    constexpr std::uint64_t maxMapBytes = 512ull * 1024 * 1024;
    struct SourceLease {
        HANDLE value = INVALID_HANDLE_VALUE;
        explicit SourceLease(const std::filesystem::path& path) {
            value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(value != INVALID_HANDLE_VALUE, "hold real source map readonly against writes and deletion");
        }
        ~SourceLease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } sourceLease(source);
    LARGE_INTEGER size{};
    BY_HANDLE_FILE_INFORMATION information{};
    Require(GetFileType(sourceLease.value) == FILE_TYPE_DISK && GetFileInformationByHandle(sourceLease.value, &information)
        && !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && GetFileSizeEx(sourceLease.value, &size) && size.QuadPart > 0 && static_cast<std::uint64_t>(size.QuadPart) <= maxMapBytes,
        "full real extension-map source is an ordinary disk file within 512MiB stream budget");
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
    const auto initialHashStart = Clock::now();
    const auto sourceSha = Sha256Handle(sourceLease.value); Success(sourceSha, "stream-bind complete real source map SHA");
    const auto initialHashMs = milliseconds(Clock::now() - initialHashStart);
    double prepareMs = 0, runtimeMs = 0, extraCoreMs = 0, finalTestHashMs = 0;
    std::filesystem::path ownedRoot, plannedTask;
    std::string frozenSha, runtimeDiagnostic;
    {
        Fixture fixture;
        ownedRoot = fixture.root;
        Services::MapTestRequest request;
        request.inputMap = source; request.sourceMap = source; request.expectedInputSha256 = *sourceSha.value;
        request.gameRoot = game; request.workspace = fixture.root / "full-map-prepared"; request.maxMapBytes = maxMapBytes;
        const auto prepareStart = Clock::now();
        const auto frozen = Services::PrepareMapTest(request);
        prepareMs = milliseconds(Clock::now() - prepareStart);
        Success(frozen, "freeze one complete real extension map only into own temporary workspace");
        Require(frozen.value->testMapSha256 == *sourceSha.value && !frozen.value->optimizedSlk
            && std::filesystem::equivalent(frozen.value->workspace.parent_path(), fixture.root),
            "complete real map freeze is byte-identical and belongs only to this temporary fixture");
        const auto coreStart = Clock::now();
        const auto corePlan = Services::BuildMapTestLaunchPlan(*frozen.value);
        extraCoreMs = milliseconds(Clock::now() - coreStart);
        Success(corePlan, "readonly full-map core plan computes fresh game task without creating it");
        plannedTask = corePlan.value->gameMapCopy.parent_path();
        Require(!std::filesystem::exists(plannedTask) && !std::filesystem::exists(corePlan.value->gameMapCopy),
            "full real-map planned game task and copy are absent before preflight");
        const auto runtimeStart = Clock::now();
        const auto plan = Script::BuildIndependentMapTestLaunchPlan(*frozen.value);
        runtimeMs = milliseconds(Clock::now() - runtimeStart);
        FailureCode(plan, "MAP_TEST_RUNTIME_REQUIRED",
            "complete real JAPI map passes MPQ structure/member/byte-script preflight and reaches extension classification");
        runtimeDiagnostic = BoundedFirstDiagnostic(plan.diagnostics);
        Require(!std::filesystem::exists(plannedTask) && !std::filesystem::exists(corePlan.value->gameMapCopy),
            "full real-map readonly preflight creates no directory or map inside selected game");
        const auto testHashStart = Clock::now();
        const auto testSha = Sha256File(frozen.value->testMap);
        finalTestHashMs = milliseconds(Clock::now() - testHashStart);
        Success(testSha, "stream recheck complete temporary test map after runtime analysis");
        Require(*testSha.value == frozen.value->testMapSha256 && *testSha.value == *sourceSha.value,
            "full-map preflight leaves exact complete frozen map bytes unchanged");
        frozenSha = *testSha.value;
    }
    Require(!std::filesystem::exists(ownedRoot) && !std::filesystem::exists(plannedTask),
        "full-map fixture destructor reclaims only its own complete temporary map while game task stays absent");
    const auto finalSourceHashStart = Clock::now();
    const auto finalSourceSha = Sha256Handle(sourceLease.value);
    const auto finalSourceHashMs = milliseconds(Clock::now() - finalSourceHashStart);
    Success(finalSourceSha, "stream recheck original real map after temporary fixture cleanup");
    Require(*finalSourceSha.value == *sourceSha.value, "full-map gate preserves original author map SHA throughout readonly source lease");
    std::cout << "Real extension-map readonly preparation/preflight gate passed; source=" << IO::PathUtf8(source)
        << "; bytes=" << size.QuadPart << "; source SHA256=" << *sourceSha.value << "; frozen SHA256=" << frozenSha
        << "; selected game=" << IO::PathUtf8(game) << "; first runtime diagnostic=" << runtimeDiagnostic
        << "; prepare_ms=" << prepareMs << "; runtime_preflight_ms=" << runtimeMs
        << "; extra_core_plan_ms=" << extraCoreMs << "; initial_source_hash_ms=" << initialHashMs
        << "; final_source_hash_ms=" << finalSourceHashMs << "; final_testmap_hash_ms=" << finalTestHashMs
        << "; prepare/runtime timings exclude separately reported extra core/hash checks and fixture cleanup; no prior timing baseline or game-entry proof; no launch/DLL/GPU/registry/game/source writes\n";
}

void OptionalRealNativeMapPlan() {
    const auto source = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_NATIVE_PLAN_MAP");
    const auto game = OptionalEnvironmentPath(L"FORGE_INDEPENDENT_NATIVE_PLAN_GAME_ROOT");
    // game 是多个只读门禁共用的上下文；仅地图 opt-in 能启用本用例。
    if (source.empty()) {
        std::cout << "Optional real native-map plan gate NOT EXECUTED; configure FORGE_INDEPENDENT_NATIVE_PLAN_MAP and FORGE_INDEPENDENT_NATIVE_PLAN_GAME_ROOT\n";
        return;
    }
    Require(!source.empty() && !game.empty() && source.is_absolute() && game.is_absolute(),
        "optional real-map gate requires both explicit absolute map and game paths");
    constexpr std::uint64_t maxMapBytes = 128ull * 1024 * 1024;
    auto originalBytes = IO::ReadFileBytes(source, maxMapBytes); Success(originalBytes, "bounded readonly original map snapshot");
    const auto originalSha = Sha256(*originalBytes.value); Success(originalSha, "hash real original map snapshot");
    originalBytes.value.reset();
    const auto originalUnchanged = [&] {
        Success(ReadVerifiedFile(source, *originalSha.value, maxMapBytes), "real original map remains byte-identical and within read budget");
    };
    originalUnchanged();
    std::filesystem::path plannedTask;
    {
        // 只创建自己的临时根和 CorePrepare 副本；不创建或清理游戏安装内的任何目录。
        struct OwnedTemp {
            std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-real-native-plan-"
                + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            OwnedTemp() { Require(std::filesystem::create_directory(root), "create fresh owned real-plan temporary root"); }
            ~OwnedTemp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        } temp;
        Services::MapTestRequest request;
        request.inputMap = source; request.sourceMap = source; request.expectedInputSha256 = *originalSha.value;
        request.gameRoot = game; request.workspace = temp.root / "prepared"; request.maxMapBytes = maxMapBytes;
        const auto frozen = Services::PrepareMapTest(request);
        originalUnchanged();
        Success(frozen, "freeze real original map only into owned temporary workspace");
        Require(frozen.value->testMapSha256 == *originalSha.value && !frozen.value->optimizedSlk,
            "real plan consumes exact unoptimized original map byte snapshot");
        Require(std::filesystem::equivalent(frozen.value->workspace.parent_path(), temp.root),
            "real frozen map belongs to this gate's owned temporary root");
        const auto plan = Script::BuildIndependentMapTestLaunchPlan(*frozen.value);
        originalUnchanged();
        Success(plan, "real native map and selected-game common/blizzard parse without independent extensions");
        Require(std::filesystem::equivalent(plan.value->executable, game / "war3.exe")
            && std::filesystem::equivalent(plan.value->workingDirectory, game)
            && plan.value->runtimeRoot.empty() && plan.value->runtimeReceipt.empty()
            && plan.value->arguments.size() == 3 && plan.value->arguments[0] == L"-window"
            && plan.value->arguments[1] == L"-loadfile",
            "real original map produces a direct native-game launch plan without tools or runtime");
        const auto relative = plan.value->gameMapCopy.lexically_relative(plan.value->workingDirectory);
        Require(!relative.empty() && !relative.is_absolute() && relative.parent_path().parent_path() == std::filesystem::path("Maps/Audastra")
            && relative.parent_path().filename().wstring().size() == 16 && plan.value->arguments[2] == relative.wstring(),
            "real map plan uses one short dedicated Maps/Audastra task path");
        plannedTask = plan.value->gameMapCopy.parent_path();
        Require(!std::filesystem::exists(plannedTask) && !std::filesystem::exists(plan.value->gameMapCopy),
            "readonly real-plan gate never creates a game task directory or game map copy");
        Require(!plan.value->gameExecutableSha256.empty() && !plan.value->gameModuleSha256.empty(),
            "real selected game executable and module identity remain bound");
    }
    originalUnchanged();
    Require(!std::filesystem::exists(plannedTask), "temporary gate cleanup does not create or touch game task paths");
    std::cout << "Real native-map readonly launch-plan gate passed; map=" << IO::PathUtf8(source)
        << "; source SHA256=" << *originalSha.value << "; selected game=" << IO::PathUtf8(game)
        << "; no launch, DLL execution, GPU, registry or game-directory writes\n";
}
void WriteProviderGameFixtures(const std::filesystem::path& seed, std::string_view expectedSha,
    const std::filesystem::path& directory) {
    const auto sha = Sha256File(seed);
    Success(sha, "verify owned seed");
    Require(*sha.value == expectedSha, "seed identity differs");
    const auto listed = Mpq::MpqArchive::OpenReadOnly(seed);
    Success(listed, "open owned seed archive");
    const auto entries = listed.value->ListFilesStrict();
    Success(entries, "list only seed names");
    for (const auto& entry : *entries.value) {
        auto name = entry.name;
        for (auto& ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        Require(!name.ends_with(".ai") && !name.ends_with(".dll"), "seed has executable members");
    }
    const auto original = MapArchiveSession::Open(seed);
    Success(original, "open owned fixture seed");
    const auto source = original.value->ReadMember("war3map.j");
    Success(source, "read seed entry script");
    const std::string base(reinterpret_cast<const char*>(source.value->data()), source.value->size());
    Require(std::filesystem::create_directory(directory), "use fresh fixture output directory");
    const auto make = [&](const char* file, std::string_view declarations, std::string_view body,
        std::string_view lua = {}) {
        std::string script = base;
        constexpr std::string_view main = "function main takes nothing returns nothing";
        const auto mainStart = script.find(main);
        Require(mainStart != std::string::npos, "seed main entry missing");
        const auto mainEnd = script.find("endfunction", mainStart + main.size());
        Require(mainEnd != std::string::npos, "seed main end missing");
        script.insert(mainEnd, "    call TimerStart(CreateTimer(), 3., false, function AudastraFixtureDisplay)\r\n");
        script.insert(mainStart, "function AudastraFixtureDisplay takes nothing returns nothing\r\n" +
            std::string(body) + "\r\nendfunction\r\n\r\n");
        if (!declarations.empty()) {
            const auto globalEnd = script.find("endglobals");
            Require(globalEnd != std::string::npos && globalEnd < mainStart, "seed globals end missing");
            script.insert(globalEnd + std::string_view("endglobals").size(), "\r\n" + std::string(declarations));
        }
        std::vector<MemberReplacement> replacements{{"war3map.j", Bytes(script)}};
        if (!lua.empty()) replacements.push_back({Audastra::Runtime32::LuaModuleMember("provider"), Bytes(lua)});
        const auto result = original.value->SaveCandidate(directory / file, replacements);
        Success(result, "save test fixture without changing seed");
        std::cout << file << '\t' << result.value->sha256 << '\n';
    };
    make("provider-jass.w3x", "native AudastraFixtureAdd takes integer value returns integer\r\n"
        "native AudastraFixturePlayer takes player value returns player\r\n",
        "    call DisplayTimedTextToPlayer(Player(0), 0., 0., 90., \"R54 JASS RESULT \" + "
        "I2S(AudastraFixtureAdd(35)) + \" / PLAYER \" + I2S(GetPlayerId(AudastraFixturePlayer(Player(0)))))");
    make("provider-lua.w3x", {}, R"JASS(    call Cheat("exec-lua: \"provider\""))JASS", R"LUA(
local ext = require("jass.japi")
local jass = require("jass.common")
local value = ext.AudastraFixtureAdd(35)
local player = ext.AudastraFixturePlayer(jass.Player(0))
local id = jass.GetPlayerId(player)
local ok, error = pcall(ext.AudastraFixtureCallback, function() end)
assert(not ok and tostring(error):find("code", 1, true))
assert(value == 42 and id == 0)
local proof = "R54 LUA RESULT " .. value .. " / PLAYER " .. id .. " / CALLBACK REFUSED"
jass.DisplayTimedTextToPlayer(jass.Player(0), 0, 0, 90, proof)
print(proof)
)LUA");
    make("provider-optional.w3x", {}, R"JASS(    call Cheat("exec-lua: \"provider\""))JASS", R"LUA(
local japi = require("jass.japi")
local jass = require("jass.common")
local value = japi.DzGetMouseX
assert(value == nil)
local proof = "R54B OPTIONAL RESULT: NIL, MAP STARTED"
jass.DisplayTimedTextToPlayer(jass.Player(0), 0, 0, 90, proof)
print(proof)
)LUA");
    const std::string_view exDeclaration = "native EXGetUnitAbility takes unit whichUnit, integer abilityId returns ability\r\n";
    make("ydwe-ex-jass.w3x", exDeclaration,
        "    local unit hero = CreateUnit(Player(0), 'Hpal', 0., 0., 0.)\r\n"
        "    call UnitAddAbility(hero, 'AHhb')\r\n"
        "    if EXGetUnitAbility(hero, 'AHhb') != null then\r\n"
        "        call DisplayTimedTextToPlayer(Player(0), 0., 0., 90., \"R54B EX JASS: ABILITY FOUND\")\r\n"
        "    endif");
    make("ydwe-ex-lua.w3x", exDeclaration, R"JASS(    call Cheat("exec-lua: \"provider\""))JASS", R"LUA(
local ext = require("jass.japi")
local jass = require("jass.common")
local hero = jass.CreateUnit(jass.Player(0), |Hpal|, 0, 0, 0)
jass.UnitAddAbility(hero, |AHhb|)
assert(ext.EXGetUnitAbility(hero, |AHhb|))
jass.DisplayTimedTextToPlayer(jass.Player(0), 0, 0, 90, "R54B EX LUA: ABILITY FOUND")
)LUA");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--check-original-provider-plan") {
            CheckOriginalProviderPlan(argv[2]);
            std::cout << "Original Game.dll identity, provider arguments, and unchanged source verified. No game started.\n";
            return 0;
        }
        if (argc == 5 && std::wstring_view(argv[1]) == L"--make-provider-fixtures") {
            const std::wstring_view expected(argv[3]);
            WriteProviderGameFixtures(argv[2], std::string(expected.begin(), expected.end()), argv[4]);
            return 0;
        }
        Require(argc == 1, "unexpected runtime test arguments");
        Services::MapTestArtifact owned;
        owned.enableLuaHotReload = true;
        FailureCode(Script::BuildIndependentMapTestLaunchPlan(owned), "MAP_TEST_LUA_RELOAD_RUNTIME", "reload needs explicit owned runtime before inspecting map");
        owned.useOwnedLuaRuntime = true;
        Require(!Script::BuildIndependentMapTestLaunchPlan(owned).Succeeded(), "owned runtime needs a real frozen map and cannot fall back");
        ExternalDependencyCatalog(); CliAndHLuaDependencyPreferences();
        OwnedLuaSourcesAndComponents(); NativeMapsNeedNoToolsOrRuntime(); LexicalTextAndLocalPrefixesAreNotExtensions();
        CompiledJassOpaqueBytesPreserveRuntimeClassification(); CompiledJassByteRawcodesAndMalformedBoundaries();
        ExplicitAndUnknownInterfacesNeedRuntime(); RuntimeFilesAndProfileAreCheckedWithoutExecution();
        YdweTreeFallbackIsReadonlyAndOnlyNeededForExtensions();
        UnusedUiPreferencesCannotBlockNativeMaps();
        LuaMapsAreExplicitRuntimeInputs(); HandwrittenLuaEntriesSelectRuntime(); MissingMalformedAndOversizeScriptsStopPlanning();
        AnonymousScriptFlagsAreCheckedBeforeDecoding();
        std::cout << "Independent map runtime preflight fixtures passed; registry settings were process-isolated; no game or DLL executed\n";
        OptionalRealNativeMapPlan();
        OptionalCapturedCompiledScriptPlan();
        OptionalRealExtensionMapPlan();
        OptionalRealYdweRuntimePlan();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
