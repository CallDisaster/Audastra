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
        {{"ID","ucr2"},{"field","Curve"},{"slk","Profile"},{"type","real"},{"index","1"}},
        {{"ID","utg1"},{"field","Tags"},{"slk","Profile"},{"type","stringList"},{"index","0"}},
        {{"ID","utg2"},{"field","Tags"},{"slk","Profile"},{"type","stringList"},{"index","1"}},
        {{"ID","urac"},{"field","Race"},{"slk","UnitData"},{"type","string"}},
        {{"ID","uspc"},{"field","Special"},{"slk","UnitData"},{"type","string"},{"useSpecific","hdef"}}});
    files["item-meta.slk"] = Slk({
        {{"ID","inam"},{"field","Name"},{"slk","Profile"},{"type","string"},{"useItem","1"},{"index","0"}},
        {{"ID","iuse"},{"field","Uses"},{"slk","ItemData"},{"type","int"},{"useItem","1"}}});
    files["items.slk"] = Slk({{{"ID","gold"},{"code","gold"},{"Name","Item parent"}}});
    files["items.txt"] = "[gold]\nName=Item parent\n";
    files["ability-meta.slk"] = Slk({
        {{"ID","atp1"},{"field","Tip"},{"slk","Profile"},{"type","string"},{"repeat","4"},{"index","0"}},
        {{"ID","Hbz1"},{"field","Data"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"},{"data","1"},{"useSpecific","AHbz"}},
        {{"ID","acdn"},{"field","Cool"},{"slk","AbilityData"},{"type","unreal"},{"repeat","4"}}});
    files["units.slk"] = Slk({{{"ID","hfoo"},{"code","hfoo"},{"Name","Parent"}},
        {{"ID","hbar"},{"code","hbar"},{"Name","Missing components"}},
        {{"ID","hdef"},{"code","hdef"},{"Name","Complete empty profile"}}});
    files["units.txt"] = "[hfoo]\nFx=,\nButtonpos=0,0\nCurve=0.1,0.1\n[hdef]\n";
    files["abilities.slk"] = Slk({{{"ID","AHbz"},{"code","AHbz"},{"Name","Ability parent"}}});
    files["abilities.txt"]="[AHbz]\nTip=\"first, note\",second\n";
    const auto read = [&](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.sources = {{ObjectType::Unit,"unit-meta.slk"},{ObjectType::Ability,"ability-meta.slk"},{ObjectType::Item,"item-meta.slk"}};
    schemaOptions.stringFiles.clear();
    auto schema = ObjectFieldSchema::Load(read, schemaOptions); Good(schema);
    CatalogLoadOptions catalogOptions;
    catalogOptions.sources = {{ObjectType::Unit,{"units.slk"},{"units.txt"}},{ObjectType::Ability,{"abilities.slk"},{"abilities.txt"}}};
    catalogOptions.stringFiles.clear();
    catalogOptions.sources.push_back({ObjectType::Item,{"items.slk"},{"items.txt"}});
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
    auto omitted = ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *unit.value, *schema.value, *standard.value); Good(omitted);
    Need(omitted.diagnostics.front().code == "HLUA_FIELD_NOT_WRITTEN", "absent field is explicitly warned first");

    const auto redundant = [&](std::string_view fields, const LniObjectDocument& baseline,
        std::string_view parent = "hfoo") {
        auto input = ParseF6ObjectIni(ObjectType::Unit, "[v010]\n_parent=\"" + std::string(parent) + "\"\n" + std::string(fields), "original.ini");
        Good(input);
        return ApplyHslkObjects(ObjectType::Unit, baseline, *input.value, *schema.value, *standard.value);
    };
    auto accepted = redundant("Fx=\"\"\n", empty(ObjectType::Unit)); Good(accepted);
    Need(!redundant("Special=\"value\"\n", empty(ObjectType::Unit)).Succeeded(),
        "known field inapplicable to this parent cannot be skipped as absent");
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
    const auto located = ParseF6ObjectIni(ObjectType::Unit,
        std::string(4096, '\n') + "[v012]\n_parent=\"hfoo\"\nFx=\"\"\n", "block.ini"); Good(located);
    Need(located.value->front().fields.front().source == "block.ini:4099", "bounded line index locates a field beyond a block boundary");
    auditInput.value->front().fields.push_back({"scale", {std::string("invalid number")}, false});
    Need(!ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *auditInput.value, *schema.value, *standard.value, &audits).Succeeded()
        && audits.size() == 1, "failed mapping never publishes partial audit records");

    auto itemInput = ParseF6ObjectIni(ObjectType::Item, "[I000]\n_parent=\"gold\"\nrace=\"human\"\nnAmE=\"Named\"\n", "item.ini"); Good(itemInput);
    std::vector<UnwrittenField> unwritten;
    auto itemResult = ApplyHslkObjects(ObjectType::Item, empty(ObjectType::Item), *itemInput.value, *schema.value, *standard.value, nullptr, &unwritten); Good(itemResult);
    Need(itemResult.value->Objects().front().modifications.size() == 1 && unwritten.size() == 1
        && unwritten.front().source == "item.ini:3" && unwritten.front().otherObjectClasses == std::vector<std::string>{"unit"}
        && itemResult.diagnostics.front().message.find("单位有此字段，物品没有") != std::string::npos,
        "foreign class field skipped with located warning, mixed case known field still written");
    BuildReceipt omittedReceipt; omittedReceipt.unwrittenFields = unwritten;
    Need(BuildReceiptJson(omittedReceipt).find("\"unwritten_field_count\": 1") != std::string::npos
        && BuildReceiptJson(omittedReceipt).find("未写入的字段") != std::string::npos
        && BuildReceiptJson(omittedReceipt, 1).find("\"unwritten_fields\": []") != std::string::npos,
        "unwritten receipt has a separate paged section and total");
    itemInput.value->front().fields.push_back({"Uses", {std::string("text")}, false});
    auto itemFailed = ApplyHslkObjects(ObjectType::Item, empty(ObjectType::Item), *itemInput.value, *schema.value, *standard.value, nullptr, &unwritten);
    Need(!itemFailed.Succeeded() && unwritten.size() == 1 && itemFailed.diagnostics.front().code == "HLUA_FIELD_NOT_WRITTEN",
        "known numeric text still rejected, warnings retained and no partial audit published");
    auto missingSchemaOptions = schemaOptions;
    missingSchemaOptions.sources.push_back({ObjectType::Item,"missing-meta.slk"});
    auto incompleteSchema = ObjectFieldSchema::Load(read, missingSchemaOptions); Good(incompleteSchema);
    itemInput.value->front().fields.pop_back();
    auto unproven = ApplyHslkObjects(ObjectType::Item, empty(ObjectType::Item), *itemInput.value, *incompleteSchema.value, *standard.value);
    Need(!unproven.Succeeded() && unproven.diagnostics.front().code == "HLUA_METADATA_INCOMPLETE",
        "incomplete metadata cannot prove a field absent");
    auto duplicateSchemaOptions = schemaOptions;
    duplicateSchemaOptions.sources.push_back(duplicateSchemaOptions.sources.back());
    auto duplicateSchema = ObjectFieldSchema::Load(read, duplicateSchemaOptions); Good(duplicateSchema);
    Need(!ApplyHslkObjects(ObjectType::Item, empty(ObjectType::Item), *itemInput.value, *duplicateSchema.value, *standard.value).Succeeded(),
        "duplicate metadata sources cannot prove a field absent");
    Need(!ParseF6ObjectIni(ObjectType::Item,"[I000]\nrace=true\n","item.ini").Succeeded()
        && !ParseF6ObjectIni(ObjectType::Item,"[I000]\nrace={{1}}\n","item.ini").Succeeded(), "unknown fields do not bypass boolean or nested rejection");

    auto strictOptions = catalogOptions; strictOptions.rejectInvalidPresentSources = true;
    auto strictCatalog = StandardObjectCatalog::Load(read, strictOptions); Good(strictCatalog);
    Need(strictCatalog.value->Find(ObjectType::Unit, "hdef")->completeProfileAndSlk, "strict complete empty section is retained as presence");
    auto defaultInput = ParseF6ObjectIni(ObjectType::Unit, "[v020]\n_parent=\"hdef\"\nFx=\"\"\n", "default.ini"); Good(defaultInput);
    const auto mapped = [&](const StandardObjectCatalog& catalog) {
        return ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *defaultInput.value, *schema.value, catalog);
    };
    auto defaulted = mapped(*strictCatalog.value); Good(defaulted);
    Need(defaulted.value->Objects().front().modifications.empty()
        && defaulted.diagnostics.front().message.find("缺省按空串") != std::string::npos
        && defaulted.diagnostics.front().message.find("docs/HLUA_PROJECTS.md#profile-text-default-evidence") != std::string::npos,
        "absent text in complete standard profile is empty with evidence, no native override");
    std::vector<IndexedFieldAudit> defaultAudits;
    Good(ApplyHslkObjects(ObjectType::Unit, empty(ObjectType::Unit), *defaultInput.value,
        *schema.value, *strictCatalog.value, &defaultAudits));
    BuildReceipt defaultReceipt; defaultReceipt.redundantIndexedFields = defaultAudits;
    const auto defaultJson = BuildReceiptJson(defaultReceipt);
    Need(defaultJson.find("\"defaulted_text_empty\": true") != std::string::npos
        && defaultJson.find("docs/HLUA_PROJECTS.md#profile-text-default-evidence") != std::string::npos,
        "structured receipt preserves the default permission and native evidence path");
    Need(!mapped(*standard.value).Succeeded(), "permissive or incomplete source does not grant missing text defaults");
    defaultInput.value->front().fields.front().values = {std::string("nonempty")};
    Need(!mapped(*strictCatalog.value).Succeeded(), "absent standard text does not equal nonempty scalar");
    defaultInput.value->front().fields.front() = {"Buttonpos", {0.0}, false};
    Need(!mapped(*strictCatalog.value).Succeeded(), "missing numeric profile field never defaults to zero");
    defaultInput.value->front().fields.front() = {"Tags", {std::string{}}, false};
    Need(!mapped(*strictCatalog.value).Succeeded(), "missing list field is not a text scalar default");
    defaultInput.value->front().fields.front() = {"Fx", {std::string{}}, false};
    const auto cleanProfile = files["units.txt"];
    files["units.txt"] = cleanProfile + "Fx=\"\"\n";
    auto shortValue = StandardObjectCatalog::Load(read, strictOptions); Good(shortValue);
    Need(!mapped(*shortValue.value).Succeeded(), "present key with missing indexed component is not absent key");
    files["units.txt"] = cleanProfile + "[hdef]\n";
    auto duplicate = StandardObjectCatalog::Load(read, strictOptions); Good(duplicate);
    Need(!mapped(*duplicate.value).Succeeded(), "repeated section does not grant absent defaults");
    files["units.txt"] = cleanProfile + "unknown line\n";
    auto partial = StandardObjectCatalog::Load(read, strictOptions); Good(partial);
    Need(!mapped(*partial.value).Succeeded(), "uninterpreted profile line does not grant complete source");
    files["units.txt"] = cleanProfile;
    auto multiOptions = strictOptions; multiOptions.sources.push_back(strictOptions.sources.front());
    auto multi = StandardObjectCatalog::Load(read, multiOptions); Good(multi);
    Need(!mapped(*multi.value).Succeeded(), "multiple catalog definitions do not grant missing defaults");
    auto splitOptions = strictOptions; splitOptions.sources.front().profileFiles.push_back("more.txt");
    files["units.txt"] = cleanProfile + "Name=Complete\n";
    files["more.txt"] = "[hdef]\nOther=split source\n";
    auto split = StandardObjectCatalog::Load(read, splitOptions); Good(split);
    Good(mapped(*split.value));
    files["more.txt"] += "Name=Complete\n";
    auto overlapping = StandardObjectCatalog::Load(read, splitOptions); Good(overlapping);
    Need(!mapped(*overlapping.value).Succeeded(), "overlapping profile keys are not a unique source even when equal");
    files["units.txt"] = cleanProfile;
    auto rowOptions = strictOptions; rowOptions.sources.front().slkFiles.push_back("extra.slk");
    files["extra.slk"] = Slk({{{"ID","hfoo"}}});
    auto missingRow = StandardObjectCatalog::Load(read, rowOptions); Good(missingRow);
    Need(!mapped(*missingRow.value).Succeeded(), "parent row absent from one required SLK is incomplete");
    auto unreadable = StandardObjectCatalog::Load([&](const std::string& path) {
        return path == "units.txt" ? Result<std::vector<std::byte>>::Failure("Test", "read-failed", "unreadable") : read(path);
    }, strictOptions);
    Need(!unreadable.Succeeded(), "failed profile read is not an empty complete profile");
}
