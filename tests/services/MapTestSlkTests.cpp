#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/Script/MapTestToolchain.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Hash.h"
#include "../MapDataFixtures.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

// 原生 SLK 测试优化的真实游戏数据门禁：用本机原版 MPQ 的物编表生成测试副本，
// 不需要 YDWE、Lua 或 w3x2lni。未设置游戏目录时跳过（返回 77）。
namespace {
using namespace War3Map;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())}; }
std::string Text(const std::vector<std::byte>& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
template<class T> void Success(const T& result, const char* message) {
    if (result.Succeeded()) return;
    std::string text(message); for (const auto& d : result.diagnostics) text += "\n" + d.code + ": " + d.message;
    throw std::runtime_error(text);
}
// 节内某键的原始值；无此节或键返回空。
std::optional<std::string> ProfileValue(std::string_view text, std::string_view id, std::string_view key) {
    bool section = false;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start); if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start); if (line.ends_with('\r')) line.remove_suffix(1);
        start = end + 1;
        if (line.starts_with('[')) { section = line == "[" + std::string(id) + "]"; continue; }
        if (section && line.starts_with(std::string(key) + "=")) return std::string(line.substr(key.size() + 1));
    }
    return std::nullopt;
}
bool SlkHasRow(std::string_view text, std::string_view id) {
    return text.find(";K\"" + std::string(id) + "\"") != std::string_view::npos;
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("forge-test-slk-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Temp() { Require(std::filesystem::create_directory(root), "fresh owned SLK fixture root"); }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    Require(size <= 32768, "environment path exceeds supported Windows budget");
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    Require(written != 0 && written < size, "environment changed during bounded read");
    value.resize(written);
    return std::filesystem::path(value);
}
template<class Document, class Value> void Add(Document& document, ObjectData::ObjectType type, const char* id,
    const ObjectData::ModificationKey& key, ObjectData::NativeValueType valueType, Value value) {
    Require(document.AddModification({type, id}, key, valueType, std::move(value)).Succeeded(), key.fieldId.c_str());
}
}

int main(int argc, char** argv) {
    try {
        std::filesystem::path game = argc == 2 ? std::filesystem::absolute(argv[1]) : EnvironmentPath(L"FORGE_WORLD_GAME_ROOT");
        if (game.empty()) { std::cout << "No game root configured (FORGE_WORLD_GAME_ROOT); native SLK game-data gate not executed\n"; return 77; }
        Temp temp;
        using ObjectData::ObjectType; using ObjectData::NativeValueType;
        auto units = ObjectData::NativeObjectDocument::CreateEmpty(ObjectType::Unit); Success(units, "create unit fixture");
        Require(units.value->AddCustomObject("H001", "hfoo").Succeeded(), "custom unit");
        Add(*units.value, ObjectType::Unit, "H001", {"uhpm"}, NativeValueType::Int32, std::int32_t{1234});
        Add(*units.value, ObjectType::Unit, "H001", {"unam"}, NativeValueType::String, std::string("TRIGSTR_042"));
        Add(*units.value, ObjectType::Unit, "H001", {"utub"}, NativeValueType::String, std::string("Strong, fast"));
        Require(units.value->AddCustomObject("E001", "Ecen").Succeeded(), "custom hero");
        Add(*units.value, ObjectType::Unit, "E001", {"utub"}, NativeValueType::String, std::string{});
        // 作者对原版物体的修改不进表，原样留在物编文件。
        Require(units.value->AddOriginalObject("hfoo").Succeeded(), "original unit override");
        Add(*units.value, ObjectType::Unit, "hfoo", {"uhpm"}, NativeValueType::Int32, std::int32_t{777});
        const auto unitBytes = ObjectData::NativeObjectCodec(ObjectType::Unit).Serialize(*units.value); Success(unitBytes, "encode units");

        auto destructables = ObjectData::NativeObjectDocument::CreateEmpty(ObjectType::Destructable); Success(destructables, "create destructable fixture");
        Require(destructables.value->AddCustomObject("D001", "ATg1").Succeeded(), "custom destructable");
        Add(*destructables.value, ObjectType::Destructable, "D001", {"bmas"}, NativeValueType::Unreal, 1.23456789f);
        const auto destructableBytes = ObjectData::NativeObjectCodec(ObjectType::Destructable).Serialize(*destructables.value); Success(destructableBytes, "encode destructables");

        auto abilities = ObjectData::NativeObjectDocument::CreateEmpty(ObjectType::Ability); Success(abilities, "create ability fixture");
        Require(abilities.value->AddCustomObject("A000", "AHbz").Succeeded(), "custom ability");
        Add(*abilities.value, ObjectType::Ability, "A000", {"Hbz2", 2, 2}, NativeValueType::Unreal, 12.5f);
        const auto abilityBytes = ObjectData::NativeObjectCodec(ObjectType::Ability).Serialize(*abilities.value); Success(abilityBytes, "encode abilities");

        auto settings = MapDataFixtures::FullMapSettingsV25();
        const std::vector<MapArchiveMember> members{{"war3map.w3i", settings.bytes}, {"war3map.w3u", *unitBytes.value},
            {"war3map.w3b", *destructableBytes.value}, {"war3map.w3a", *abilityBytes.value},
            {"war3map.j", Bytes("// unchanged source JASS\n")}, {"war3map.wts", Bytes("STRING 42\r\n{\r\nFixture Footman\r\n}\r\n")},
            {"tiara.ai", Bytes("opaque-ai-byte-fixture")}, {"war3map.wtg", Bytes("opaque-author-trigger-byte-fixture")},
            {"opaque/resource.bin", {std::byte{19}, std::byte{0}, std::byte{99}}}};
        const auto source = WriteVerifiedArchive(temp.root / "source.w3x", members); Success(source, "write original map fixture");

        Services::MapTestRequest request;
        request.inputMap = source.value->path; request.expectedInputSha256 = source.value->sha256; request.sourceMap = source.value->path;
        request.gameRoot = game; request.workspace = temp.root / "prepared"; request.optimizeSlk = true;
        const auto optimized = Services::PrepareMapTest(request, Script::OptimizeMapTestSlk);
        Success(optimized, "native SLK conversion with real game tables");
        Require(HasCode(optimized, "MAP_TEST_SLK_SUMMARY"), "conversion summary reaches the caller");
        Require(optimized.value->optimizedSlk && optimized.value->testMap != source.value->path, "SLK only writes the independent artifact");
        Require(!std::filesystem::exists(request.workspace / "slk-test.lua"), "no external Lua runner is written");

        const auto final = MapArchiveSession::Open(optimized.value->testMap); Success(final, "open optimized test archive");
        const auto unitData = final.value->ReadMember("Units\\UnitData.slk"); Success(unitData, "read generated unit table");
        Require(SlkHasRow(Text(*unitData.value), "H001") && SlkHasRow(Text(*unitData.value), "E001"), "custom units become table rows");
        const auto unitBalance = final.value->ReadMember("Units\\UnitBalance.slk"); Success(unitBalance, "read generated balance table");
        Require(Text(*unitBalance.value).find("K1234") != std::string::npos, "custom hit points written into the balance row");
        const auto unitProfile = final.value->ReadMember("Units\\CampaignUnitStrings.txt"); Success(unitProfile, "read generated unit profile");
        const auto profile = Text(*unitProfile.value);
        Require(ProfileValue(profile, "H001", "Name") == std::optional<std::string>("Fixture Footman"), "TRIGSTR name resolved into profile");
        Require(ProfileValue(profile, "H001", "Ubertip") == std::optional<std::string>("\"Strong, fast\""), "comma text quoted as one item");
        Require(ProfileValue(profile, "E001", "Ubertip") == std::optional<std::string>(""), "explicit empty author tooltip kept");
        Require(final.value->HasMember("Units\\DestructableData.slk"), "destructable table generated");
        const auto destructableData = final.value->ReadMember("Units\\DestructableData.slk"); Success(destructableData, "read destructable table");
        Require(SlkHasRow(Text(*destructableData.value), "D001"), "custom destructable becomes a table row");
        const auto abilityData = final.value->ReadMember("Units\\AbilityData.slk"); Success(abilityData, "read ability table");
        Require(SlkHasRow(Text(*abilityData.value), "A000"), "custom ability becomes a table row");

        // 精简后的单位物编只剩作者对原版物体的修改。
        const auto residualBytes = final.value->ReadMember("war3map.w3u"); Success(residualBytes, "read residual unit object data");
        const auto residual = ObjectData::NativeObjectCodec(ObjectType::Unit).Parse(*residualBytes.value);
        Require(residual.Succeeded(), "residual unit object data parses");
        const auto* residualDocument = dynamic_cast<const ObjectData::NativeObjectDocument*>(residual.document.get());
        Require(residualDocument != nullptr, "residual document type");
        Require(residualDocument->FindObject({ObjectType::Unit, "H001"}) == nullptr, "converted custom unit leaves the object file");
        Require(residualDocument->FindObject({ObjectType::Unit, "E001"}) == nullptr, "new single-item profile list fully converts the hero");
        Require(residualDocument->FindObject({ObjectType::Unit, "hfoo"}) != nullptr, "author override of the original unit stays");

        for (const auto& member : members) {
            if (member.name.starts_with("war3map.w3") && member.name != "war3map.w3i") continue;
            const auto bytes = final.value->ReadMember(member.name); Success(bytes, "unrelated member retained");
            Require(*bytes.value == member.data, "JASS, WTS, WTG, imports and opaque runtime bytes preserved");
        }
        Require(*Sha256File(source.value->path).value == source.value->sha256, "source map byte identity retained");

        // 拒绝路径：缺游戏目录、地图含 LNI 物编。均不写出地图。
        const auto workspace = source.value->path.parent_path();
        Require(std::filesystem::equivalent(workspace, temp.root), "rejection fixture stays in the owned temporary directory");
        Services::MapTestSlkRequest rejected{source.value->path, workspace / "rejected.w3x", {}, workspace, source.value->sha256};
        Require(HasCode(Script::OptimizeMapTestSlk(rejected), "MAP_TEST_SLK_GAME"), "missing game root fails before reading tables");
        Require(!std::filesystem::exists(rejected.outputMap), "rejected run writes no map");

        Temp lniTemp;
        auto lniMembers = members; lniMembers.push_back({"table\\unit.ini", Bytes("[H001]\nHP = 1\n")});
        const auto lniSource = WriteVerifiedArchive(lniTemp.root / "source.w3x", lniMembers); Success(lniSource, "write LNI fixture");
        const auto lniWorkspace = lniSource.value->path.parent_path();
        Services::MapTestSlkRequest lni{lniSource.value->path, lniWorkspace / "lni.w3x", game, lniWorkspace, lniSource.value->sha256};
        Require(HasCode(Script::OptimizeMapTestSlk(lni), "SLK_LNI_UNSUPPORTED"), "LNI object data is rejected rather than ignored");
        Require(!std::filesystem::exists(lni.outputMap), "LNI rejection writes no map");

        // 可选真实地图门禁：只读源图，输出写进本次临时目录；打印转换摘要。
        if (const auto realMap = EnvironmentPath(L"FORGE_SLK_REAL_MAP"); !realMap.empty()) {
            const auto realSha = Sha256File(realMap); Success(realSha, "hash real map");
            Temp realTemp;
            Services::MapTestRequest real;
            real.inputMap = realMap; real.expectedInputSha256 = *realSha.value; real.sourceMap = realMap;
            real.gameRoot = game; real.workspace = realTemp.root / "prepared"; real.optimizeSlk = true;
            const auto started = GetTickCount64();
            const auto prepared = Services::PrepareMapTest(real, Script::OptimizeMapTestSlk);
            for (const auto& diagnostic : prepared.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
            Success(prepared, "native SLK on the configured real map");
            Require(*Sha256File(realMap).value == *realSha.value, "real source map unchanged");
            std::cout << "Real map SLK in " << (GetTickCount64() - started) << " ms\n";
        }
        std::cout << "Native SLK game-data gate passed; no YDWE, Lua or w3x2lni used\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
