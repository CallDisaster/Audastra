// 原创小样本：游戏表、元数据和地图物编全部在测试内构造，不读取游戏或 YDWE 文件。
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/SlkOptimizer.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <iostream>
#include <map>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using Row = std::vector<std::pair<std::string, std::string>>;

int failures = 0;
void Check(bool condition, std::string_view label) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
std::vector<std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
std::string Text(const std::vector<std::byte>& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

// 首行为列名；数值单元格写成数字，其余加引号，和游戏表一样混用两种记号。
std::string Table(const std::vector<std::string>& columns, const std::vector<Row>& rows) {
    std::string table = "ID;PWXL;N;E\r\nB;X" + std::to_string(columns.size()) + ";Y" + std::to_string(rows.size() + 1) + ";D0\r\n";
    for (std::size_t x = 0; x < columns.size(); ++x) table += "C;X" + std::to_string(x + 1) + ";Y1;K\"" + columns[x] + "\"\r\n";
    for (std::size_t y = 0; y < rows.size(); ++y) {
        bool first = true;
        for (std::size_t x = 0; x < columns.size(); ++x) {
            const auto found = std::find_if(rows[y].begin(), rows[y].end(), [&](const auto& cell) { return cell.first == columns[x]; });
            if (found == rows[y].end()) continue;
            const bool number = !found->second.empty() && found->second.find_first_not_of("-0123456789.") == std::string::npos;
            table += "C;X" + std::to_string(x + 1) + (first ? ";Y" + std::to_string(y + 2) : std::string{}) + ";K"
                + (number ? found->second : "\"" + found->second + "\"") + "\r\n";
            first = false;
        }
    }
    return table + "E\r\n";
}

std::string Metadata(const std::vector<Row>& rows) {
    return Table({"ID", "field", "slk", "index", "repeat", "data", "appendIndex", "type", "useUnit", "useHero", "useBuilding", "useItem", "useSpecific"}, rows);
}

struct Fixture {
    std::map<std::string, std::string> game;
    std::map<std::string, std::vector<std::byte>> map;
    std::optional<ObjectFieldSchema> schema;

    Fixture() {
        game["Units/UnitMetaData.slk"] = Metadata({
            {{"ID", "uhpm"}, {"field", "HP"}, {"slk", "UnitBalance"}, {"type", "int"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "ussc"}, {"field", "scale"}, {"slk", "UnitUI"}, {"type", "real"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "uabi"}, {"field", "abilList"}, {"slk", "UnitAbilities"}, {"type", "abilityList"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "unam"}, {"field", "Name"}, {"slk", "Profile"}, {"type", "string"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "utub"}, {"field", "Ubertip"}, {"slk", "Profile"}, {"type", "string"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "ubpx"}, {"field", "Buttonpos"}, {"slk", "Profile"}, {"index", "0"}, {"type", "int"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "ubpy"}, {"field", "Buttonpos"}, {"slk", "Profile"}, {"index", "1"}, {"type", "int"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
            {{"ID", "usuf"}, {"field", "EditorSuffix"}, {"slk", "UnitUI"}, {"type", "string"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "0"}},
        });
        game["Units/AbilityMetaData.slk"] = Metadata({
            {{"ID", "alev"}, {"field", "levels"}, {"slk", "AbilityData"}, {"type", "int"}},
            {{"ID", "Hbz1"}, {"field", "Data"}, {"slk", "AbilityData"}, {"repeat", "1"}, {"data", "1"}, {"type", "int"}, {"useSpecific", "AHbz"}},
            {{"ID", "Adm1"}, {"field", "Data"}, {"slk", "AbilityData"}, {"repeat", "1"}, {"data", "1"}, {"type", "int"}, {"useSpecific", "Adef"}},
            {{"ID", "atp1"}, {"field", "Tip"}, {"slk", "Profile"}, {"repeat", "1"}, {"index", "0"}, {"type", "string"}},
        });
        for (const auto* name : {"Units/AbilityBuffMetaData.slk", "Units/UpgradeMetaData.slk", "Doodads/DoodadMetaData.slk", "Units/DestructableMetaData.slk"})
            game[name] = Metadata({});
        game["Units/UnitData.slk"] = Table({"unitID", "race", "isbldg"}, {
            {{"unitID", "hfoo"}, {"race", "human"}, {"isbldg", "0"}}, {{"unitID", "Hpal"}, {"race", "human"}, {"isbldg", "0"}}});
        game["Units/UnitUI.slk"] = Table({"unitUIID", "file", "scale"}, {
            {{"unitUIID", "hfoo"}, {"file", "units\\human\\Footman\\Footman"}, {"scale", "1"}}, {{"unitUIID", "Hpal"}, {"file", "units\\human\\Paladin"}, {"scale", "1.1"}}});
        game["Units/UnitBalance.slk"] = Table({"unitBalanceID", "HP"}, {{{"unitBalanceID", "hfoo"}, {"HP", "420"}}, {{"unitBalanceID", "Hpal"}, {"HP", "650"}}});
        game["Units/UnitWeapons.slk"] = Table({"unitWeapID", "dmgplus1"}, {{{"unitWeapID", "hfoo"}, {"dmgplus1", "11"}}});
        game["Units/UnitAbilities.slk"] = Table({"unitAbilID", "abilList"}, {{{"unitAbilID", "hfoo"}, {"abilList", "Adef"}}, {{"unitAbilID", "Hpal"}, {"abilList", "_"}}});
        game["Units/HumanUnitFunc.txt"] = "[hfoo]\r\nButtonpos=0,0\r\nArt=ReplaceableTextures\\CommandButtons\\BTNFootman.blp\r\n";
        game["Units/HumanUnitStrings.txt"] = "[hfoo]\r\nName=Footman\r\nUbertip=\"Basic, sturdy unit.\"\r\n";
        game["Units/CampaignUnitStrings.txt"] = "// campaign\r\n[ncmp]\r\nName=Campaign Unit\r\n";
        game["Units/AbilityData.slk"] = Table({"alias", "code", "levels", "DataA1", "DataA2"}, {
            {{"alias", "AHbz"}, {"code", "AHbz"}, {"levels", "2"}, {"DataA1", "6"}, {"DataA2", "8"}}});
        game["Units/HumanAbilityStrings.txt"] = "[AHbz]\r\nTip=\"Blizzard - Level 1\",\"Blizzard - Level 2\"\r\n";
        game["Units/CampaignAbilityStrings.txt"] = "[ACmp]\r\nTip=Campaign\r\n";
        for (const auto* name : {"Units/ItemData.slk"}) game[name] = Table({"itemID"}, {});
        game["Units/AbilityBuffData.slk"] = Table({"alias"}, {});
        game["Units/UpgradeData.slk"] = Table({"upgradeid"}, {});
        game["Doodads/Doodads.slk"] = Table({"doodID"}, {});
        game["Units/DestructableData.slk"] = Table({"DestructableID"}, {});

        ObjectFieldSchemaOptions options;
        options.stringFiles = {}; options.worldEditDataFile = {}; options.unitEditorDataFile = {};
        auto loaded = ObjectFieldSchema::Load(Reader(), options);
        Check(loaded.Succeeded(), "synthetic schema");
        if (loaded.Succeeded()) schema = std::move(*loaded.value);
    }

    ObjectFieldSchema::FileReader Reader() const {
        return [this](const std::string& path) {
            const auto found = game.find(path);
            return found == game.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
                : Result<std::vector<std::byte>>::Success(Bytes(found->second));
        };
    }
    SlkOptimizerInput Input() const {
        SlkOptimizerInput input;
        input.mapMember = [this](const std::string& name) -> std::optional<std::vector<std::byte>> {
            const auto found = map.find(War3Map::Text::LowerAscii(name));
            if (found == map.end()) return std::nullopt;
            return found->second;
        };
        input.gameFile = Reader();
        input.schema = &*schema;
        return input;
    }
};

std::vector<std::byte> Encode(ObjectType type, NativeObjectDocument& document) {
    auto bytes = NativeObjectCodec(type).Serialize(document);
    Check(bytes.Succeeded(), "encode native fixture");
    return bytes.Succeeded() ? *bytes.value : std::vector<std::byte>{};
}

const SlkOptimizerReplacement* Find(const SlkOptimizerResult& result, std::string_view name) {
    const auto found = std::find_if(result.replacements.begin(), result.replacements.end(),
        [&](const auto& item) { return War3Map::Text::LowerAscii(item.name) == War3Map::Text::LowerAscii(name); });
    return found == result.replacements.end() ? nullptr : &*found;
}

std::map<std::string, std::string> RowOf(std::string_view text, std::string_view id) {
    auto cells = War3Map::Text::ParseSlkCells(text, War3Map::Text::SlkCellMode::CatalogStrict);
    std::map<std::string, std::string> row;
    if (!cells.Succeeded()) return row;
    for (const auto& [y, values] : *cells.value) {
        if (y == 1 || !values.contains(1) || values.at(1) != id) continue;
        for (const auto& [x, value] : values) row[War3Map::Text::LowerAscii(cells.value->at(1).at(x))] = value;
    }
    return row;
}

void UnitsAndAbilities() {
    Fixture fixture;
    if (!fixture.schema) return;
    auto units = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Check(units.Succeeded(), "empty unit document");
    auto& unitDocument = *units.value;
    const ObjectKey footman{ObjectType::Unit, "hfoo"};
    Check(unitDocument.AddOriginalObject("hfoo").Succeeded(), "original override");
    Check(unitDocument.AddModification(footman, {"uhpm"}, NativeValueType::Int32, std::int32_t{999}).Succeeded(), "original HP override");
    const ObjectKey knight{ObjectType::Unit, "h001"};
    Check(unitDocument.AddCustomObject("h001", "hfoo").Succeeded(), "custom knight");
    Check(unitDocument.AddModification(knight, {"uhpm"}, NativeValueType::Int32, std::int32_t{1234}).Succeeded(), "knight HP");
    Check(unitDocument.AddModification(knight, {"unam"}, NativeValueType::String, std::string("Knight, Sir")).Succeeded(), "knight name with comma");
    Check(unitDocument.AddModification(knight, {"ubpx"}, NativeValueType::Int32, std::int32_t{2}).Succeeded(), "knight button x");
    Check(unitDocument.AddModification(knight, {"ussc"}, NativeValueType::Real, 1.23456789f).Succeeded(), "knight precise scale");
    Check(unitDocument.AddModification(knight, {"uabi"}, NativeValueType::String, std::string("AHbz,Adef")).Succeeded(), "knight ability list");
    Check(unitDocument.AddModification(knight, {"utub"}, NativeValueType::String, std::string("line one\r\nline two")).Succeeded(), "multi-line tooltip");
    Check(unitDocument.AddModification(knight, {"usuf"}, NativeValueType::String, std::string("(editor)")).Succeeded(), "editor suffix without table column");
    Check(unitDocument.AddModification(knight, {"zzzz"}, NativeValueType::Int32, std::int32_t{7}).Succeeded(), "undefined field");
    const ObjectKey named{ObjectType::Unit, "h002"};
    Check(unitDocument.AddCustomObject("h002", "hfoo").Succeeded(), "custom named by string table");
    Check(unitDocument.AddModification(named, {"unam"}, NativeValueType::String, std::string("TRIGSTR_007")).Succeeded(), "trigger string name");
    Check(unitDocument.AddModification(named, {"utub"}, NativeValueType::String, std::string{}).Succeeded(), "explicit empty tooltip");
    Check(unitDocument.AddCustomObject("H003", "Hpal").Succeeded(), "custom hero without profile");
    Check(unitDocument.AddCustomObject("h004", "zzzz").Succeeded(), "parent outside tables");
    Check(unitDocument.AddCustomObject("ncmp", "hfoo").Succeeded(), "id taken by profile section");
    fixture.map["war3map.w3u"] = Encode(ObjectType::Unit, unitDocument);
    fixture.map["war3map.wts"] = Bytes("STRING 7\r\n{\r\nSwordsman\r\n}\r\n");

    auto abilities = NativeObjectDocument::CreateEmpty(ObjectType::Ability);
    auto& abilityDocument = *abilities.value;
    const ObjectKey storm{ObjectType::Ability, "A000"};
    Check(abilityDocument.AddCustomObject("A000", "AHbz").Succeeded(), "custom ability");
    Check(abilityDocument.AddModification(storm, {"alev", 0, 0}, NativeValueType::Int32, std::int32_t{3}).Succeeded(), "ability levels");
    Check(abilityDocument.AddModification(storm, {"Hbz1", 2, 1}, NativeValueType::Int32, std::int32_t{12}).Succeeded(), "level 2 data");
    Check(abilityDocument.AddModification(storm, {"Hbz1", 3, 1}, NativeValueType::Int32, std::int32_t{15}).Succeeded(), "level 3 data beyond table");
    Check(abilityDocument.AddModification(storm, {"atp1", 2, 0}, NativeValueType::String, std::string("Storm, rank 2")).Succeeded(), "level 2 tip");
    // 另一技能专用字段在游戏里写到同一 DataA 列，不能与进表的值重排顺序：整个物体保留。
    Check(abilityDocument.AddCustomObject("A001", "AHbz").Succeeded(), "custom ability with foreign field");
    Check(abilityDocument.AddModification({ObjectType::Ability, "A001"}, {"Adm1", 1, 1}, NativeValueType::Int32, std::int32_t{9}).Succeeded(), "field specific to another ability");
    fixture.map["war3map.w3a"] = Encode(ObjectType::Ability, abilityDocument);

    auto result = OptimizeObjectDataToSlk(fixture.Input());
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Check(false, "synthetic optimization succeeds");
        return;
    }
    Check(result.value->convertedObjects == 4, "h001, h002, H003 and A000 converted");
    Check(result.value->keptCustomObjects == 3, "parent outside tables, profile collision and foreign field kept");
    Check(std::any_of(result.value->notes.begin(), result.value->notes.end(), [](const std::string& note) { return note.starts_with("A001"); }),
        "foreign-field ability reported");
    Check(result.value->verifiedValues > 0, "effective values compared");

    const auto* balance = Find(*result.value, "Units\\UnitBalance.slk");
    Check(balance != nullptr, "unit balance written");
    if (balance) {
        const auto text = Text(balance->bytes);
        // 原有记录逐字节保留，只有 B 记录的行数随三行新单位更新（3 → 6）。
        auto original = fixture.game["Units/UnitBalance.slk"];
        original = original.substr(0, original.size() - 3).replace(original.find(";Y3;D0"), 6, ";Y6;D0");
        Check(text.starts_with(original) && text.ends_with("E\r\n"), "original records kept and row count updated");
        Check(RowOf(text, "h001")["hp"] == "1234", "custom HP in table");
        Check(RowOf(text, "hfoo")["hp"] == "420", "original row keeps default; author override stays in object file");
        Check(RowOf(text, "H003")["hp"] == "650", "hero row copied from parent");
        Check(text.find("B;X2;Y6;") != std::string::npos, "bounds row count updated");
    }
    if (const auto* ui = Find(*result.value, "Units\\UnitUI.slk")) {
        const auto scale = RowOf(Text(ui->bytes), "h001")["scale"];
        Check(static_cast<float>(std::stod(scale)) == 1.23456789f, "real written with exact float round trip");
    } else Check(false, "unit UI written");
    if (const auto* abilitiesTable = Find(*result.value, "Units\\UnitAbilities.slk"))
        Check(RowOf(Text(abilitiesTable->bytes), "h001")["abillist"] == "AHbz,Adef", "ability list string cell");
    if (const auto* profile = Find(*result.value, "Units\\CampaignUnitStrings.txt")) {
        const auto sections = War3Map::Text::ParseProfile(Text(profile->bytes));
        Check(sections.contains("ncmp") && sections.at("ncmp").at("name") == "Campaign Unit", "existing campaign section kept");
        Check(sections.contains("h001") && sections.at("h001").at("name") == "\"Knight, Sir\"", "comma name quoted");
        Check(sections.contains("h001") && sections.at("h001").at("buttonpos") == "2,0", "button x replaced, y inherited");
        Check(sections.contains("h001") && sections.at("h001").at("ubertip") == "\"Basic, sturdy unit.\"", "multi-line tooltip not written; parent value inherited");
        Check(sections.contains("h002") && sections.at("h002").at("name") == "Swordsman", "trigger string expanded");
        Check(sections.contains("h002") && sections.at("h002").at("ubertip").empty(), "explicit empty tooltip");
        Check(Text(profile->bytes).find("[H003]") != std::string::npos, "hero section written even without profile source");
    } else Check(false, "campaign unit strings written");
    if (const auto* objects = Find(*result.value, "war3map.w3u")) {
        auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(objects->bytes);
        const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        Check(document != nullptr, "residual unit file parses");
        if (document) {
            const auto* override = document->FindObject(footman);
            Check(override && !override->parentRawcode && override->modifications.size() == 1, "original override kept");
            const auto* residual = document->FindObject(knight);
            Check(residual && !residual->parentRawcode && residual->modifications.size() == 3, "knight residual: tooltip, suffix, undefined field");
            Check(document->FindObject(named) == nullptr, "fully converted object leaves no entry");
            const auto* kept = document->FindObject({ObjectType::Unit, "h004"});
            Check(kept && kept->parentRawcode == "zzzz", "unconvertible custom object kept");
        }
    } else Check(false, "residual unit file written");
    if (const auto* data = Find(*result.value, "Units\\AbilityData.slk")) {
        auto row = RowOf(Text(data->bytes), "A000");
        Check(row["code"] == "AHbz" && row["levels"] == "3" && row["dataa1"] == "6" && row["dataa2"] == "12", "ability row keeps code, sets levels and level 2 data");
    } else Check(false, "ability data written");
    if (const auto* profile = Find(*result.value, "Units\\CampaignAbilityStrings.txt")) {
        const auto sections = War3Map::Text::ParseProfile(Text(profile->bytes));
        Check(sections.contains("a000") && sections.at("a000").at("tip") == "\"Blizzard - Level 1\",\"Storm, rank 2\"", "level 2 tip replaced in list");
    } else Check(false, "campaign ability strings written");
    if (const auto* objects = Find(*result.value, "war3map.w3a")) {
        auto parsed = NativeObjectCodec(ObjectType::Ability).Parse(objects->bytes);
        const auto* document = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        const auto* residual = document ? document->FindObject(storm) : nullptr;
        Check(residual && !residual->parentRawcode && residual->modifications.size() == 1
            && residual->modifications.front().key.level == 3u, "level beyond table columns stays in object file");
    } else Check(false, "residual ability file written");
}

void LniRejected() {
    Fixture fixture;
    if (!fixture.schema) return;
    fixture.map["table\\unit.ini"] = Bytes("[h001]\n");
    const auto result = OptimizeObjectDataToSlk(fixture.Input());
    Check(!result.Succeeded() && result.diagnostics.front().code == "SLK_LNI_UNSUPPORTED", "LNI object data refused");
}

void NothingToConvert() {
    Fixture fixture;
    if (!fixture.schema) return;
    auto units = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Check(units.value->AddOriginalObject("hfoo").Succeeded(), "original only");
    fixture.map["war3map.w3u"] = Encode(ObjectType::Unit, *units.value);
    const auto result = OptimizeObjectDataToSlk(fixture.Input());
    Check(result.Succeeded() && result.value->replacements.empty() && result.value->convertedObjects == 0, "no custom objects means no changes");
}

} // namespace

int main() {
    UnitsAndAbilities();
    LniRejected();
    NothingToConvert();
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "slk_optimizer_synthetic=1\n";
    return 0;
}
