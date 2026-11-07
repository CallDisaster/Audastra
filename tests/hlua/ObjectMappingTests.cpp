#include "War3MapCore/HLua/ObjectMapping.h"
#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Text/LniData.h"
#include "War3MapCore/Services/LuaObjectSnapshot.h"
#include "War3MapCore/Script/LuaTemplateSandbox.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::HLua;
void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Good(const T& result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
    throw std::runtime_error(message);
}
std::vector<std::byte> Bytes(std::string_view value) { return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())}; }
using Row = std::map<std::string, std::string>;
std::string Slk(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row)
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    std::string text = "ID;PWXL\n";
    for (std::size_t y = 0; y <= rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        auto value = y == 0 ? columns[x] : (rows[y - 1].contains(columns[x]) ? rows[y - 1].at(columns[x]) : "");
        text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 1) + ";K\"" + value + "\"\n";
    }
    return text + "E\n";
}
}
void TestHLuaObjectMapping() {
    std::map<std::string, std::string> files;
    files["unit-meta.slk"] = Slk({
        {{"ID","unam"},{"field","Name"},{"slk","Profile"},{"type","string"},{"index","0"}},
        {{"ID","ussc"},{"field","scale"},{"slk","UnitUI"},{"type","real"}},
        {{"ID","ubpx"},{"field","Buttonpos"},{"slk","Profile"},{"type","int"},{"index","0"}},
        {{"ID","ubpy"},{"field","Buttonpos"},{"slk","Profile"},{"type","int"},{"index","1"}},
        {{"ID","uxf1"},{"field","Fx"},{"slk","Profile"},{"type","string"},{"index","0"}},
        {{"ID","uxf2"},{"field","Fx"},{"slk","Profile"},{"type","string"},{"index","1"}},
        {{"ID","ucr1"},{"field","Curve"},{"slk","Profile"},{"type","real"},{"index","0"}},
        {{"ID","ucr2"},{"field","Curve"},{"slk","Profile"},{"type","real"},{"index","1"}}});
    files["ability-meta.slk"] = Slk({
        {{"ID","atp1"},{"field","Tip"},{"slk","Profile"},{"type","string"},{"repeat","4"},{"index","0"}},
        {{"ID","Hbz1"},{"field","Data"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"},{"data","1"},{"useSpecific","AHbz"}},
        {{"ID","acdn"},{"field","Cool"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"}}});
    files["units.slk"] = Slk({{{"ID","hfoo"},{"code","hfoo"},{"Name","Parent"}},
        {{"ID","hbar"},{"code","hbar"},{"Name","Missing components"}}});
    files["units.txt"] = "[hfoo]\nFx=,\nButtonpos=0,0\nCurve=0.1,0.1\n";
    files["abilities.slk"] = Slk({{{"ID","AHbz"},{"code","AHbz"},{"Name","Ability parent"}}});
    files["abilities.txt"]="[AHbz]\nTip=\"first, note\",second\n";
    const auto read = [&](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.sources = {{ObjectType::Unit,"unit-meta.slk"},{ObjectType::Ability,"ability-meta.slk"}};
    schemaOptions.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load(read, schemaOptions); Good(schema);
    CatalogLoadOptions catalogOptions;
    catalogOptions.sources = {{ObjectType::Unit,{"units.slk"},{"units.txt"}},{ObjectType::Ability,{"abilities.slk"},{"abilities.txt"}}};
    catalogOptions.stringFiles.clear();
    auto standard = StandardObjectCatalog::Load(read, catalogOptions); Good(standard);
    const auto empty = [](ObjectType type) {
        auto parsed = LniObjectCodec(type).Parse({});
        Need(parsed.Succeeded(), "empty LNI");
        return *dynamic_cast<LniObjectDocument*>(parsed.document.get());
    };
    auto unit = ParseF6ObjectIni(ObjectType::Unit,
        "[v000]\n_parent=\"hfoo\"\nName=[=[\nline \"one\"\nline two]=]\nscale=0.13799999999999998\nButtonpos_1=0\nButtonpos_2=2\n", "unit.ini");
    Good(unit);
    auto edited = ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *unit.value, *schema.value, *standard.value); Good(edited);
    auto native = Services::ObjectTransactionSnapshot::Freeze(*edited.value, std::nullopt, std::string(64,'a'), &*schema.value, &*standard.value); Good(native);
    const auto* object = native.value->get()->CandidateNative().FindObject({ObjectType::Unit,"v000"});
    Need(object && object->modifications.size() == 4, "native address count");
    auto scale = std::find_if(object->modifications.begin(), object->modifications.end(), [](const auto& f) { return f.key.fieldId == "ussc"; });
    Need(scale != object->modifications.end() && std::get<float>(scale->value) == static_cast<float>(0.13799999999999998), "real retains float32 value");
    auto ability = ParseF6ObjectIni(ObjectType::Ability,
        "[K000]\n_parent=\"AHbz\"\nDataA={0.125,0.001}\nCool={1=3,2=4}\n", "ability.ini"); Good(ability);
    auto staged = ApplyHslkObjects(ObjectType::Ability, empty(ObjectType::Ability), *ability.value, *schema.value, *standard.value); Good(staged);
    auto encoded = Services::ObjectTransactionSnapshot::Freeze(*staged.value, std::nullopt, std::string(64,'a'), &*schema.value, &*standard.value); Good(encoded);
    const auto* skill = encoded.value->get()->CandidateNative().FindObject({ObjectType::Ability,"K000"});
    Need(skill && skill->modifications.size() == 4, "data letter and repeat addresses");
    ability.value->front().fields.push_back({"Tip",{std::string("line\none"),std::string("quote \"two\"")},true});
    auto rich=ApplyHslkObjects(ObjectType::Ability,empty(ObjectType::Ability),*ability.value,*schema.value,*standard.value); Good(rich);
    Services::LuaObjectSnapshotSources snapshot;
    snapshot.schema = &*schema.value; snapshot.standard = &*standard.value;
    snapshot.documents[static_cast<unsigned>(ObjectType::Unit)] = &*edited.value;
    snapshot.documents[static_cast<unsigned>(ObjectType::Ability)] = &*rich.value;
    snapshot.misc = {{"Misc", {{"Original", "unchanged"}}}};
    auto frozen = Services::BuildLuaObjectSnapshot(snapshot); Good(frozen);
    Script::LuaTemplateRequest check;
    std::string chunk = "<? local data=(function()\n" + std::string(reinterpret_cast<const char*>(frozen.value->data()), frozen.value->size())
        + "\nend)(); assert(data.unit.v000.Name == 'line \\\"one\\\"\\nline two'); "
          "assert(data.unit.v000.Buttonpos == '0,2'); assert(data.ability.K000.DataA1 == '0.125'); "
          "assert(data.ability.K000.Cool2 == '4'); assert(data.misc.Misc.Original == 'unchanged') ?>";
    check.preprocessed=chunk;
    Good(Script::RunLuaTemplate(check));
    chunk="<? local data=(function()\n"+std::string(reinterpret_cast<const char*>(frozen.value->data()),frozen.value->size())
        +"\nend)(); assert(data.ability.AHbz.Tip1=='first, note' and data.ability.AHbz.Tip2=='second'); "
         "assert(data.ability.K000.Tip1=='line\\none' and data.ability.K000.Tip==data.ability.K000.Tip1); "
         "assert(data.ability.K000.Tip2==[=[quote \"two\"]=]) ?>";
    check.preprocessed=chunk;
    Good(Script::RunLuaTemplate(check));
    auto blank=ParseF6ObjectIni(ObjectType::Unit,"[v001]\n_parent=\"hfoo\"\nscale=\"\"\n", "blank.ini"); Good(blank);
    auto zero=ApplyHslkObjects(ObjectType::Unit,empty(ObjectType::Unit),*blank.value,*schema.value,*standard.value); Good(zero);
    Need(std::get<float>(zero.value->Objects().front().modifications.front().value)==0.0f,"explicit empty LNI numeric is zero");
    blank.value->front().fields.front().values={std::string("not a number")};
    Need(!ApplyHslkObjects(ObjectType::Unit,empty(ObjectType::Unit),*blank.value,*schema.value,*standard.value).Succeeded(),"invalid numeric never becomes zero");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName=true\n","unit.ini").Succeeded(), "boolean rejected");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName={{1}}\n","unit.ini").Succeeded(), "nested rejected");
    Need(!ParseF6ObjectIni(ObjectType::Unit,"[v000]\nName={2=1}\n","unit.ini").Succeeded(), "sparse rejected");
    unit.value->front().fields.push_back({"unknown", {1.0}, false});
    Need(!ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *unit.value, *schema.value, *standard.value).Succeeded(), "unknown not dropped");

    const auto redundant = [&](std::string_view fields, const LniObjectDocument& baseline,
        std::string_view parent = "hfoo") {
        auto input = ParseF6ObjectIni(ObjectType::Unit, "[v010]\n_parent=\"" + std::string(parent) + "\"\n" + std::string(fields), "original.ini");
        Good(input);
        return ApplyHslkObjects(ObjectType::Unit, baseline, *input.value, *schema.value, *standard.value);
    };
    auto accepted = redundant("Fx=\"\"\n", empty(ObjectType::Unit)); Good(accepted);
    Need(accepted.value->Objects().front().modifications.empty(), "redundant alias adds no native field");
    Need(accepted.diagnostics.size() == 1 && accepted.diagnostics.front().severity == DiagnosticSeverity::Warning
        && accepted.diagnostics.front().code == "HLUA_REDUNDANT_INDEXED_FIELD"
        && accepted.diagnostics.front().message.find("original.ini:3") != std::string::npos
        && accepted.diagnostics.front().message.find("fx_2=[]") != std::string::npos, "one located warning includes every effective index");
    auto different = redundant("Fx=\"changed\"\n", empty(ObjectType::Unit));
    Need(!different.Succeeded() && different.diagnostics.front().message.find("fx_1, fx_2") != std::string::npos,
        "different scalar rejected with explicit key names");
    Need(!redundant("Fx=\"\"\nFx_2=\"changed\"\n", empty(ObjectType::Unit)).Succeeded(), "own explicit second value participates before alias check");
    Good(redundant("Fx_2=\"\"\nFx=\"\"\n", empty(ObjectType::Unit)));
    Need(!redundant("Fx={\"\"}\n", empty(ObjectType::Unit)).Succeeded(), "one-element array is not a scalar alias");
    Good(redundant("Buttonpos=0\n", empty(ObjectType::Unit)));
    Need(!redundant("Buttonpos=1\n", empty(ObjectType::Unit)).Succeeded(), "numeric alias does not broadcast");
    Need(!redundant("Curve=0.1\n", empty(ObjectType::Unit)).Succeeded(), "float32 rounding cannot prove exact scalar equality");
    Good(redundant("Curve=0.10000000149011612\n", empty(ObjectType::Unit)));
    auto parentInput = ParseF6ObjectIni(ObjectType::Unit, "[v009]\n_parent=\"hfoo\"\nFx_1=\"inherited\"\nFx_2=\"inherited\"\n", "parent.ini"); Good(parentInput);
    auto customParent = ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *parentInput.value, *schema.value, *standard.value); Good(customParent);
    Good(redundant("Fx=\"inherited\"\n", *customParent.value, "v009"));
    Need(!redundant("Fx=\"\"\n", *customParent.value, "v009").Succeeded(), "custom parent overrides are not replaced by standard defaults");
    Need(!redundant("Fx=\"inherited\"\nFx_2=\"changed\"\n", *customParent.value, "v009").Succeeded(), "own override wins across parent chain");
    Need(!redundant("Fx=\"\"\n", empty(ObjectType::Unit), "xxxx").Succeeded(), "missing parent remains rejected");
    const auto absent = redundant("Fx=\"\"\nFx_1=\"\"\n", empty(ObjectType::Unit), "hbar");
    Need(!absent.Succeeded() && absent.diagnostics.front().message.find("fx_2 (uxf2)") != std::string::npos,
        "missing second effective value is not assumed empty and diagnostic names the exact index");
    auto auditInput = ParseF6ObjectIni(ObjectType::Unit, "[v011]\n_parent=\"hfoo\"\nFx=\"\"\n", "audit.ini:3"); Good(auditInput);
    std::vector<IndexedFieldAudit> audits;
    auto audited = ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *auditInput.value, *schema.value, *standard.value, &audits); Good(audited);
    Need(audits.size() == 1 && audits.front().indices.size() == 2 && audits.front().indices[1].valueSource == "standard:hfoo", "structured ID receipt audit preserves source and all indices");
    BuildReceipt receipt; receipt.fieldMappingChecked = true; receipt.redundantIndexedFields = audits;
    const auto json = BuildReceiptJson(receipt);
    Need(json.find("\"field_mapping_checked\": true") != std::string::npos
        && json.find("\"field\": \"fx_2\"") != std::string::npos
        && json.find("standard:hfoo") != std::string::npos, "ID receipt exposes checked compatibility evidence");
    const auto nextPage = BuildReceiptJson(receipt, 1);
    Need(nextPage.find("\"redundant_indexed_field_count\": 1") != std::string::npos
        && nextPage.find("\"redundant_indexed_fields\": []") != std::string::npos, "audit pages retain total without repeating prior entries");
    auditInput.value->front().fields.push_back({"unknown", {1.0}, false});
    Need(!ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *auditInput.value, *schema.value, *standard.value, &audits).Succeeded()
        && audits.size() == 1, "failed mapping never publishes partial audit records");
}
