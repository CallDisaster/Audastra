#include "War3MapCore/Services/ProjectSearch.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <windows.h>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes; for (const unsigned char c : text) bytes.push_back(static_cast<std::byte>(c)); return bytes;
}
const ProjectSearchCoverage& Coverage(const ProjectSearchResult& result, ProjectSearchCategory category) {
    return result.coverage[static_cast<std::size_t>(category)];
}
ObjectData::StandardObjectCatalog Catalog() {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
        "C;X1;Y2;K\"u001\"\nC;X2;K\"冰冠原版\"\nC;X1;Y3;K\"u002\"\nC;X2;K\"冰冠守卫\"\n"
        "C;X1;Y4;K\"u003\"\nC;X2;K\"冰冠斥候\"\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"original.slk"}, {}}};
    auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(slk));
    }, options);
    Require(catalog.Succeeded(), "original synthetic catalog parse"); return std::move(*catalog.value);
}
void OverlayAndIdentity() {
    Trigger::TriggerDocument triggers;
    Trigger::TriggerRecord first, second; first.id = 41; second.id = 42;
    first.name.text = second.name.text = "冰冠触发器"; triggers.triggers = {first, second};
    Trigger::VariableRecord variable; variable.id = 77; variable.name.text = "冰冠变量";
    variable.typeName.text = "integer"; triggers.variables = {variable};
    auto catalog = Catalog();
    ObjectData::ObjectRecord record; record.key = {ObjectData::ObjectType::Unit, "U001"};
    record.stableId = 123; record.parentRawcode = "u001";
    ObjectData::ObjectModification name; name.key.fieldId = "unam"; name.value = std::string("TRIGSTR_5");
    record.modifications = {name}; std::vector<ObjectData::ObjectRecord> records{record};
    ProjectSearchObjectSource objects{record.key.type, records, ProjectSearchState::Ready, ProjectSearchObjectOrigin::Native};
    auto strings = MapData::ParseStringTable(Bytes("STRING 5\n{\n冰冠自定义\n}\n")); Require(strings.Succeeded(), "WTS parse");
    ProjectSearchContext context; context.revision = "project-a/4/edited-9"; context.triggers = &triggers;
    context.triggerState = ProjectSearchState::Ready; context.objects = {&objects, 1};
    context.standardObjects = &catalog; context.standardState = ProjectSearchState::Ready; context.strings = &*strings.value;
    auto searched = SearchProject(context, context.revision, "冰冠");
    Require(searched.Succeeded() && searched.value->rows.size() == 7, "all actual categories search");
    Require(std::count_if(searched.value->rows.begin(), searched.value->rows.end(), [](const auto& row) {
        return row.title == "冰冠触发器" && row.sourceId && (*row.sourceId == 41 || *row.sourceId == 42);
    }) == 2, "same names retain separate source identities");
    const auto custom = std::find_if(searched.value->rows.begin(), searched.value->rows.end(), [](const auto& row) {
        return row.category == ProjectSearchCategory::MapObject;
    });
    Require(custom != searched.value->rows.end() && custom->title == "冰冠自定义" && custom->object == record.key
        && custom->contextRevision == context.revision, "typed object identity and WTS name");
    auto shortQuery = SearchProject(context, context.revision, "冰");
    Require(shortQuery.Succeeded() && Coverage(*shortQuery.value, ProjectSearchCategory::StandardObject).state == ProjectSearchState::QueryTooShort
        && Coverage(*shortQuery.value, ProjectSearchCategory::StandardObject).examined == 0, "short query must not scan standard objects");
    const auto oldRevision = context.revision; triggers.triggers[0].name.text = "当前修改"; context.revision = "project-a/4/edited-10";
    Require(!SearchProject(context, oldRevision, "冰冠").Succeeded(), "stale overlay context must fail");
    auto current = SearchProject(context, context.revision, "当前修改");
    Require(current.Succeeded() && current.value->rows.size() == 1 && current.value->rows[0].sourceId == 41,
        "search visible mutation rather than old disk or cache");
    context.revision = "project-b/1/edited-0";
    Require(!SearchProject(context, oldRevision, "冰冠").Succeeded(), "same titles in new project cannot reactivate old result");
    auto bounded = SearchProject(context, context.revision, "冰冠", {1, 2, 65536});
    Require(bounded.Succeeded() && bounded.value->rows.size() == 1 && bounded.value->hasMore
        && Coverage(*bounded.value, ProjectSearchCategory::StandardObject).scanTruncated
        && Coverage(*bounded.value, ProjectSearchCategory::StandardObject).examined == 2, "bounded top matches and explicit scan truncation");
    ProjectSearchObjectSource ambiguous[]{objects, objects}; context.objects = ambiguous;
    auto duplicate = SearchProject(context, context.revision, "冰冠");
    Require(duplicate.Succeeded() && Coverage(*duplicate.value, ProjectSearchCategory::MapObject).state == ProjectSearchState::Ambiguous,
        "multiple effective native/LNI sources cannot silently choose one");
}
void MapRowsAndLimits() {
    MapData::SoundsDocument sounds; MapData::SoundRecord sound{}; sound.name = "OldSound";
    sound.filePath = "Sound\\Original.wav"; sound.pitch = 1; sounds.entries = {sound};
    auto soundBytes = sounds.Serialize(); Require(soundBytes.Succeeded(), "synthetic sound serializer");
    MapData::RegionsDocument regions; MapData::RegionRecord region{}; region.name = "NorthRegion";
    region.right = region.top = 128; region.creationNumber = 12; regions.entries = {region};
    auto regionBytes = regions.Serialize(); Require(regionBytes.Succeeded(), "synthetic region serializer");
    MapData::CamerasDocument cameras; MapData::CameraRecord camera{}; camera.name = "CameraNorth";
    cameras.entries = {camera}; auto cameraBytes = cameras.Serialize(); Require(cameraBytes.Succeeded(), "synthetic camera serializer");
    MapData::UnitPlacements units; MapData::UnitPlacement unit; unit.rawcode = "u001"; unit.scale = {1, 1, 1};
    unit.creationId = 3; units.units = {unit}; auto unitBytes = units.Serialize(); Require(unitBytes.Succeeded(), "synthetic unit serializer");
    MapData::DoodadPlacements doodads; MapData::DoodadPlacement doodad; doodad.rawcode = "D001";
    doodad.scale = {1, 1, 1}; doodad.creationId = 4; doodads.doodads = {doodad};
    auto doodadBytes = doodads.Serialize(); Require(doodadBytes.Succeeded(), "synthetic doodad serializer");
    std::vector<MapArchiveMember> members{{"war3map.w3s", *soundBytes.value}, {"war3map.w3r", *regionBytes.value},
        {"war3map.w3c", *cameraBytes.value}, {"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value}};
    MapData::MapDataEditSession session(MapDocument::FromMembers(members));
    sounds.entries[0].name = "CurrentSound"; Require(session.EditSounds(sounds).Succeeded(), "sound overlay edit");
    MapDataQueryBinding binding{"map-search", 1, session.Revision(), std::string(64, '0')};
    auto snapshot = CaptureMapDataSnapshot(session.Snapshot(), binding, false); Require(snapshot.Succeeded(), "edited map snapshot");
    auto imports = Resources::ImportTable::CreateEmpty(); Require(imports.Upsert("Icons\\Custom.blp").Succeeded(), "import overlay");
    ProjectSearchContext context; context.revision = "map-search/1/1"; context.mapData = &*snapshot.value;
    context.mapDataBinding = binding; context.imports = &imports; context.importState = ProjectSearchState::Ready;
    auto result = SearchProject(context, context.revision, ""); Require(result.Succeeded(), "map typed search");
    for (const auto category : {ProjectSearchCategory::Sound, ProjectSearchCategory::Region, ProjectSearchCategory::Camera,
            ProjectSearchCategory::UnitPlacement, ProjectSearchCategory::DoodadPlacement, ProjectSearchCategory::ImportedFile})
        Require(Coverage(*result.value, category).examined == 1, "typed map/import category enumeration");
    auto current = SearchProject(context, context.revision, "CurrentSound");
    Require(current.Succeeded() && current.value->rows.size() == 1 && current.value->rows[0].mapKind == MapDataKind::Sounds
        && current.value->rows[0].sourceIndex == 0 && !current.value->rows[0].rowKey.empty(), "current overlay sound with same service row key");
    auto old = SearchProject(context, context.revision, "OldSound"); Require(old.Succeeded() && old.value->rows.empty(), "old baseline not searched");
    ProjectSearchImportedFile pending[]{ {"pending-1", "Icons\\Pending.blp"} };
    context.currentFiles = pending; context.usesImportedFiles = true;
    auto files = SearchProject(context, context.revision, "Pending");
    Require(files.Succeeded() && files.value->rows.size() == 1 && files.value->rows[0].rowKey == "pending-1", "current resource overlay has priority over import baseline");
    context.currentFiles = {};
    Require(SearchProject(context, context.revision, "Custom").value->rows.empty(), "explicit empty current resources must not revive deleted baseline");
    ++context.mapDataBinding.revision; Require(!SearchProject(context, context.revision, "").Succeeded(), "stale typed snapshot binding refused");
    context.mapDataBinding = binding;
    auto tiny = SearchProject(context, context.revision, "", {128, 4096, 1});
    Require(tiny.Succeeded() && tiny.value->rows.empty() && tiny.value->hasMore, "byte budget cannot spill result payload");
    context.mapData = nullptr; context.imports = nullptr; context.importState = ProjectSearchState::Unavailable;
    auto unavailable = SearchProject(context, context.revision, "North");
    Require(unavailable.Succeeded() && !unavailable.value->complete
        && Coverage(*unavailable.value, ProjectSearchCategory::Region).state == ProjectSearchState::Unavailable,
        "unsupported/missing typed data is not a verified zero count");
    Require(!SearchProject(context, context.revision, std::string(257, 'x')).Succeeded()
        && !SearchProject(context, context.revision, "\xC0\xAF").Succeeded(), "bounded valid UTF8 queries");
}
void CodeIndexAndAliasBoundaries() {
    const std::string body = "function Searchable takes integer count returns nothing\nendfunction\n";
    const Jass::JassSource sources[]{{"WCT/header.j", body}, {"base/common.j", body}};
    auto index = Jass::IndexJassDeclarations(sources);
    Require(index.completed && index.symbols.size() == 2, "actual parser declaration input");
    ProjectSearchContext context; context.revision = "code-project/1/2";
    context.codeDeclarations = &index; context.codeState = ProjectSearchState::Ready; context.codeBinding = "owned-source-binding";
    const auto result = SearchProject(context, context.revision, "Searchable");
    Require(result.Succeeded() && result.value->rows.size() == 2, "U3 searches real frozen declarations");
    Require(result.value->rows[0].stableId != result.value->rows[1].stableId, "duplicate names retain source identity");
    for (const auto& row : result.value->rows)
        Require(row.category == ProjectSearchCategory::CodeSymbol && row.codeSource && row.codeLine == 1
            && row.codeBinding == context.codeBinding && row.contextRevision == context.revision,
            "U3 navigation carries actual source line and binding");
    const auto limited = SearchProject(context, context.revision, "Searchable", {128, 1, 65536});
    Require(limited.Succeeded() && Coverage(*limited.value, ProjectSearchCategory::CodeSymbol).scanTruncated,
        "declaration scan retains shared search budget");
    index.completed = false;
    const auto partial = SearchProject(context, context.revision, "Searchable");
    Require(partial.Succeeded() && Coverage(*partial.value, ProjectSearchCategory::CodeSymbol).state == ProjectSearchState::Partial,
        "partial declaration index is not a complete empty or complete reference index");
    context.codeBinding = {};
    const auto missingBinding = SearchProject(context, context.revision, "Searchable");
    Require(missingBinding.Succeeded() && missingBinding.value->rows.empty()
        && Coverage(*missingBinding.value, ProjectSearchCategory::CodeSymbol).state == ProjectSearchState::Invalid,
        "unbound code source cannot provide navigation");

    const auto root = std::filesystem::temp_directory_path() / ("Audastra-SearchAlias-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root / "base");
    std::ofstream(root / "config", std::ios::binary) << "base\n";
    std::ofstream(root / "base/define.txt", std::ios::binary) << "[TriggerTypes]\nunitcode=0,1,1,单位,integer\nUnitAlias=0,1,1,单位别名,unitcode\n";
    auto ui = Trigger::UiCatalog::Load(root);
    Require(ui.Succeeded(), "original alias metadata loads");
    const Trigger::TriggerTypeSystem types(*ui.value);
    auto catalog = Catalog();
    Trigger::TriggerDocument document; Trigger::VariableRecord variable;
    variable.id = 9; variable.name.text = "TypedAlias"; variable.typeName.text = "UnitAlias";
    variable.hasDefault = 1; variable.value.text = "u001"; document.variables.push_back(variable);
    context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
    context.types = &types; context.standardObjects = &catalog; context.standardState = ProjectSearchState::Ready;
    const auto alias = SearchProject(context, context.revision, "TypedAlias");
    Require(alias.Succeeded() && alias.value->rows.size() == 1 && alias.value->rows[0].detail.find("冰冠原版") != std::string::npos,
        "typed variable alias uses the shared resolver in U3");
    std::filesystem::remove_all(root);
}
ProjectSearchContext ParameterContext(Trigger::TriggerDocument& document) {
    Trigger::TriggerRecord trigger; trigger.id = 100; trigger.name.text = "参数所属触发器"; trigger.ecas = {0};
    Trigger::EcaNode action; action.id = 200; action.sourceTriggerId = 100; action.name.text = "DisplayText"; action.arguments = {0};
    Trigger::EcaNode call; call.id = 201; call.sourceTriggerId = 100; call.name.text = "NestedText"; call.arguments = {1};
    Trigger::ArgumentNode argument; argument.id = 300; argument.type = 3; argument.value.text = "中文参数唯一命中";
    argument.insertCall = 1; argument.callEca = 1; argument.insertIndex = 1; argument.indexArgument = 2;
    Trigger::ArgumentNode nested; nested.id = 301; nested.value.text = "嵌套参数唯一命中";
    Trigger::ArgumentNode index; index.id = 302; index.value.text = "下标参数唯一命中";
    document.triggers = {trigger}; document.ecaNodes = {action, call}; document.argumentNodes = {argument, nested, index};
    ProjectSearchContext context; context.revision = "parameter-project/1/5";
    context.triggers = &document; context.triggerState = ProjectSearchState::Ready; return context;
}
void ParameterIdentityAndBounds() {
    Trigger::TriggerDocument document; auto context = ParameterContext(document);
    const auto result = SearchProject(context, context.revision, "中文参数唯一命中");
    Require(result.Succeeded() && result.value->rows.size() == 1, "Chinese parameter actual IR is searchable");
    const auto& row = result.value->rows.front();
    Require(row.sourceId == 100 && row.ecaId == 200 && row.argumentId == 300
        && row.stableId == "trigger/100/eca/200/argument/300" && row.rawValue == "中文参数唯一命中",
        "parameter navigation binds real trigger ECA and argument IDs");
    const auto nested = SearchProject(context, context.revision, "嵌套参数唯一命中");
    Require(nested.Succeeded() && nested.value->rows.size() == 1 && nested.value->rows[0].ecaId == 201
        && nested.value->rows[0].argumentId == 301, "nested call belongs to actual nested ECA");
    const auto index = SearchProject(context, context.revision, "下标参数唯一命中");
    Require(index.Succeeded() && index.value->rows.size() == 1 && index.value->rows[0].ecaId == 200
        && index.value->rows[0].argumentId == 302, "index parameter retains owning ECA");
    document.argumentNodes[1].value.text = "TRIGSTR_9";
    auto strings = MapData::ParseStringTable(Bytes("STRING 9\n{\n当前参数字符串\n}\n"));
    Require(strings.Succeeded(), "parameter WTS synthetic parse"); context.strings = &*strings.value;
    const auto resolved = SearchProject(context, context.revision, "当前参数字符串");
    Require(resolved.Succeeded() && resolved.value->rows.size() == 1 && resolved.value->rows[0].title == "当前参数字符串"
        && resolved.value->rows[0].rawValue == "TRIGSTR_9", "shared WTS resolution preserves raw protocol parameter");
    document.argumentNodes[1].value.text = "\xC0\xAF";
    const auto malformed = SearchProject(context, context.revision, "中文参数");
    Require(malformed.Succeeded() && malformed.value->rows.empty()
        && Coverage(*malformed.value, ProjectSearchCategory::Trigger).state == ProjectSearchState::Partial,
        "malformed graph text is partial and no uncertain parameter navigation escapes");
    Require(std::any_of(malformed.diagnostics.begin(), malformed.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "PROJECT_SEARCH_TRIGGER_TEXT_INVALID" && diagnostic.severity == DiagnosticSeverity::Warning;
    }), "bad UTF8 remains an explicit warning rather than a completed empty query");
    document.argumentNodes[1].value.text = "valid";
    document.argumentNodes[2].insertIndex = 1; document.argumentNodes[2].indexArgument = 0;
    const auto cycle = SearchProject(context, context.revision, "中文参数");
    Require(cycle.Succeeded() && cycle.value->rows.empty()
        && Coverage(*cycle.value, ProjectSearchCategory::Trigger).graphNodesExamined <= 6
        && Coverage(*cycle.value, ProjectSearchCategory::Trigger).state == ProjectSearchState::Partial,
        "argument cycle terminates and declines malformed trigger graph navigation");
    Require(std::any_of(cycle.diagnostics.begin(), cycle.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "PROJECT_SEARCH_TRIGGER_GRAPH_INVALID";
    }), "cyclic graph cause is reported independently of truncation");
    context = ParameterContext(document); document.ecaNodes[1].sourceTriggerId = 999;
    const auto foreign = SearchProject(context, context.revision, "中文参数");
    Require(foreign.Succeeded() && foreign.value->rows.empty(), "cross-trigger ECA source binding is refused");
    context = ParameterContext(document); document.argumentNodes[1].id = document.argumentNodes[0].id;
    const auto duplicate = SearchProject(context, context.revision, "中文参数");
    Require(duplicate.Succeeded() && duplicate.value->rows.empty()
        && Coverage(*duplicate.value, ProjectSearchCategory::Trigger).state == ProjectSearchState::Partial,
        "duplicate argument stable IDs cannot provide ambiguous navigation");
    context = ParameterContext(document);
    ProjectSearchOptions options; options.maxTriggerNodes = 2;
    const auto bounded = SearchProject(context, context.revision, "中文参数", options);
    Require(bounded.Succeeded() && bounded.value->rows.empty()
        && Coverage(*bounded.value, ProjectSearchCategory::Trigger).graphNodesExamined == 2
        && Coverage(*bounded.value, ProjectSearchCategory::Trigger).scanTruncated,
        "graph budget counts unmatching ECA/argument work and reports partial scan");
    options.maxTriggerNodes = 8192; options.maxTriggerTextBytes = 1;
    const auto text = SearchProject(context, context.revision, "中文参数", options);
    Require(text.Succeeded() && text.value->rows.empty()
        && Coverage(*text.value, ProjectSearchCategory::Trigger).scanTruncated
        && Coverage(*text.value, ProjectSearchCategory::Trigger).textBytesExamined == 0,
        "text budget accounts before scanning or copying oversize values");
    options.maxTriggerTextBytes = 1024 * 1024; document.argumentNodes[0].value.text = "long-value-" + std::string(4096, 'z');
    const auto longValue = SearchProject(context, context.revision, "long-value", options);
    Require(longValue.Succeeded() && longValue.value->rows.size() == 1 && longValue.value->rows[0].textTruncated
        && longValue.value->rows[0].rawValue->size() <= 2048, "large raw parameters are bounded and explicitly partial");
}
void StableWindowPages() {
    Trigger::TriggerDocument document;
    for (StableId i = 1; i <= 9; ++i) {
        Trigger::TriggerRecord trigger; trigger.id = i; trigger.name.text = "PageItem"; document.triggers.push_back(trigger);
    }
    ProjectSearchContext context; context.revision = "page-project/4/9";
    context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
    ProjectSearchOptions options; options.limit = 8;
    const auto first = SearchProjectPage(context, context.revision, "PageItem", options, 3);
    Require(first.Succeeded() && first.value->rows.size() == 3 && first.value->next
        && first.value->totalWindowRows == 8 && first.value->windowHasMore,
        "page contains only bounded ranked window and truthfully reports ninth external match");
    const auto second = SearchProjectPage(context, context.revision, "PageItem", options, 3, &*first.value->next);
    Require(second.Succeeded() && second.value->rows.size() == 3 && second.value->next
        && second.value->rows.front().sourceId == 4 && second.value->windowSha256 == first.value->windowSha256,
        "stable equal-score pages retain source order without duplicate rows");
    const auto last = SearchProjectPage(context, context.revision, "PageItem", options, 3, &*second.value->next);
    Require(last.Succeeded() && last.value->rows.size() == 2 && !last.value->next && last.value->windowHasMore,
        "end of bounded window does not promise access to unretained results");
    Require(!SearchProjectPage(context, context.revision, "Other", options, 3, &*first.value->next).Succeeded(),
        "query change invalidates cursor");
    Require(!SearchProjectPage(context, context.revision, "PageItem", options, 2, &*first.value->next).Succeeded(),
        "page scope change invalidates cursor");
    auto changed = options; ++changed.maxTriggerNodes;
    Require(!SearchProjectPage(context, context.revision, "PageItem", changed, 3, &*first.value->next).Succeeded(),
        "even unused scan budget changes invalidate cursor binding");
    document.triggers[4].name.text = "ChangedItem";
    Require(!SearchProjectPage(context, context.revision, "PageItem", options, 3, &*first.value->next).Succeeded(),
        "window mutation cannot continue despite host accidentally unchanged revision");
    context.revision = "page-project/4/10";
    Require(!SearchProjectPage(context, context.revision, "PageItem", options, 3, &*first.value->next).Succeeded(),
        "new edited revision rejects old result cursor");
    const auto empty = SearchProjectPage(context, context.revision, "NoMatchingText", options, 3);
    Require(empty.Succeeded() && empty.value->rows.empty() && !empty.value->next && empty.value->totalWindowRows == 0,
        "empty page does not invent total source coverage");
    Require(!SearchProjectPage(context, context.revision, std::string("a\0b", 3), options, 3).Succeeded(),
        "NUL query is invalid UTF8 search input");
}
bool NameDiagnostic(const Result<ProjectSearchResult>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code && diagnostic.severity == DiagnosticSeverity::Warning;
    });
}
void NameBudget(const ProjectSearchResult& result, const ProjectSearchOptions& options) {
    Require(result.nameNodesCharged <= options.maxTriggerNodes && result.nameTextBytesCharged <= options.maxTriggerTextBytes,
        "global name accounting must stay within both budgets");
}
Trigger::VariableRecord NameVariable(std::string value, std::string type = "string") {
    Trigger::VariableRecord variable; variable.id = 77; variable.name.text = "BudgetVariable";
    variable.typeName.text = std::move(type); variable.hasDefault = 1; variable.value.text = std::move(value); return variable;
}
void CompleteNameDomainsAndSuffixes() {
    ObjectData::ObjectRecord object; object.stableId = 1; object.key = {ObjectData::ObjectType::Unit, "U001"};
    ObjectData::ObjectModification name; name.key.fieldId = "unam"; name.value = std::string("NeverResolveSuffix");
    object.modifications = {name};
    std::vector<ObjectData::ObjectRecord> records(5000, object);
    ProjectSearchObjectSource source{object.key.type, records, ProjectSearchState::Ready, ProjectSearchObjectOrigin::Native};
    ProjectSearchContext context; context.revision = "name-budget/1/1"; context.objects = {&source, 1};
    ProjectSearchOptions options; options.maxExaminedPerCategory = 1;
    const auto limited = SearchProject(context, context.revision, "", options);
    Require(limited.Succeeded() && Coverage(*limited.value, ProjectSearchCategory::MapObject).state == ProjectSearchState::Partial
        && limited.value->rows.size() == 1 && limited.value->rows[0].title == "U001"
        && NameDiagnostic(limited, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET"), "one result cannot resolve against 5000 unchecked suffix records");
    Require(limited.value->nameNodesCharged < 32, "large suffix must be rejected before domain traversal");
    NameBudget(*limited.value, options);
    Require(records.size() == 5000 && records.back().key.rawcode == "U001"
        && std::get<std::string>(records.front().modifications[0].value) == "NeverResolveSuffix", "search leaves all object data intact");
    records.resize(1); source.records = records;
    records[0].modifications.resize(5000, name);
    const auto modifications = SearchProject(context, context.revision, "", options);
    Require(modifications.Succeeded() && modifications.value->rows[0].title == "U001"
        && NameDiagnostic(modifications, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET"), "large unmatching modification suffix cannot bypass row limit");
    NameBudget(*modifications.value, options);
    records[0].modifications = {name}; records[0].modifications[0].value = std::string(100000, 'x');
    const auto field = SearchProject(context, context.revision, "", options);
    Require(field.Succeeded() && field.value->rows[0].title == "U001"
        && NameDiagnostic(field, "PROJECT_SEARCH_NAME_FIELD_BUDGET")
        && std::get<std::string>(records[0].modifications[0].value).size() == 100000,
        "large field is rejected before ValueToDisplayString and remains untouched");
    NameBudget(*field.value, options);
    records[0].modifications = {name, name}; records[0].modifications[1].key.fieldId = "unused_field";
    records[0].modifications[1].value = std::string(100000, 'z');
    options.maxExaminedPerCategory = 2;
    const auto unused = SearchProject(context, context.revision, "", options);
    Require(unused.Succeeded() && unused.value->rows[0].title == "U001"
        && NameDiagnostic(unused, "PROJECT_SEARCH_NAME_FIELD_BUDGET"), "unmatching large fields are counted before resolving an earlier name field");
    NameBudget(*unused.value, options);
    options.maxExaminedPerCategory = 1;

    Trigger::TriggerDocument document; document.variables.assign(5000, NameVariable("udg_missing", "variable"));
    for (std::size_t index = 0; index < document.variables.size(); ++index) document.variables[index].id = index + 1;
    context.objects = {}; context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
    const auto variables = SearchProject(context, context.revision, "BudgetVariable", options);
    Require(variables.Succeeded() && variables.value->rows.size() == 1
        && variables.value->rows[0].detail.find("udg_missing") != std::string::npos
        && NameDiagnostic(variables, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET") && variables.value->nameNodesCharged < 32,
        "variable default must not scan unchecked variable suffix");
    NameBudget(*variables.value, options);
    document.variables = {NameVariable("TRIGSTR_9")};
    MapData::StringTableDocument strings;
    strings.entries.assign(5000, MapData::StringTableEntry{9, "ForbiddenWtsName", 0}); context.strings = &strings;
    const auto wts = SearchProject(context, context.revision, "BudgetVariable", options);
    Require(wts.Succeeded() && wts.value->rows[0].detail.find("TRIGSTR_9") != std::string::npos
        && wts.value->rows[0].detail.find("ForbiddenWtsName") == std::string::npos
        && NameDiagnostic(wts, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET") && wts.value->nameNodesCharged < 32,
        "WTS variable default refuses full unchecked table rather than guessing first match");
    NameBudget(*wts.value, options);
    Require(strings.entries.size() == 5000 && strings.entries.back().value == "ForbiddenWtsName"
        && document.variables.front().value.text == "TRIGSTR_9", "name fallback does not modify WTS or variable initial value");
    strings.entries.resize(1); strings.entries[0].value = "ResolvedDefault";
    const auto resolvedDefault = SearchProject(context, context.revision, "BudgetVariable", options);
    Require(resolvedDefault.Succeeded() && resolvedDefault.value->rows.size() == 1
        && resolvedDefault.value->rows[0].detail.find("ResolvedDefault") != std::string::npos
        && resolvedDefault.value->rows[0].sourceId == 77, "complete single-entry WTS still uses shared variable-default resolver");
    NameBudget(*resolvedDefault.value, options);
    strings.entries[0].value.assign(100000, 'y');
    const auto wtsField = SearchProject(context, context.revision, "BudgetVariable", options);
    Require(wtsField.Succeeded() && NameDiagnostic(wtsField, "PROJECT_SEARCH_NAME_FIELD_BUDGET")
        && wtsField.value->rows[0].detail.find("TRIGSTR_9") != std::string::npos, "large WTS value is not decoded before byte guard");
    NameBudget(*wtsField.value, options);
}
void InheritanceAndSharedNameBudget() {
    ObjectData::ObjectRecord first; first.key = {ObjectData::ObjectType::Unit, "U001"}; first.parentRawcode = "U002";
    ObjectData::ObjectRecord second; second.key = {ObjectData::ObjectType::Unit, "U002"}; second.parentRawcode = "U001";
    std::vector<ObjectData::ObjectRecord> records{first, second};
    ProjectSearchObjectSource source{first.key.type, records, ProjectSearchState::Ready, ProjectSearchObjectOrigin::Native};
    ProjectSearchContext context; context.revision = "name-cycle/1/1"; context.objects = {&source, 1};
    ProjectSearchOptions options;
    const auto cycle = SearchProject(context, context.revision, "", options);
    Require(cycle.Succeeded() && cycle.value->rows.size() == 2 && cycle.value->rows[0].title == "U001"
        && cycle.value->rows[1].title == "U002" && NameDiagnostic(cycle, "PROJECT_SEARCH_NAME_INHERITANCE_CYCLE")
        && records[0].parentRawcode == "U002" && records[1].parentRawcode == "U001", "inheritance cycle fallback preserves original keys and data");
    NameBudget(*cycle.value, options);
    records.assign(17, first);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto key = std::string("U") + (index < 10 ? "00" : "0") + std::to_string(index);
        records[index].key.rawcode = key;
        records[index].parentRawcode = index + 1 < records.size()
            ? std::optional<std::string>{std::string("U") + (index + 1 < 10 ? "00" : "0") + std::to_string(index + 1)} : std::nullopt;
    }
    source.records = records;
    const auto chain = SearchProject(context, context.revision, "", options);
    Require(chain.Succeeded() && NameDiagnostic(chain, "PROJECT_SEARCH_NAME_INHERITANCE_BUDGET"), "search does not enter 256-layer resolver for long inheritance chain");
    NameBudget(*chain.value, options);

    Trigger::TriggerDocument document;
    for (std::size_t index = 0; index < 10; ++index) {
        auto variable = NameVariable(std::string(40, 'a')); variable.id += index; document.variables.push_back(std::move(variable));
    }
    context.objects = {}; context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
    options.maxTriggerTextBytes = 1000;
    const auto bytes = SearchProject(context, context.revision, "NoMatch", options);
    Require(bytes.Succeeded() && bytes.value->rows.empty() && bytes.value->nameTextBytesCharged > 0
        && NameDiagnostic(bytes, "PROJECT_SEARCH_NAME_TEXT_BUDGET"), "unmatched defaults consume one global name byte account");
    NameBudget(*bytes.value, options);
    options.maxTriggerTextBytes = 1024 * 1024; options.maxTriggerNodes = 2;
    const auto nodes = SearchProject(context, context.revision, "BudgetVariable", options);
    Require(nodes.Succeeded() && NameDiagnostic(nodes, "PROJECT_SEARCH_NAME_NODE_BUDGET"), "all defaults share one global name node account");
    NameBudget(*nodes.value, options);
    const auto page = SearchProjectPage(context, context.revision, "BudgetVariable", options, 3);
    Require(page.Succeeded() && page.value->nameNodesCharged == nodes.value->nameNodesCharged
        && page.value->nameTextBytesCharged == nodes.value->nameTextBytesCharged, "page exposes exact same conservative name charges");
}
void PlacementAndTypedWtsNameBudgets() {
    MapData::UnitPlacements units; MapData::UnitPlacement unit; unit.rawcode = "u001"; unit.scale = {1, 1, 1}; unit.creationId = 3;
    units.units.assign(5000, unit); const auto unitBytes = units.Serialize(); Require(unitBytes.Succeeded(), "original large placement metadata fixture serializes");
    MapData::SoundsDocument sounds; MapData::SoundRecord sound{}; sound.name = "TRIGSTR_9"; sound.filePath = "Sound\\Fixture.wav"; sound.pitch = 1;
    sounds.entries = {sound}; const auto soundBytes = sounds.Serialize(); Require(soundBytes.Succeeded(), "original sound-name WTS fixture serializes");
    std::string wtsBytes;
    for (std::size_t index = 0; index < 5000; ++index) wtsBytes += "STRING " + std::to_string(index) + "\n{\nFixtureWtsValue\n}\n";
    const MapArchiveMember members[]{{"war3mapUnits.doo", *unitBytes.value}, {"war3map.w3s", *soundBytes.value}, {"war3map.wts", Bytes(wtsBytes)}};
    const MapDataQueryBinding binding{"name-map", 1, 0, std::string(64, '0')};
    auto snapshot = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, false); Require(snapshot.Succeeded(), "held map metadata snapshot");
    const auto owned = snapshot.value->OwnedDocument();
    const auto originalUnits = owned->Member(MapDataKind::Units).sources.front().data;
    const auto originalStrings = owned->Member(MapDataKind::Strings).sources.front().data;
    Trigger::TriggerDocument document; document.variables = {NameVariable("gg_unit_u001_0003", "unit")};
    ProjectSearchContext context; context.revision = "name-map/1/0"; context.mapData = &*snapshot.value; context.mapDataBinding = binding;
    context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
    ProjectSearchOptions options; options.maxExaminedPerCategory = 1;
    const auto result = SearchProject(context, context.revision, "", options);
    Require(result.Succeeded() && NameDiagnostic(result, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET"), "gg default and typed sound cannot scan 5000-row domains");
    const auto variable = std::find_if(result.value->rows.begin(), result.value->rows.end(), [](const auto& row) { return row.category == ProjectSearchCategory::Variable; });
    const auto namedSound = std::find_if(result.value->rows.begin(), result.value->rows.end(), [](const auto& row) { return row.category == ProjectSearchCategory::Sound; });
    Require(variable != result.value->rows.end() && variable->detail.find("gg_unit_u001_0003") != std::string::npos,
        "placement name fallback retains original default");
    const auto rowKey = MapData::MapFieldRowKey(*owned, MapDataKind::Sounds, 0);
    Require(namedSound != result.value->rows.end() && namedSound->title == "TRIGSTR_9" && namedSound->sourceIndex == 0
        && rowKey.Succeeded() && namedSound->rowKey == *rowKey.value
        && Coverage(*result.value, ProjectSearchCategory::Sound).state == ProjectSearchState::Partial,
        "typed WTS fallback keeps same owned snapshot row identity rather than guessed resolved name");
    Require(result.value->nameNodesCharged < 64, "one-row name budget does not traverse placement or WTS suffix");
    NameBudget(*result.value, options);
    Require(snapshot.value->OwnedDocument() == owned && snapshot.value->Binding() == binding
        && owned->Member(MapDataKind::Units).sources.front().data == originalUnits
        && owned->Member(MapDataKind::Strings).sources.front().data == originalStrings
        && document.variables[0].value.text == "gg_unit_u001_0003", "all held map bytes, owner, binding and defaults remain read-only");
}
} // namespace
int main() {
    try { OverlayAndIdentity(); MapRowsAndLimits(); CodeIndexAndAliasBoundaries(); ParameterIdentityAndBounds(); StableWindowPages();
        CompleteNameDomainsAndSuffixes(); InheritanceAndSharedNameBudget(); PlacementAndTypedWtsNameBudgets();
        std::cout << "project search bounded identity tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
