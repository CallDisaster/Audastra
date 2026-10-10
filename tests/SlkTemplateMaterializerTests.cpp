#include "War3MapCore/Script/SlkTemplateMaterializer.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

// 原创 typed defaults、SLK metadata、native 和 WTS；不读取或运行 YDWE/私图。
namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Script;
constexpr std::array<ObjectType, 7> kTypes{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
    ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
constexpr std::array<std::string_view, 7> kNames{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
using Row = std::map<std::string, std::string>;
void Require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
void Require(const Status& status) { Require(status.Succeeded(), status.diagnostics.empty() ? "fixture failed" : status.diagnostics.front().message); }
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char c : text) result.push_back(static_cast<std::byte>(c));
    return result;
}
std::string Digest(const std::vector<std::byte>& bytes) {
    const auto result = Sha256(bytes); Require(result.Succeeded(), "fixture hash failed"); return *result.value;
}
SlkSessionValue Scalar(SlkSessionScalar value) { return value; }
SlkSessionValue String(std::string_view value) { return Scalar(std::string(value)); }
SlkSessionValue Array(std::vector<SlkSessionScalar> values) { return values; }
std::string Table(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row) {
        (void)value;
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    }
    std::string text = "ID;PWXL\n";
    for (std::size_t x = 0; x < columns.size(); ++x)
        text += "C;X" + std::to_string(x + 1) + ";Y1;K\"" + columns[x] + "\"\n";
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        if (const auto entry = rows[y].find(columns[x]); entry != rows[y].end())
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 2) + ";K\"" + entry->second + "\"\n";
    }
    return text + "E\n";
}
ObjectFieldSchema Schema(std::string_view abilityScope = {}, std::size_t extraRepeatFields = 0) {
    std::map<std::string, std::string> files;
    ObjectFieldSchemaOptions options;
    options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
    for (std::size_t i = 0; i < 7; ++i) {
        std::vector<Row> rows{
            {{"ID", "Name"}, {"field", "Name"}, {"slk", "Profile"}, {"type", "string"}, {"index", "0"}},
            {{"ID", "Flag"}, {"field", "Flag"}, {"slk", "Data"}, {"type", "bool"}},
        };
        if (i == 1) {
            if (!abilityScope.empty()) rows[0][std::string(abilityScope)] = "P001";
            rows.push_back({{"ID", "alev"}, {"field", "levels"}, {"slk", "AbilityData"}, {"type", "int"}});
            rows.push_back({{"ID", "Adat"}, {"field", "Data"}, {"slk", "AbilityData"}, {"type", "unreal"}, {"repeat", "4"}, {"data", "1"}});
            rows.push_back({{"ID", "Atip"}, {"field", "Tip"}, {"slk", "Profile"}, {"type", "string"}, {"repeat", "4"}, {"index", "0"}});
            rows.push_back({{"ID", "Aapp"}, {"field", "App"}, {"slk", "Profile"}, {"type", "string"}, {"repeat", "4"}, {"index", "0"}, {"appendIndex", "1"}});
            rows.push_back({{"ID", "Aspt"}, {"field", "Split"}, {"slk", "Profile"}, {"type", "string"}, {"index", "-1"}});
            for (std::size_t field = 0; field < extraRepeatFields; ++field)
                rows.push_back({{"ID", std::to_string(1000 + field)}, {"field", "Extra" + std::to_string(field)},
                    {"slk", "AbilityData"}, {"type", "int"}, {"repeat", "4"}});
        }
        if (i == 4) rows.push_back({{"ID", "glvl"}, {"field", "maxlevel"}, {"slk", "UpgradeData"}, {"type", "int"}});
        if (i == 5) {
            rows.push_back({{"ID", "dvar"}, {"field", "numvar"}, {"slk", "DoodadData"}, {"type", "int"}});
            rows.push_back({{"ID", "dnum"}, {"field", "Number"}, {"slk", "DoodadData"}, {"type", "real"}, {"repeat", "4"}});
        }
        for (auto& row : rows) for (const auto key : {"useUnit", "useHero", "useBuilding", "useItem", "useCreep"}) row[key] = "1";
        const std::string file = std::string(kNames[i]) + ".slk";
        files[file] = Table(rows); options.sources.push_back({kTypes[i], file});
    }
    const auto loaded = ObjectFieldSchema::Load([&](const std::string& path) {
        return Result<std::vector<std::byte>>::Success(Bytes(files.at(path)));
    }, options);
    Require(loaded.Succeeded() && loaded.diagnostics.empty(), "原创 metadata 加载失败");
    return *loaded.value;
}
SlkSessionObject Base(std::size_t i) {
    SlkSessionObject result;
    result.id = "P001";
    result.fields = {{"_id", String("P001")}, {"_code", String("P001")}, {"_type", String(kNames[i])},
        {"name", String("默认名称")}, {"flag", Scalar(std::int64_t{1})}};
    if (i == 1) {
        result.fields["_max_level"] = Scalar(std::int64_t{2});
        result.fields["levels"] = Scalar(std::int64_t{2});
        result.fields["dataa"] = Array({1.0, 2.0});
        result.fields["tip"] = Array({std::string("tip1")});
        result.fields["app"] = Array({std::string("app1")});
        result.fields["split"] = String("default");
    }
    if (i == 4 || i == 5) {
        result.fields["_max_level"] = Scalar(std::int64_t{1});
        result.fields[i == 4 ? "maxlevel" : "numvar"] = Scalar(std::int64_t{1});
        if (i == 5) result.fields["number"] = Array({1.0});
    }
    return result;
}
struct Fixture {
    ObjectFieldSchema schema = Schema();
    std::array<std::optional<NativeObjectDocument>, 7> documents;
    SlkNativeFrontendInput input;
    std::optional<MapData::StringTableDocument> strings;
    Fixture() {
        input.verifiedNativeOnlySources = true;
        input.bindings.candidateSha256 = std::string(64, '1');
        input.bindings.metadataSha256 = std::string(64, '2');
        input.bindings.orderedTemplatesSha256 = std::string(64, '3');
        input.bindings.materializerSha256 = std::string(64, '4');
        input.bindings.frontendModelSha256 = std::string(64, '5');
        input.bindings.slkLibrarySha256 = "9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1";
        input.bindings.dataSetIdentity = "原创子集默认集";
        input.bindings.mapInfo = {SlkSessionSourcePresence::Present, std::string(64, '6')};
        input.bindings.strings = {SlkSessionSourcePresence::Missing, {}};
        for (std::size_t i = 0; i < 7; ++i) {
            auto doc = NativeObjectDocument::CreateEmpty(kTypes[i]); Require(doc.Succeeded(), "native fixture creation failed");
            documents[i] = std::move(*doc.value);
            input.standardDefaults[i].push_back(Base(i));
            input.bindings.defaultsSha256[i] = std::string(64, 'a');
        }
    }
    void Bind(bool updateHashes = true) {
        input.schema = &schema; input.strings = strings ? &*strings : nullptr;
        for (std::size_t i = 0; i < 7; ++i) {
            input.nativeDocuments[i] = documents[i] ? &*documents[i] : nullptr;
            if (!updateHashes) continue;
            if (!documents[i]) { input.bindings.nativeMembers[i] = {SlkSessionSourcePresence::Missing, {}}; continue; }
            const auto bytes = NativeObjectCodec(kTypes[i]).Serialize(*documents[i]); Require(bytes.Succeeded(), "native fixture serialize failed");
            input.bindings.nativeMembers[i] = {SlkSessionSourcePresence::Present, Digest(*bytes.value)};
        }
        if (updateHashes && strings) {
            const auto bytes = strings->Serialize(); Require(bytes.Succeeded(), "WTS fixture serialize failed");
            input.bindings.strings = {SlkSessionSourcePresence::Present, Digest(*bytes.value)};
        }
    }
    void Custom(std::size_t i = 1) { Require(documents[i]->AddCustomObject("N001", "P001")); }
    void Add(std::string field, NativeValueType type, ObjectData::Value value, std::uint32_t level = 0, std::uint32_t data = 0) {
        Require(documents[1]->AddModification({ObjectType::Ability, "N001"}, {std::move(field), level, data, {}}, type, std::move(value)));
    }
};
const SlkSessionObject& Object(const SlkNativeFrontendMaterialization& result, std::size_t type, std::string_view id) {
    for (const auto& object : result.session.classes[type].existingObjects) if (object.id == id) return object;
    throw std::runtime_error("materialized object missing");
}
const std::vector<SlkSessionScalar>& Values(const SlkSessionObject& object, const std::string& field) {
    return std::get<std::vector<SlkSessionScalar>>(object.fields.at(field));
}
template<class T> const T& Field(const SlkSessionObject& object, const std::string& field) {
    return std::get<T>(std::get<SlkSessionScalar>(object.fields.at(field)));
}
SlkNativeFrontendMaterialization Built(Fixture& fixture) {
    fixture.Bind();
    auto result = MaterializeSlkNativeFrontend(fixture.input);
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "无诊断" : result.diagnostics.front().message);
    Require(result.value->coverage == SlkNativeFrontendCoverage::ExistingNativeFrontendOnly, "子集覆盖身份错误");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().code == "SLK_MATERIALIZER_SESSION_UNVERIFIED", "子集必须报告完整会话尚未验证");
    for (const auto& type : result.value->session.classes)
        Require(type.materialization == SlkSessionMaterialization::Unverified && type.createdObjects.empty(), "不能给 old 子集自报完整物化");
    Require(!ProveSlkTemplateSessionNoObjectWrites(result.value->session).Succeeded(), "已有对象子集不能绕过会话门禁");
    return std::move(*result.value);
}
void Rejected(Fixture& fixture, std::string_view code, bool rebind = true) {
    fixture.Bind(rebind);
    const auto result = MaterializeSlkNativeFrontend(fixture.input);
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "失败不能返回部分物化图");
    if (result.diagnostics.front().code != code) throw std::runtime_error("期望 " + std::string(code) + "，实际 " + result.diagnostics.front().code);
    Require(result.diagnostics.front().origin.has_value(), "失败必须定位输入来源");
}

void TestDefaultsAndNativeTypes() {
    Fixture fixture;
    for (std::size_t i = 0; i < 7; ++i) fixture.Custom(i);
    fixture.Add("Adat", NativeValueType::Unreal, 0.1f, 0, 1);
    fixture.Add("Flag", NativeValueType::Int32, std::int32_t{0});
    fixture.Add("W2lo", NativeValueType::String, std::string{});
    const auto result = Built(fixture);
    const auto& ability = Object(result, 1, "N001");
    Require(std::get<double>(Values(ability, "dataa")[0]) == static_cast<double>(0.1f), "Float32 必须精确提升为 double");
    Require(Field<std::int64_t>(ability, "flag") == 0, "native bool 字段必须保留 Lua integer 0");
    Require(result.generatedObjectIds[1] == std::vector<std::string>{"N001"}, "空 W2lo 字符串也是 old generated");
    Require(Field<std::string>(ability, "_parent") == "P001" && Field<bool>(ability, "_obj"), "native 内部身份未构造");
    Require(fixture.input.standardDefaults[1][0].fields.at("dataa") == Array({1.0, 2.0}), "物化不能修改输入 default");
    for (std::size_t i = 0; i < 7; ++i) Require(Object(result, i, "N001").id == "N001", "七类 custom 对象必须都构造");
    Fixture absent;
    for (auto& document : absent.documents) document.reset();
    const auto defaults = Built(absent);
    Require(defaults.appliedModifications == 0 && Values(Object(defaults, 1, "P001"), "tip").size() == 2,
        "缺失原生成员仍需对原始默认数组填充");
    Require(!Object(defaults, 1, "P001").fields.contains("_obj"), "缺失成员不能凭空标为原生覆盖");
}

void TestLevelFillOrderAndParentOverlay() {
    Fixture fixture; fixture.Custom();
    fixture.Add("Adat", NativeValueType::Unreal, 9.0f, 2, 1);
    fixture.Add("Atip", NativeValueType::String, std::string("third"), 3);
    fixture.Add("Aapp", NativeValueType::String, std::string("third"), 3);
    fixture.Add("alev", NativeValueType::Int32, std::int32_t{5}); // 故意最后写级数
    Require(fixture.documents[1]->AddOriginalObject("P001"));
    Require(fixture.documents[1]->AddModification({ObjectType::Ability, "P001"}, {"Name", 0, 0, {}}, NativeValueType::String, std::string("原版覆盖名")));
    const auto result = Built(fixture);
    const auto& ability = Object(result, 1, "N001");
    Require(Values(ability, "dataa") == std::vector<SlkSessionScalar>{1.0, 9.0, 2.0, 2.0, 2.0}, "SLK 末级必须回退默认末项");
    Require(Values(ability, "tip") == std::vector<SlkSessionScalar>{std::string("tip1"), std::string{}, std::string("third"), std::string("third"), std::string("third")}, "profile 空洞与末项延续错误");
    Require(Values(ability, "app") == std::vector<SlkSessionScalar>{std::string("app1"), std::string{}, std::string("third"), std::string{}, std::string{}}, "appendIndex 必须填空字符串");
    Require(Field<std::int64_t>(ability, "_max_level") == 5 && Field<std::string>(ability, "name") == "默认名称", "custom 不能继承 original 覆盖后的父字段");
    Require(Field<std::string>(Object(result, 1, "P001"), "name") == "原版覆盖名", "原版覆盖自身必须保留");
    Fixture zero; zero.Custom(); zero.Add("alev", NativeValueType::Int32, std::int32_t{0});
    const auto one = Built(zero);
    Require(Field<std::int64_t>(Object(one, 1, "N001"), "levels") == 0 && Values(Object(one, 1, "N001"), "dataa").size() == 1,
        "等级字段0保留，但_max_level与默认数组须按1处理");
}

void TestOldOnlyWtsAndSplit() {
    Fixture fixture; fixture.Custom();
    auto parsed = MapData::ParseStringTable(Bytes("STRING 7\n{\nresolved\n}\nSTRING 8\n{\ninner\n}\nSTRING 9\n{\nTRIGSTR_8\n}\nSTRING 10\n{\nstatic\n}\n"));
    Require(parsed.Succeeded(), "WTS fixture parse failed"); fixture.strings = std::move(*parsed.value);
    fixture.Add("Name", NativeValueType::String, std::string("preTRIGSTR_7/midTRIGSTR_9end"));
    fixture.Add("W2lo", NativeValueType::String, std::string("TRIGSTR_10"));
    fixture.Add("Aspt", NativeValueType::String, std::string("TRIGSTR_7,tail"));
    const auto result = Built(fixture);
    const auto& ability = Object(result, 1, "N001");
    Require(Field<std::string>(ability, "name") == "preresolved/midTRIGSTR_8end", "old WTS 须嵌入替换且不得递归");
    Require(Field<std::string>(ability, "split") == "resolved", "WTS 必须先于 metadata splite");
    Require(Field<std::string>(ability, "w2lobject") == "static" && result.resolvedStringReferences == 4, "W2lo 同样经过旧端 WTS");
    Require(fixture.strings->entries[2].value == "TRIGSTR_8", "不能改写 WTS");
}

void TestRefusalAndIdentity() {
    Fixture mismatch; mismatch.Custom(); mismatch.Bind(); mismatch.Add("Name", NativeValueType::String, std::string("changed"));
    Rejected(mismatch, "SLK_MATERIALIZER_IDENTITY_MISMATCH", false);
    Fixture wrongData; wrongData.Custom(); wrongData.Add("Adat", NativeValueType::Unreal, 1.0f, 1, 0);
    Rejected(wrongData, "SLK_MATERIALIZER_ADDRESS_UNSUPPORTED");
    Fixture wrongType; wrongType.Custom(); wrongType.Add("Adat", NativeValueType::Real, 1.0f, 1, 1);
    Rejected(wrongType, "SLK_MATERIALIZER_ADDRESS_UNSUPPORTED");
    Fixture duplicate; duplicate.Custom(); duplicate.Add("Adat", NativeValueType::Unreal, 1.0f, 0, 1); duplicate.Add("Adat", NativeValueType::Unreal, 2.0f, 1, 1);
    Rejected(duplicate, "SLK_MATERIALIZER_DUPLICATE_ADDRESS");
    Fixture missing; missing.Custom(); missing.Add("Name", NativeValueType::String, std::string("prefixTRIGSTR_99"));
    Rejected(missing, "SLK_MATERIALIZER_WTS_MISSING");
    Fixture unknown; unknown.Custom(); unknown.Add("ZZZZ", NativeValueType::Int32, std::int32_t{1});
    Rejected(unknown, "SLK_MATERIALIZER_METADATA_UNSUPPORTED");
    Fixture noArray; noArray.Custom(); noArray.input.standardDefaults[1][0].fields.erase("dataa"); noArray.Add("Adat", NativeValueType::Unreal, 1.0f, 1, 1);
    Rejected(noArray, "SLK_MATERIALIZER_DEFAULT_FIELD_UNKNOWN");
    Fixture parent; Require(parent.documents[1]->AddCustomObject("N001", "MISS"));
    Rejected(parent, "SLK_MATERIALIZER_PARENT_UNSUPPORTED");
    Fixture overlay; overlay.input.verifiedNativeOnlySources = false;
    Rejected(overlay, "SLK_MATERIALIZER_SOURCES_UNPROVEN");
    Fixture inconsistent; inconsistent.input.standardDefaults[1][0].fields["_max_level"] = Scalar(std::int64_t{3});
    Rejected(inconsistent, "SLK_MATERIALIZER_DEFAULT_UNSUPPORTED");
    Fixture floatingLevel; floatingLevel.input.standardDefaults[1][0].fields["levels"] = Scalar(2.0);
    Rejected(floatingLevel, "SLK_MATERIALIZER_LEVEL_UNSUPPORTED");
    Fixture section; section.schema = Schema("section");
    Rejected(section, "SLK_MATERIALIZER_METADATA_UNSUPPORTED");
    Fixture alias; alias.schema = Schema("useSpecific");
    alias.input.standardDefaults[1][0].fields["_code"] = String("ALIA");
    Rejected(alias, "SLK_MATERIALIZER_METADATA_UNSUPPORTED");
    Fixture doodad; doodad.Custom(5);
    Require(doodad.documents[5]->AddModification({ObjectType::Doodad, "N001"}, {"dnum", {}, 0, 0}, NativeValueType::Real, 2.0f));
    Rejected(doodad, "SLK_MATERIALIZER_VARIATION_UNPROVEN");
}

void TestBudgetsBeforeExpandedAllocations() {
    Fixture arrays;
    arrays.schema = Schema({}, 64);
    for (auto& defaults : arrays.input.standardDefaults) defaults.clear();
    arrays.input.standardDefaults[1].push_back(Base(1));
    for (std::size_t i = 0; i < 64; ++i)
        arrays.input.standardDefaults[1][0].fields["extra" + std::to_string(i)] = Array({});
    arrays.input.limits.values = 200;
    Rejected(arrays, "SLK_MATERIALIZER_LIMIT"); // 空数组、字段和 metadata 副本也必须计入。
    arrays.input.limits = {};
    const auto expanded = Built(arrays);
    Require(Values(Object(expanded, 1, "P001"), "extra63").empty(), "正常预算下空默认数组须保持为空");

    Fixture entries;
    auto emptyWts = MapData::ParseStringTable(Bytes("STRING 1\n{\n\n}\nSTRING 2\n{\n\n}\n"));
    Require(emptyWts.Succeeded(), "empty WTS fixture parse failed");
    entries.strings = std::move(*emptyWts.value);
    entries.input.limits.wtsEntries = 1;
    Rejected(entries, "SLK_MATERIALIZER_LIMIT"); // 两个空值也在索引前触发条目限制。
    entries.input.limits = {};
    (void)Built(entries);

    Fixture strings;
    auto textWts = MapData::ParseStringTable(Bytes("STRING 1\n{\naaaa\n}\nSTRING 2\n{\nbbbb\n}\n"));
    Require(textWts.Succeeded(), "text WTS fixture parse failed");
    strings.strings = std::move(*textWts.value);
    strings.input.limits.stringBytes = 7;
    Rejected(strings, "SLK_MATERIALIZER_LIMIT");

    Fixture raised;
    ++raised.input.limits.values;
    Rejected(raised, "SLK_MATERIALIZER_LIMIT");
    raised.input.limits = {};
    ++raised.input.limits.wtsEntries;
    Rejected(raised, "SLK_MATERIALIZER_LIMIT");
}

std::vector<std::byte> SelectionW3i(bool melee = false) {
    auto parsed = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(parsed.Succeeded(), "selection fixture failed");
    parsed.value->gameDataSet = melee ? 2 : 1; parsed.value->flags = 0;
    auto bytes = parsed.value->Serialize(); Require(bytes.Succeeded(), "selection serialize failed");
    return std::move(*bytes.value);
}
std::string Meta(std::string_view name, std::string_view id, int type, std::string_view extra = {}) {
    return "[." + std::string(name) + "]\nfield=" + std::string(name) + "\nid=" + std::string(id) +
        "\nkey=" + std::string(name) + "\ntype=" + std::to_string(type) + "\n" + std::string(extra);
}
SlkTypedDataset OriginalCanonicalDataset() {
    const auto core = std::filesystem::current_path() / "original-canonical-core";
    const auto installed = std::filesystem::current_path() / "original-canonical-installed";
    std::map<std::filesystem::path, std::vector<std::byte>> files;
    files[core / "config.ini"] = Bytes("[global]\ndata=original\n");
    std::string metadata;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        metadata += "[" + std::string(kNames[i]) + "]\n" + Meta("name", "Name", 3) + Meta("flag", "Flag", 0) + Meta("w2lobject", "W2lo", 3);
        std::string text = "[P001]\n_id=\"P001\"\n_type=\"" + std::string(kNames[i]) + "\"\nname=\"TRIGSTR_7\"\nflag=1\n";
        if (i == 0) text += "_name=\"original-unit-label\"\n";
        if (i == 1) {
            text += "_code=\"P001\"\n_max_level=2\nlevels=2\ndataa={1.0,2.0}\ntip={\"first\"}\napp={\"first\"}\nsplit=\"default\"\n";
            metadata += Meta("levels", "alev", 0) + Meta("dataa", "Dcom", 2, "data=1\nrepeat=4\n") +
                Meta("tip", "Atip", 3, "repeat=0\nprofile=true\n") + Meta("app", "Aapp", 3, "repeat=4\nappendindex=true\nprofile=true\n") +
                Meta("split", "S", 3, "splite=true\n");
        }
        if (i == 4) { text += "_max_level=1\nmaxlevel=1\n"; metadata += Meta("maxlevel", "glvl", 0); }
        if (i == 5) {
            text += "_max_level=1\nnumvar=1\nnumber={1.0}\n";
            metadata += Meta("numvar", "dvar", 0) + Meta("number", "Cr", 1, "repeat=4\n");
        }
        files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom" / (std::string(kNames[i]) + ".ini")] = Bytes(text);
    }
    // 同native ID已被common消费；不同ID的同canonical名由byCode第二轮覆盖。
    metadata += "[P001]\n" + Meta("consumed", "Dcom", 2, "data=1\nrepeat=4\n") +
        Meta("dataa", "Dspe", 2, "data=1\nrepeat=4\n") + Meta("tip", "Talt", 3, "repeat=4\nprofile=false\n") +
        Meta("name", "Bnme", 3);
    files[core / "defined/metadata.ini"] = Bytes(metadata);
    files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom/txt.ini"] = {};
    files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom/misc.ini"] = {};
    const auto w3i = SelectionW3i();
    auto result = ReadSlkTypedDataset({core, installed, w3i, 31}, [&](const auto& path, std::size_t maximum) {
        const auto at = files.find(path);
        Require(at != files.end() && at->second.size() <= maximum, "canonical fixture requested wrong source");
        return Result<std::vector<std::byte>>::Success(at->second);
    });
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}
void BindDataset(SlkNativeFrontendInput& input, const SlkTypedDataset& dataset) {
    input.canonicalDataset = &dataset; input.schema = nullptr;
    input.bindings.dataSetIdentity = dataset.manifestSha256;
    input.bindings.documentRevision = dataset.documentRevision;
    input.bindings.mapInfo = {SlkSessionSourcePresence::Present, dataset.w3iSha256};
    for (const auto& source : dataset.sources) {
        if (source.role == "canonical-metadata") input.bindings.metadataSha256 = source.sha256;
        for (std::size_t i = 0; i < kTypes.size(); ++i)
            if (source.role == "prebuilt-" + std::string(kNames[i])) input.bindings.defaultsSha256[i] = source.sha256;
    }
}
struct CanonicalFixture {
    Fixture native;
    SlkTypedDataset dataset = OriginalCanonicalDataset();
    CanonicalFixture() { for (auto& objects : native.input.standardDefaults) objects.clear(); }
    void Bind() { native.Bind(); BindDataset(native.input, dataset); }
};
void CheckUnverified(const SlkNativeFrontendMaterialization& result) {
    Require(result.coverage == SlkNativeFrontendCoverage::ExistingNativeFrontendOnly && !result.session.completeStaticCreationSession,
        "canonical old changed coverage");
    for (const auto& type : result.session.classes)
        Require(type.materialization == SlkSessionMaterialization::Unverified && type.createdObjects.empty() && type.metadata.empty(),
            "canonical per-baseCode metadata cannot be flattened or reported complete");
    Require(!ProveSlkTemplateSessionNoObjectWrites(result.session).Succeeded(), "canonical old bypassed production proof gate");
}
SlkNativeFrontendMaterialization Built(CanonicalFixture& fixture) {
    fixture.Bind(); auto result = MaterializeSlkNativeFrontend(fixture.native.input);
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    CheckUnverified(*result.value);
    Require(result.value->canonicalDatasetManifest == fixture.dataset.manifestSha256, "canonical manifest lost");
    return std::move(*result.value);
}
void RejectInput(const SlkNativeFrontendInput& input, std::string_view code) {
    const auto result = MaterializeSlkNativeFrontend(input);
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "canonical failure returned partial graph");
    if (result.diagnostics.front().code != code)
        throw std::runtime_error("expected " + std::string(code) + ", actual " + result.diagnostics.front().code);
    Require(result.diagnostics.front().origin.has_value(), "canonical failure lost origin");
}
void AddPaddedNative(Fixture& fixture, std::size_t type, std::string rawId, NativeValueType storage, ObjectData::Value value,
    std::uint32_t level = 0, std::uint32_t data = 0) {
    Require(rawId.size() == 4, "fixture raw native ID needs four bytes");
    // UI新增字段API有意只接受普通4字节ID；本夹具通过真实codec解析原创短ID原始字节。
    const ModificationKey key{"Ztmp", type == 5 ? std::optional<std::uint32_t>{} : level, data,
        type == 5 ? std::optional<std::uint32_t>{level} : std::nullopt};
    Require(fixture.documents[type]->AddModification({kTypes[type], "N001"}, key, storage, std::move(value)));
    const NativeObjectCodec codec(kTypes[type]);
    auto bytes = codec.Serialize(*fixture.documents[type]); Require(bytes.Succeeded(), "short-ID serialize failed");
    const auto marker = Bytes("Ztmp");
    const auto at = std::search(bytes.value->begin(), bytes.value->end(), marker.begin(), marker.end());
    Require(at != bytes.value->end(), "short-ID placeholder missing");
    for (std::size_t i = 0; i < 4; ++i) at[static_cast<std::ptrdiff_t>(i)] = static_cast<std::byte>(rawId[i]);
    auto parsed = codec.Parse(*bytes.value); Require(parsed.Succeeded(), "short-ID native parse failed");
    auto* document = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
    Require(document && document->IsEditable(), "short-ID fixture is not editable");
    fixture.documents[type] = std::move(*document);
}
void TestCanonicalOldFrontend() {
    CanonicalFixture fixture;
    fixture.native.Custom(); fixture.native.Custom(0); fixture.native.Custom(5);
    fixture.native.Add("Dcom", NativeValueType::Unreal, 11.0f, 2, 1);
    fixture.native.Add("Dspe", NativeValueType::Unreal, 99.0f, 2, 1);
    fixture.native.Add("Atip", NativeValueType::String, std::string("third"), 3);
    fixture.native.Add("Aapp", NativeValueType::String, std::string("third"), 3);
    fixture.native.Add("alev", NativeValueType::Int32, std::int32_t{5});
    fixture.native.Add("Name", NativeValueType::String, std::string("preTRIGSTR_7"));
    fixture.native.Add("Bnme", NativeValueType::String, std::string("TRIGSTR_9"));
    AddPaddedNative(fixture.native, 1, std::string("S\0\0\0", 4), NativeValueType::String, std::string("TRIGSTR_7,tail"));
    fixture.native.Add("W2lo", NativeValueType::String, std::string{});
    auto strings = MapData::ParseStringTable(Bytes("STRING 7\n{\nresolved\n}\nSTRING 9\n{\nTRIGSTR_7\n}\n"));
    Require(strings.Succeeded(), "canonical WTS fixture failed"); fixture.native.strings = std::move(*strings.value);
    Require(fixture.native.documents[1]->AddOriginalObject("P001"));
    Require(fixture.native.documents[1]->AddModification({ObjectType::Ability, "P001"}, {"Name", 0, 0, {}}, NativeValueType::String, std::string("parent-edit")));
    AddPaddedNative(fixture.native, 5, std::string("Cr\0\0", 4), NativeValueType::Real, 2.0f);
    const auto result = Built(fixture);
    const auto& ability = Object(result, 1, "N001");
    Require(Values(ability, "dataa") == std::vector<SlkSessionScalar>{1.0, 99.0, 2.0, 2.0, 2.0} && !ability.fields.contains("consumed"),
        "common/byCode must consume same rawID once and later different rawID must override");
    Require(Values(ability, "tip") == std::vector<SlkSessionScalar>{std::string("first"), std::string("first"), std::string("third"), std::string("first"), std::string("first")},
        "fill must prefer byCode metadata, repeat=0 is truthy");
    Require(Values(ability, "app") == std::vector<SlkSessionScalar>{std::string("first"), std::string{}, std::string("third"), std::string{}, std::string{}},
        "canonical appendindex fill differs");
    Require(Field<std::string>(ability, "name") == "TRIGSTR_7" && Field<std::string>(ability, "split") == "resolved", "WTS must run once before split");
    Require(Field<std::string>(Object(result, 0, "N001"), "name") == "TRIGSTR_7", "raw default must never use old WTS substitution");
    Require(Field<std::string>(Object(result, 1, "P001"), "name") == "parent-edit" &&
        Field<std::string>(result.session.classes[1].standardDefaults[0], "name") == "TRIGSTR_7", "original override contaminated standard parent");
    Require(!Object(result, 0, "N001").fields.contains("_code") && !Object(result, 5, "N001").fields.contains("_code"), "missing base code fabricated");
    Require(Values(Object(result, 5, "N001"), "number") == std::vector<SlkSessionScalar>{2.0}, "doodad variation0 or short ID failed");
    Require(result.generatedObjectIds[1] == std::vector<std::string>{"N001"}, "empty W2lo marker must remain generated");
}
void TestCanonicalRefusalsAndShortIds() {
    for (const std::string id : {"X", "XY", "XYZ", "WXYZ"}) {
        CanonicalFixture fixture; fixture.native.Custom(5);
        fixture.dataset.metadata.common[5].fields.at("number").fields.at("id").value = String(id);
        std::string nativeId = id; nativeId.resize(4, '\0');
        AddPaddedNative(fixture.native, 5, nativeId, NativeValueType::Real, 4.0f, 1);
        Require(Values(Object(Built(fixture), 5, "N001"), "number") == std::vector<SlkSessionScalar>{4.0}, "1-4 byte IDs must retain exact identity");
    }
    CanonicalFixture missingCode; missingCode.native.Custom(0);
    Require(missingCode.native.documents[0]->AddModification({ObjectType::Unit, "N001"}, {"Bnme", {}, {}, {}}, NativeValueType::String, std::string("unknown")));
    missingCode.Bind(); RejectInput(missingCode.native.input, "SLK_MATERIALIZER_METADATA_UNSUPPORTED");
    CanonicalFixture ambiguous; ambiguous.native.Custom(); ambiguous.native.Add("Name", NativeValueType::String, std::string("name"));
    ambiguous.dataset.metadata.common[1].fields["alias"] = ambiguous.dataset.metadata.common[1].fields.at("name");
    ambiguous.dataset.metadata.common[1].fields.at("alias").fields.at("field").value = String("alias");
    ambiguous.Bind(); RejectInput(ambiguous.native.input, "SLK_MATERIALIZER_METADATA_AMBIGUOUS");
    CanonicalFixture shortGarbage; shortGarbage.native.Custom();
    AddPaddedNative(shortGarbage.native, 1, std::string("S\0x\0", 4), NativeValueType::String, std::string("bad"));
    shortGarbage.Bind(); RejectInput(shortGarbage.native.input, "SLK_MATERIALIZER_ADDRESS_UNSUPPORTED");
    CanonicalFixture duplicate; duplicate.native.Custom(); duplicate.native.Add("Dcom", NativeValueType::Unreal, 1.0f, 0, 1);
    duplicate.native.Add("Dcom", NativeValueType::Unreal, 2.0f, 1, 2);
    duplicate.Bind(); RejectInput(duplicate.native.input, "SLK_MATERIALIZER_DUPLICATE_ADDRESS");
    CanonicalFixture identity; identity.Bind(); identity.native.input.bindings.defaultsSha256[0].clear();
    RejectInput(identity.native.input, "SLK_MATERIALIZER_IDENTITY_MISMATCH");
    identity.Bind(); ++identity.native.input.bindings.documentRevision;
    RejectInput(identity.native.input, "SLK_MATERIALIZER_IDENTITY_MISMATCH");
    identity.Bind(); identity.native.input.schema = &identity.native.schema;
    RejectInput(identity.native.input, "SLK_MATERIALIZER_SOURCES_UNPROVEN");
    CanonicalFixture budget; budget.native.input.limits.values = 20;
    budget.Bind(); RejectInput(budget.native.input, "SLK_MATERIALIZER_LIMIT");
    CanonicalFixture emptyWts; emptyWts.Bind(); MapData::StringTableDocument empty;
    emptyWts.native.input.strings = &empty;
    RejectInput(emptyWts.native.input, "SLK_MATERIALIZER_IDENTITY_MISMATCH");
}
int ProbeCanonical(int argc, char** argv) {
    Require(argc == 5 && (std::string_view(argv[4]) == "Custom" || std::string_view(argv[4]) == "Melee"),
        "usage: --probe <core-root> <installed-ydwe-root> Custom|Melee");
    const auto path = [](std::string_view value) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    };
    const auto w3i = SelectionW3i(std::string_view(argv[4]) == "Melee");
    const auto dataset = ReadSlkTypedDataset({path(argv[2]), path(argv[3]), w3i, 0});
    if (!dataset.Succeeded()) throw std::runtime_error(dataset.diagnostics.front().code + ": " + dataset.diagnostics.front().message);
    SlkNativeFrontendInput input;
    // 专用原创W3I、零native/零WTS场景；不是对任何地图来源完整性的声明。
    input.verifiedNativeOnlySources = true; input.bindings.candidateSha256 = Digest(w3i);
    input.bindings.strings.presence = SlkSessionSourcePresence::Missing;
    for (auto& identity : input.bindings.nativeMembers) identity.presence = SlkSessionSourcePresence::Missing;
    BindDataset(input, *dataset.value);
    const auto result = MaterializeSlkNativeFrontend(input);
    if (!result.Succeeded()) {
        const auto& error = result.diagnostics.front();
        std::cerr << error.code << ": " << error.message;
        if (error.origin) std::cerr << " offset=" << error.origin->offset;
        std::cerr << '\n'; return 1;
    }
    CheckUnverified(*result.value);
    Require(result.value->appliedModifications == 0 && result.value->resolvedStringReferences == 0, "zero-native probe changed source");
    std::cout << "coverage=ExistingNativeFrontendOnly manifest=" << result.value->canonicalDatasetManifest << '\n';
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        const auto& type = result.value->session.classes[i];
        Require(type.standardDefaults.size() == dataset.value->defaults[i].size() && type.existingObjects.size() == type.standardDefaults.size() &&
            result.value->generatedObjectIds[i].empty(), "zero-native count differs");
        std::cout << kNames[i] << " defaults=" << type.standardDefaults.size() << " existing=" << type.existingObjects.size() << " state=Unverified\n";
    }
    std::cout << "production_template_gate=unchanged; no map read/write; no Lua execution\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--probe") return ProbeCanonical(argc, argv);
        Require(argc == 1, "unsupported arguments");
        TestDefaultsAndNativeTypes();
        TestLevelFillOrderAndParentOverlay();
        TestOldOnlyWtsAndSplit();
        TestRefusalAndIdentity();
        TestBudgetsBeforeExpandedAllocations();
        TestCanonicalOldFrontend();
        TestCanonicalRefusalsAndShortIds();
        std::cout << "SlkTemplateMaterializer tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
