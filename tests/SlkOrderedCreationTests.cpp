#include "War3MapCore/Script/SlkOrderedCreation.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "MapDataFixtures.h"

#include <iostream>
#include <stdexcept>

// 全部来源为原创内存夹具。默认测试不读取YDWE/地图，不加载或执行Lua。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
constexpr std::string_view kNames[]{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::string_view kLibrary = "9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1";
void Require(bool condition, std::string_view message) { if (!condition) throw std::runtime_error(std::string(message)); }
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char c : text) result.push_back(static_cast<std::byte>(c));
    return result;
}
std::string Meta(std::string_view name, std::string_view id, int type, std::string_view properties = {}) {
    return "[." + std::string(name) + "]\nfield=" + std::string(name) + "\nid=" + std::string(id) +
        "\nkey=" + std::string(name) + "\ntype=" + std::to_string(type) + "\n" + std::string(properties);
}
SlkTypedDataset Dataset() {
    const auto core = std::filesystem::current_path() / "ordered-fixture-core";
    const auto installed = std::filesystem::current_path() / "ordered-fixture-install";
    std::map<std::filesystem::path, std::vector<std::byte>> files;
    files[core / "config.ini"] = Bytes("[global]\ndata=original\n");
    std::string metadata;
    for (std::size_t i = 0; i < std::size(kNames); ++i) {
        const std::string type(kNames[i]);
        metadata += "[" + type + "]\n" + Meta("name", "Name", 3) + Meta("w2lobject", "W2lo", 3);
        std::string data = "[P001]\n_id=\"P001\"\n_type=\"" + type + "\"\nname=\"default\"\n";
        if (i == 1) {
            data += "_code=\"B001\"\nlevels=3\n_max_level=3\ndataa={1.0}\ntip={\"base\",\"second\"}\napp={\"app\"}\npair_1=0\npair_2=0\nenabled=1\nonly=0.0\n";
            metadata += Meta("levels", "alev", 0) + Meta("dataa", "Dcom", 2, "repeat=4\n") +
                Meta("tip", "Atip", 3, "repeat=0\nprofile=true\n") + Meta("app", "Aapp", 3, "repeat=4\nprofile=true\nappendindex=true\n") +
                Meta("pair_1", "Pa01", 0, "index=1\n") + Meta("pair_2", "Pa02", 0, "index=2\n") + Meta("enabled", "Aena", 0);
        }
        if (i == 4) { data += "maxlevel=1\n_max_level=1\n"; metadata += Meta("maxlevel", "glvl", 0); }
        if (i == 5) { data += "numvar=1\n_max_level=1\n"; metadata += Meta("numvar", "dvar", 0); }
        if (i == 1) {
            std::string sibling = data;
            for (std::size_t at = 0; (at = sibling.find("P001", at)) != std::string::npos; at += 4) sibling.replace(at, 4, "P002");
            data += sibling;
        }
        files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom" / (type + ".ini")] = Bytes(data);
    }
    metadata += "[B001]\n" + Meta("dataa", "Dspe", 2, "repeat=4\nprofile=true\n") + Meta("only", "Aone", 2) +
        Meta("tip", "Asca", 3); // common repeat必须优先于specific标量。
    files[core / "defined/metadata.ini"] = Bytes(metadata);
    files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom/txt.ini"] = {};
    files[installed / "plugin/w3x2lni/data/original/prebuilt/Custom/misc.ini"] = {};
    auto map = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(map.Succeeded(), "W3I fixture parse failed"); map.value->flags = 0; map.value->gameDataSet = 1;
    const auto bytes = map.value->Serialize(); Require(bytes.Succeeded(), "W3I fixture serialize failed");
    auto result = ReadSlkTypedDataset({core, installed, *bytes.value, 47}, [&](const auto& path, std::size_t maximum) {
        const auto found = files.find(path); Require(found != files.end() && found->second.size() <= maximum, "wrong original source requested");
        return Result<std::vector<std::byte>>::Success(found->second);
    });
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}
SlkTemplatePlan Plan(std::string_view writes, std::string_view id = "Q001") {
    const auto code = "local s=require 'slk' local o=s.ability.P001:new('" + std::string(id) + "') " + std::string(writes);
    auto result = ParseSlkTemplatePlan(code);
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().message);
    return std::move(*result.value);
}
struct Fixture {
    SlkTypedDataset dataset = Dataset();
    SlkNativeFrontendMaterialization old;
    std::vector<SlkTemplatePlan> plans;
    SlkOrderedCreationLimits limits;
    Fixture() {
        SlkNativeFrontendInput input;
        input.canonicalDataset = &dataset; input.verifiedNativeOnlySources = true;
        input.bindings.candidateSha256 = std::string(64, '1'); input.bindings.orderedTemplatesSha256 = std::string(64, '2');
        input.bindings.slkLibrarySha256 = kLibrary; input.bindings.dataSetIdentity = dataset.manifestSha256;
        input.bindings.documentRevision = dataset.documentRevision;
        input.bindings.mapInfo = {SlkSessionSourcePresence::Present, dataset.w3iSha256};
        input.bindings.strings.presence = SlkSessionSourcePresence::Missing;
        for (auto& member : input.bindings.nativeMembers) member.presence = SlkSessionSourcePresence::Missing;
        for (const auto& source : dataset.sources) {
            if (source.role == "canonical-metadata") input.bindings.metadataSha256 = source.sha256;
            for (std::size_t i = 0; i < std::size(kNames); ++i)
                if (source.role == "prebuilt-" + std::string(kNames[i])) input.bindings.defaultsSha256[i] = source.sha256;
        }
        auto result = MaterializeSlkNativeFrontend(input);
        if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().code + ": " + result.diagnostics.front().message);
        old = std::move(*result.value);
    }
    SlkOrderedCreationInput Input() const { return {&dataset, &old, plans, limits}; }
};
const std::vector<SlkSessionScalar>& Array(const SlkSessionObject& object, std::string_view field) {
    return std::get<std::vector<SlkSessionScalar>>(object.fields.at(std::string(field)));
}
template<class T> const T& Value(const SlkSessionObject& object, std::string_view field) {
    return std::get<T>(std::get<SlkSessionScalar>(object.fields.at(std::string(field))));
}
SlkOrderedCreation Good(const Fixture& fixture) {
    auto result = MaterializeSlkOrderedAbilityCreation(fixture.Input());
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    Require(result.value->coverage == SlkOrderedCreationCoverage::OrderedAbilityCreationOnly && result.value->bindings == fixture.old.session.bindings,
        "ordered subset changed bindings or coverage");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().code == "SLK_CREATION_SESSION_UNVERIFIED" && !result.value->unresolvedSemantics.empty(),
        "ordered creation must report incomplete session");
    auto session = fixture.old.session;
    session.classes[1].createdObjects = result.value->createdAbilities;
    Require(!ProveSlkTemplateSessionNoObjectWrites(session).Succeeded(), "joining old/new must not silently grant Complete");
    return std::move(*result.value);
}
void Bad(const Fixture& fixture, std::string_view code) {
    const auto result = MaterializeSlkOrderedAbilityCreation(fixture.Input());
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty() && result.diagnostics.front().origin.has_value(), "failure returned partial new or lost origin");
    if (result.diagnostics.front().code != code)
        throw std::runtime_error("expected " + std::string(code) + ", actual " + result.diagnostics.front().code);
}
void TypesAndDefaults() {
    Fixture fixture;
    fixture.plans.push_back(Plan("o.DataA1=2^0 o.DataA2=7 o.DataA3=true o.Tip='changed' o.Enabled=false o.Name='TRIGSTR_7' o.Pair=8 o.Pair_2=9 o.Only=0.1"));
    fixture.old.session.classes[1].existingObjects[0].fields["name"] = SlkSessionScalar{std::string("map-parent-edit")};
    const auto result = Good(fixture); const auto& object = result.createdAbilities.at(0);
    Require(object.id == "Q001" && Value<std::string>(object, "_parent") == "P001" && Value<std::string>(object, "_code") == "B001" &&
        Value<bool>(object, "_obj") && Value<bool>(object, "_create") && Value<std::string>(object, "w2lobject") == "static", "new internal identity differs");
    const auto& values = Array(object, "dataa");
    Require(std::holds_alternative<double>(values[0]) && std::get<double>(values[0]) == 1.0 && std::get<std::int64_t>(values[1]) == 7 && std::get<bool>(values[2]),
        "writes must retain Lua numeric/bool subtype");
    Require(!Value<bool>(object, "enabled") && Value<std::string>(object, "name") == "TRIGSTR_7", "new must not coerce bool or resolve WTS");
    Require(std::get<std::string>(Array(object, "tip")[0]) == "changed", "common repeat must win over byCode scalar metadata");
    Require(Value<std::int64_t>(object, "pair_1") == 8 && Value<std::int64_t>(object, "pair_2") == 9 && Value<double>(object, "only") == 0.1,
        "indexed scalar or byCode field resolution differs");
    Require(Array(object, "app") == std::vector<SlkSessionScalar>{std::string("app"), std::string("app"), std::string("app")},
        "new fill must not use old appendindex-empty rule");
    Require(result.appliedWrites == 9 && Value<std::string>(fixture.old.session.classes[1].existingObjects[0], "name") == "map-parent-edit", "creation modified old input");
    Fixture untouched; untouched.plans.push_back(Plan(""));
    untouched.old.session.classes[1].existingObjects[0].fields["name"] = SlkSessionScalar{std::string("map-parent-edit")};
    Require(Value<std::string>(Good(untouched).createdAbilities[0], "name") == "default", "new inherited modified parent instead of standard default");
}
void OrderedFill() {
    Fixture first; first.plans.push_back(Plan("o.Levels=1 o.DataA1=9.0 o.Tip='custom' o.Levels=4"));
    const auto a = Good(first);
    Require(Array(a.createdAbilities[0], "dataa") == std::vector<SlkSessionScalar>{9.0, 1.0, 1.0, 1.0}, "common fill must run before byCode profile");
    Require(Array(a.createdAbilities[0], "tip") == std::vector<SlkSessionScalar>{std::string("custom"), std::string("second"), std::string("second"), std::string("second")}, "profile should prefer default[i] then previous value");
    Fixture shrink; shrink.plans.push_back(Plan("o.Tip2='custom' o.Levels=2 o.Levels=4"));
    Fixture grow; grow.plans.push_back(Plan("o.Tip2='custom' o.Levels=4"));
    Require(std::get<std::string>(Array(Good(shrink).createdAbilities[0], "tip")[3]) == "custom" &&
        std::get<std::string>(Array(Good(grow).createdAbilities[0], "tip")[3]) == "second", "level writes were collapsed instead of executed in order");
    Fixture zero; zero.plans.push_back(Plan("o.Levels=0"));
    const auto z = Good(zero);
    Require(Value<std::int64_t>(z.createdAbilities[0], "_max_level") == 0 && Array(z.createdAbilities[0], "dataa").empty(), "new zero level must remain zero and clear repeat arrays");
    Fixture beyond; beyond.plans.push_back(Plan("o.DataA4=8 o.Levels=2 o.Levels=4"));
    Require(Array(Good(beyond).createdAbilities[0], "dataa") == std::vector<SlkSessionScalar>{1.0, 1.0, 1.0, 1.0}, "trimmed level should not resurrect on later growth");
    Fixture falseDefault;
    falseDefault.dataset.defaults[1].at("P001").fields.at("tip").value = std::vector<SlkSessionScalar>{std::string("first"), false};
    falseDefault.plans.push_back(Plan("o.Levels=1 o.Levels=3"));
    Require(Array(Good(falseDefault).createdAbilities[0], "tip") == std::vector<SlkSessionScalar>{std::string("first"), std::string("first"), std::string("first")}, "Lua false default must select profile fallback");
}
void ConflictsAndRefusals() {
    Fixture generated;
    auto oldObject = generated.old.session.classes[1].existingObjects[0]; oldObject.id = "Q001";
    oldObject.fields["_id"] = SlkSessionScalar{std::string("Q001")}; oldObject.fields["w2lobject"] = SlkSessionScalar{std::string{}};
    generated.old.session.classes[1].existingObjects.push_back(oldObject);
    generated.plans.push_back(Plan("")); Require(Good(generated).createdAbilities.size() == 1, "empty old W2lo should be removed before static new");
    generated.old.session.classes[1].existingObjects.back().fields.erase("w2lobject"); Bad(generated, "SLK_CREATION_ID_CONFLICT");
    Fixture duplicate; duplicate.plans.push_back(Plan("")); duplicate.plans.push_back(Plan("", "q001")); Bad(duplicate, "SLK_CREATION_ID_CONFLICT");
    Fixture standard; standard.plans.push_back(Plan("", "P002")); Bad(standard, "SLK_CREATION_ID_CONFLICT");
    for (const auto writes : {"o.Missing=1", "o._id='Q002'", "o.W2LObject='static'", "o.DataA1junk=3", "o.DataA0=1"}) {
        Fixture fixture; fixture.plans.push_back(Plan(writes)); Bad(fixture, "SLK_CREATION_FIELD_UNSUPPORTED");
    }
    Fixture sparse; sparse.plans.push_back(Plan("o.DataA5=1")); Bad(sparse, "SLK_CREATION_SPARSE_UNSUPPORTED");
    Fixture realLevel; realLevel.plans.push_back(Plan("o.Levels=2.0")); Bad(realLevel, "SLK_CREATION_LEVEL_UNSUPPORTED");
    Fixture longString; longString.plans.push_back(Plan("o.Name='" + std::string(1024, 'x') + "'")); Bad(longString, "SLK_CREATION_STRING_UNSUPPORTED");
    Fixture parent; parent.plans.push_back(Plan("")); parent.plans[0].abilities[0].parentId = "MISS"; Bad(parent, "SLK_CREATION_PARENT_UNSUPPORTED");
    Fixture identity; identity.plans.push_back(Plan("")); identity.old.session.bindings.slkLibrarySha256.assign(64, '1'); Bad(identity, "SLK_CREATION_IDENTITY_MISMATCH");
    Fixture budget; budget.plans.push_back(Plan("o.Levels=4")); budget.limits.values = 20; Bad(budget, "SLK_CREATION_LIMIT");
    budget.limits = {}; budget.limits.writes = 0; Bad(budget, "SLK_CREATION_LIMIT");
    budget.limits = {}; ++budget.limits.values; Bad(budget, "SLK_CREATION_LIMIT");
}
void IndexedGapPriority() {
    Fixture fixture;
    const auto indexField = [&](std::size_t index) {
        const auto name = "dataa_" + std::to_string(index);
        auto record = fixture.dataset.metadata.common[1].fields.at("pair_1");
        record.fields.at("field").value = SlkSessionScalar{name};
        record.fields.at("key").value = SlkSessionScalar{name};
        record.fields.at("id").value = SlkSessionScalar{"Dx0" + std::to_string(index)};
        record.fields.at("index").value = SlkSessionScalar{static_cast<std::int64_t>(index)};
        fixture.dataset.metadata.common[1].fields.emplace(name, std::move(record));
        fixture.dataset.defaults[1].at("P001").fields.emplace(name, SlkDatasetValue{SlkSessionScalar{std::int64_t{0}}, {}});
    };
    indexField(1); indexField(3);
    fixture.plans.push_back(Plan("o.DataA_3=7"));
    Bad(fixture, "SLK_CREATION_FIELD_UNSUPPORTED");
    indexField(2);
    Require(Value<std::int64_t>(Good(fixture).createdAbilities[0], "dataa_3") == 7, "continuous indexed path should select exact field before bare repeat");
    fixture.dataset.metadata.common[1].fields.erase("dataa_2");
    fixture.dataset.metadata.common[1].fields.at("dataa").fields.erase("repeat");
    Require(Value<std::int64_t>(Good(fixture).createdAbilities[0], "dataa_3") == 7, "without bare repeat, missing indexed middle should fall back to exact canonical key");
}
} // namespace
int main() {
    try {
        TypesAndDefaults(); OrderedFill(); ConflictsAndRefusals(); IndexedGapPriority();
        std::cout << "SlkOrderedCreation tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
