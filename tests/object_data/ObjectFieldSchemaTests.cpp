#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Game/GameDataSource.h"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using Row = std::map<std::string, std::string>;
int failures = 0;
void Check(bool condition, std::string_view label) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}
std::string Quote(std::string text) {
    std::string escaped;
    for (const char c : text) { escaped += c; if (c == '"') escaped += c; }
    return '"' + escaped + '"';
}
std::string Table(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row) {
        (void)value;
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    }
    std::string table = "ID;PWXL\n";
    for (std::size_t x = 0; x < columns.size(); ++x) table += "C;X" + std::to_string(x + 1) + ";Y1;K" + Quote(columns[x]) + '\n';
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        const auto found = rows[y].find(columns[x]);
        if (found != rows[y].end()) table += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 2) + ";K" + Quote(found->second) + '\n';
    }
    return table + "E\n";
}

void Synthetic() {
    std::map<std::string, std::string> files;
    files["unit.slk"] = Table({
        {{"ID", "uhpm"}, {"field", "HP"}, {"slk", "UnitBalance"}, {"type", "int"}, {"displayName", "WESTRING_TEST_HP"}, {"category", "stats"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
        {{"ID", "ihtp"}, {"field", "HP"}, {"slk", "ItemData"}, {"type", "int"}, {"useUnit", "0"}, {"useHero", "0"}, {"useBuilding", "0"}, {"useItem", "1"}},
        {{"ID", "ua1b"}, {"field", "dmgplus1"}, {"slk", "UnitWeapons"}, {"type", "int"}},
        {{"ID", "ussc"}, {"field", "scale"}, {"slk", "UnitUI"}, {"type", "real"}},
        {{"ID", "unam"}, {"field", "Name"}, {"slk", "Profile"}, {"index", "0"}, {"type", "string"}},
        {{"ID", "ubpx"}, {"field", "Buttonpos"}, {"slk", "Profile"}, {"index", "0"}, {"type", "int"}},
        {{"ID", "ubpy"}, {"field", "Buttonpos"}, {"slk", "Profile"}, {"index", "1"}, {"type", "int"}},
        {{"ID", "uabi"}, {"field", "abilList"}, {"slk", "UnitAbilities"}, {"type", "abilityList"}},
        {{"ID", "urac"}, {"field", "race"}, {"slk", "UnitData"}, {"type", "unitRace"}},
        {{"ID", "xxxx"}, {"field", "CustomPayload"}, {"slk", "UnitData"}, {"type", "UnknownExtensionType"}},
    });
    files["ability.slk"] = Table({
        {{"ID", "Hbz1"}, {"field", "Data"}, {"slk", "AbilityData"}, {"type", "int"}, {"repeat", "4"}, {"data", "1"}, {"useSpecific", "AHbz,ACbz"}},
        {{"ID", "Ocl1"}, {"field", "Data"}, {"slk", "AbilityData"}, {"type", "unreal"}, {"repeat", "4"}, {"data", "1"}, {"useSpecific", "AOcl"}},
        {{"ID", "adur"}, {"field", "Dur"}, {"slk", "AbilityData"}, {"type", "unreal"}, {"repeat", "4"}},
        {{"ID", "atp1"}, {"field", "Tip"}, {"slk", "Profile"}, {"type", "string"}, {"repeat", "3"}, {"index", "0"}},
        {{"ID", "alev"}, {"field", "levels"}, {"slk", "AbilityData"}, {"type", "int"}},
    });
    files["upgrade.slk"] = Table({{{"ID", "greq"}, {"field", "Requires"}, {"slk", "Profile"}, {"type", "techList"}, {"repeat", "1"}, {"appendIndex", "1"}, {"index", "-1"}}});
    files["doodad.slk"] = Table({{{"ID", "dvr1"}, {"field", "vertR"}, {"slk", "DoodadData"}, {"type", "int"}, {"repeat", "10"}}});
    files["strings.txt"] = "\xEF\xBB\xBF[WorldEditStrings]\nWESTRING_TEST_HP=&测试生命上限 (&H)\nWESTRING_TEST_STATS=测试状态（&S）\nWESTRING_TEST_RACE=&测试&&种族\n";
    files["world.txt"] = "[ObjectEditorCategories]\nstats=WESTRING_TEST_STATS\n";
    files["editor.txt"] = "[unitRace]\n00=synthetic,WESTRING_TEST_RACE\n00_Alt=synth,test\nNumValues=1\n";
    const ObjectFieldSchema::FileReader reader = [&files](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    ObjectFieldSchemaOptions options;
    options.sources = {{ObjectType::Unit, "unit.slk"}, {ObjectType::Item, "unit.slk"}, {ObjectType::Ability, "ability.slk"}, {ObjectType::Upgrade, "upgrade.slk"}, {ObjectType::Doodad, "doodad.slk"}};
    options.stringFiles = {"strings.txt"}; options.worldEditDataFile = "world.txt"; options.unitEditorDataFile = "editor.txt";
    auto parsed = ObjectFieldSchema::Load(reader, options);
    Check(parsed.Succeeded(), "synthetic schema load");
    if (!parsed.Succeeded()) return;
    const auto& schema = *parsed.value;
    const auto hp = schema.MatchSourceField(ObjectType::Unit, "hp");
    Check(hp.definition && hp.definition->fieldId == "uhpm" && hp.safeToEdit, "unit lowercase hp resolves to native uhpm");
    Check(hp.definition && hp.definition->displayName == "测试生命上限" && hp.definition->categoryDisplayName == "测试状态", "labels come from source string/category tables");
    const auto itemHp = schema.MatchSourceField(ObjectType::Item, "HP");
    Check(itemHp.definition && itemHp.definition->fieldId == "ihtp", "unit/item use flags separate identical source key");
    Check(schema.MatchSourceField(ObjectType::Unit, "dmgplus1").modification.fieldId == "ua1b", "attack suffix is an exact source column, not a level");
    Check(schema.FindNative(ObjectType::Unit, "ussc")->nativeValueType == NativeValueType::Real, "real storage exact");
    Check(schema.FindNative(ObjectType::Ability, "adur")->nativeValueType == NativeValueType::Unreal, "unreal storage exact");
    const ObjectFieldContext blizzard{"A000", "AHbz"};
    const auto data = schema.MatchSourceField(ObjectType::Ability, "DataA1", blizzard);
    Check(data.definition && data.modification.fieldId == "Hbz1" && data.modification.level == 1u && data.modification.dataIndex == 1u && data.safeToEdit, "specific DataA level/data addressing");
    Check(!schema.MatchSourceField(ObjectType::Ability, "DataA1").definition, "no identity does not guess specialized data");
    Check(!schema.FindNative(ObjectType::Ability, "hbz1"), "native field ID remains case sensitive");
    Check(!schema.MatchSourceField(ObjectType::Unit, "buttonpos").definition, "compound source field remains ambiguous");
    Check(schema.MatchSourceField(ObjectType::Unit, "buttonpos", {}, 1).modification.fieldId == "ubpy", "explicit profile component resolves y");
    const auto tip = schema.MatchSourceField(ObjectType::Ability, "tip", {}, 2);
    Check(tip.definition && tip.modification.level == 3u && tip.sourceIndex == 2u && tip.safeToEdit, "profile CSV levels are zero based while native level is one based");
    Check(!schema.MatchSourceField(ObjectType::Ability, "tip").safeToEdit, "unindexed profile level sequence is not scalar-editable");
    const auto requirement = schema.MatchSourceField(ObjectType::Upgrade, "requires1");
    Check(requirement.definition && requirement.modification.level == 2u && !requirement.sourceIndex && requirement.safeToEdit, "appendIndex profile level two is Requires1");
    const auto varied = schema.MatchSourceField(ObjectType::Doodad, "vertr01");
    Check(varied.definition && varied.modification.variation == 1u && !varied.safeToEdit, "doodad padded variation identified but write numbering gate retained");
    const auto list = schema.FindNative(ObjectType::Unit, "uabi");
    Check(list && list->isList && list->referenceTypes == std::vector{ObjectType::Ability}, "ability list reference metadata");
    const auto race = schema.FindNative(ObjectType::Unit, "urac");
    Check(race && race->choices.size() == 1 && race->choices[0].displayName == "测试&种族" && race->choices[0].aliases.size() == 2, "enum display mnemonics removed while escaped ampersand remains literal");
    Check(hp.definition && hp.definition->displayNameToken == "WESTRING_TEST_HP" && hp.definition->sourceField == "HP"
        && hp.definition->category == "stats" && race && race->choices[0].value == "synthetic",
        "display cleanup preserves metadata tokens, source keys, category keys and enum values");
    const auto opaque = schema.MatchSourceField(ObjectType::Unit, "custompayload");
    Check(opaque.definition && !opaque.safeToEdit, "unknown metadata type retained but not guessed writable");
    Check(!schema.MatchSourceField(ObjectType::Unit, "opaque_extension").definition, "unknown source key retained as unmatched");
    ModificationKey wrongData{"Hbz1", 1, 2, {}};
    Check(!schema.MatchNativeField(ObjectType::Ability, wrongData, blizzard).safeToEdit, "native data index mismatch rejected");
    auto native = schema.MatchNativeField(ObjectType::Ability, data.modification, blizzard);
    Check(native.safeToEdit && native.sourceKey == "dataa1", "source/native address roundtrip");
    // 统一字符串表优先：字段名与分类名先取覆盖层，表里没有的键仍回到 stringFiles。
    Game::WorldEditStrings table;
    table.AddText("overlay", "[WorldEditStrings]\nWESTRING_TEST_HP=覆盖生命 (&H)\n");
    options.strings = &table;
    auto overlaid = ObjectFieldSchema::Load(reader, options);
    const auto* overlaidHp = overlaid.Succeeded() ? overlaid.value->FindNative(ObjectType::Unit, "uhpm") : nullptr;
    Check(overlaidHp && overlaidHp->displayName == "覆盖生命" && overlaidHp->categoryDisplayName == "测试状态",
        "string table overrides field names and falls back per key");
}

void Real(const std::filesystem::path& root) {
    auto game = Game::GameDataSource::Mount(root);
    Check(game.Succeeded(), "read-only game mount"); if (!game.Succeeded()) return;
    auto parsed = ObjectFieldSchema::Load(*game.value);
    Check(parsed.Succeeded(), "real metadata schema load"); if (!parsed.Succeeded()) return;
    const auto& schema = *parsed.value;
    for (const auto type : std::array{ObjectType::Unit, ObjectType::Item, ObjectType::Ability, ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        std::size_t count = 0, named = 0, typed = 0;
        for (const auto& field : schema.Definitions()) if (field.objectType == type) {
            ++count; named += field.displayNameResolved; typed += field.nativeValueType.has_value();
        }
        Check(count > 0, "all seven metadata kinds loaded");
        std::cout << ToString(type) << " fields=" << count << " localized=" << named << " nativeTyped=" << typed << '\n';
    }
    const ObjectFieldContext unit{"hfoo"};
    const ObjectFieldContext ability{"AHbz", "AHbz"};
    for (const auto& request : std::array{std::pair{ObjectType::Unit, "hp"}, std::pair{ObjectType::Unit, "dmgplus1"},
        std::pair{ObjectType::Ability, "dataa1"}, std::pair{ObjectType::Ability, "levels"}}) {
        const auto match = schema.MatchSourceField(request.first, request.second, request.first == ObjectType::Ability ? ability : unit);
        Check(match.definition && match.safeToEdit, "real representative field resolves safely");
        if (match.definition) std::cout << "field=" << request.second << " native=" << match.modification.fieldId << " name=" << match.definition->displayName
            << " category=" << match.definition->categoryDisplayName << " type=" << match.definition->valueType
            << " level=" << match.modification.level.value_or(0) << " data=" << match.modification.dataIndex.value_or(0) << '\n';
    }
    auto catalog = StandardObjectCatalog::Load(*game.value);
    Check(catalog.Succeeded(), "standard catalog with weapon/ability source tables");
    if (catalog.Succeeded()) {
        const auto* footman = catalog.value->Find(ObjectType::Unit, "hfoo");
        Check(footman && !footman->Field("dmgplus1").empty() && !footman->Field("abillist").empty(), "unit default damage and abilities loaded from real tables");
        std::cout << "catalogLoadedFiles=" << catalog.value->LoadedFiles().size() << '\n';
        if (footman) std::cout << "hfoo dmgplus1=" << footman->Field("dmgplus1") << " abillist=" << footman->Field("abillist") << '\n';
    }
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    Synthetic();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--game-root") Real(argv[2]);
    std::cout << "object-field-schema failures=" << failures << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
