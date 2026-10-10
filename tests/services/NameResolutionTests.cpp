#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Services/TriggerStringEditing.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Text/TextUtil.h"
#include "../MapDataFixtures.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <windows.h>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using namespace War3Map::ObjectData;
void Expect(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* start = reinterpret_cast<const std::byte*>(value.data());
    return {start, start + value.size()};
}
StandardObjectCatalog Catalog() {
    CatalogLoadOptions options;
    options.sources = {{ObjectType::Unit, {"units.slk"}, {}}};
    options.stringFiles.clear();
    auto result = StandardObjectCatalog::Load([](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes("ID;PWXL\nC;X1;Y1;K\"unitID\"\nC;X2;K\"Name\"\nC;X3;K\"Art\"\n"
            "C;X1;Y2;K\"hfoo\"\nC;X2;K\"原版步兵\"\nC;X3;K\"Icons/Footman.blp\"\nE\n"));
    }, options);
    Expect(result.Succeeded(), "original catalog loads");
    return std::move(*result.value);
}
void Names() {
    auto catalog = Catalog();
    auto strings = MapData::ParseStringTable(Bytes("// original\nSTRING 7\n{\n|cffff0000地图名称|r\n}\nSTRING 8\n{\n正文\n}\n"));
    Expect(strings.Succeeded(), "original WTS parses");
    std::vector<ObjectRecord> records(3);
    records[0].key = {ObjectType::Unit, "U001"}; records[0].parentRawcode = "hfoo";
    ObjectModification name; name.key.fieldId = "unam"; name.value = std::string("TRIGSTR_7");
    records[0].modifications.push_back(name);
    records[1].key = {ObjectType::Unit, "U002"}; records[1].parentRawcode = "U001";
    records[2].key = {ObjectType::Unit, "hfoo"}; records[2].parentRawcode = "hfoo";
    ProjectSearchObjectSource source{ObjectType::Unit, records, ProjectSearchState::Ready};
    Trigger::TriggerDocument triggers;
    Trigger::TriggerRecord trigger; trigger.id = 10; trigger.name.text = "中文 开始!"; triggers.triggers.push_back(trigger);
    Trigger::VariableRecord variable; variable.id = 11; variable.name.text = "Hero"; triggers.variables.push_back(variable);
    NameResolutionContext context;
    context.objects = std::span(&source, 1); context.standardObjects = &catalog;
    context.strings = &*strings.value; context.triggers = &triggers;
    const auto custom = ResolveName(context, "'U002'", "unitcode");
    Expect(custom.rawValue == "'U002'" && custom.displayName == "地图名称" && custom.reference == "TRIGSTR_7"
        && custom.iconPath == "Icons/Footman.blp", "typed inheritance WTS/icon raw bytes");
    ObjectModification clearedIcon; clearedIcon.key.fieldId = "uico"; clearedIcon.value = std::string{};
    records[1].modifications.push_back(clearedIcon);
    const auto clearedChild = ResolveName(context, "'U002'", "unitcode");
    Expect(clearedChild.state == NameResolutionState::Resolved && clearedChild.displayName == "地图名称"
        && clearedChild.iconPath.empty(), "explicit empty child icon overrides inherited and standard icons");
    records[1].modifications.clear();
    records[0].modifications.push_back(clearedIcon);
    Expect(ResolveObjectName(context, records[1].key).iconPath.empty(), "empty parent icon remains authoritative through the chain");
    records[0].modifications.pop_back();
    Expect(ResolveObjectName(context, records[1].key).iconPath == "Icons/Footman.blp", "absent icon inherits again without mutating its source");
    Expect(ResolveObjectName(context, {ObjectType::Unit, "hfoo"}).displayName == "原版步兵", "parent self is standard override");
    Expect(ResolveName(context, "hfoo", "string").displayName == "hfoo", "untyped code does not guess object category");
    const auto symbol = "gg_trg_" + Text::ObjectScriptName(trigger.name.text);
    Expect(ResolveName(context, symbol).displayName == trigger.name.text, "Chinese and symbols use shared script name rule");
    triggers.triggers.push_back(trigger); triggers.triggers.back().id = 12;
    const auto ambiguous = ResolveName(context, symbol);
    Expect(ambiguous.state == NameResolutionState::Ambiguous && ambiguous.displayName == symbol && ambiguous.candidates.size() == 2,
        "ambiguous trigger identity is not chosen");
    Expect(ResolveName(context, "udg_Hero").displayName == "Hero", "declared variable resolves");
    Expect(ResolveName(context, "udg_Missing").state == NameResolutionState::MissingDefinition, "missing variable keeps identity");
    Expect(ResolveName(context, "\"TRIGSTR_8\"", "string").displayName == "正文", "quoted WTS parameter resolves");
    Expect(ResolveName(context, "TRIGSTR_99").state == NameResolutionState::MissingString, "missing WTS entry explicit");
    Expect(ResolveName(context, "TRIGSTR_7junk").state == NameResolutionState::Invalid, "only whole decimal reference");
    records[0].parentRawcode = "U002"; records[0].modifications.clear();
    Expect(ResolveObjectName(context, records[1].key).reason.find("循环") != std::string::npos, "parent cycle explicit");
    source.state = ProjectSearchState::Invalid;
    Expect(ResolveObjectName(context, {ObjectType::Unit, "hfoo"}).state == NameResolutionState::Invalid, "unsafe map does not fall back to standard");
}
void PlacedClassification() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    MapData::UnitPlacement item; item.rawcode = "I001"; item.creationId = 7; item.scale = {1, 1, 1};
    units.value->units.push_back(item);
    MapData::DoodadPlacement tree; tree.rawcode = "D001"; tree.creationId = 9; tree.scale = {1, 1, 1};
    doodads.value->doodads.push_back(tree);
    const auto map = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3mapUnits.doo", *units.value->Serialize().value}, {"war3map.doo", *doodads.value->Serialize().value}});
    std::array<ObjectRecord, 2> items;
    items[0].key = {ObjectType::Item, "I001"}; items[1] = items[0];
    ObjectModification name; name.key.fieldId = "name"; name.value = std::string("地图物品");
    items[0].modifications.push_back(name);
    ObjectRecord destructable; destructable.key = {ObjectType::Destructable, "D001"};
    destructable.modifications.push_back(name);
    std::array<ProjectSearchObjectSource, 4> sources{{
        {ObjectType::Unit, {}, ProjectSearchState::Ready},
        {ObjectType::Item, std::span(items.data(), 1), ProjectSearchState::Ready},
        {ObjectType::Destructable, std::span(&destructable, 1), ProjectSearchState::Ready},
        {ObjectType::Doodad, {}, ProjectSearchState::Ready}}};
    NameResolutionContext context; context.mapDocument = &map; context.objects = sources;
    Expect(ResolveName(context, "gg_item_I001_0007").state == NameResolutionState::Resolved,
        "complete competing category permits a unique real item classification");
    for (const auto state : {ProjectSearchState::Invalid, ProjectSearchState::Unavailable,
        ProjectSearchState::Partial}) {
        sources[0].state = state;
        const auto result = ResolveName(context, "gg_item_I001_0007");
        Expect(result.state == NameResolutionState::Invalid && result.displayName == result.rawValue,
            "unreadable competing class is not evidence of absence");
    }
    sources[0].state = ProjectSearchState::Ready;
    sources[1].records = items;
    Expect(ResolveName(context, "gg_item_I001_0007").state == NameResolutionState::Ambiguous,
        "duplicate item definitions retain placement identity and ambiguity");
    sources[1].records = std::span(items.data(), 1);
    sources[0].state = ProjectSearchState::Ambiguous;
    sources[1].state = ProjectSearchState::Invalid;
    Expect(ResolveName(context, "gg_item_I001_0007").state == NameResolutionState::Ambiguous,
        "a later invalid category does not hide an earlier ambiguity");
    sources[0].state = sources[1].state = ProjectSearchState::Ready;
    sources[3].state = ProjectSearchState::Ambiguous;
    Expect(ResolveName(context, "gg_dest_D001_0009").state == NameResolutionState::Ambiguous,
        "ambiguous competing doodad class cannot become a resolved destructable");
    context.objects = std::span(sources).subspan(1);
    Expect(ResolveName(context, "gg_item_I001_0007").state == NameResolutionState::Invalid,
        "unprovided competing category cannot be assumed absent");
}
void DynamicPresentation() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-NameResolution-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root / "base");
    const auto write = [&](const auto& name, auto text) { std::ofstream(root / name, std::ios::binary) << text; };
    write("config", "base\n");
    write("base/define.txt", "[TriggerTypes]\nunitcode=0,1,1,单位,integer\ntypename=1,0,0,类型,string\n"
        "UnitAlias=0,1,1,单位别名,unitcode\n[TriggerParams]\ntypename08_unitcode=0,typename,unitcode,单位类型\nPaladin=0,unitcode,'Hpal',旧预设标签\n");
    write("base/action.txt", "[SaveGeneric]\ndescription=\"保存 ${类型} ${值}\"\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    Expect(catalog.Succeeded(), "synthetic UI loads");
    Trigger::TriggerDocument document;
    Trigger::ArgumentNode selector; selector.id = 1; selector.type = 0; selector.value.text = "typename08_unitcode";
    Trigger::ArgumentNode value; value.id = 2; value.type = 3; value.value.text = "Hpal";
    document.argumentNodes = {selector, value};
    Trigger::EcaNode eca; eca.id = 3; eca.type = 2; eca.name.text = "SaveGeneric"; eca.arguments = {0, 1}; document.ecaNodes.push_back(eca);
    Trigger::TriggerTypeSystem types(*catalog.value);
    Trigger::PresentationOptions options; options.typeSystem = &types;
    options.referenceResolver = [](std::string_view raw, std::string_view type) {
        Trigger::ReferencePresentation resolved;
        resolved.rawValue = raw; resolved.displayName = raw;
        if ((raw == "Hpal" || raw == "'Hpal'") && type == "unitcode") { resolved.displayName = "圣骑士"; resolved.iconPath = "Icons/Paladin.blp"; resolved.state = "resolved"; }
        return resolved;
    };
    const auto result = Trigger::PresentEca(document, 0, *catalog.value, options);
    Expect(result.text.find("圣骑士") != std::string::npos && result.arguments[1].uiType == "unitcode"
        && result.arguments[1].rawValue == "Hpal" && result.arguments[1].iconPath == "Icons/Paladin.blp", "dynamic Null selects typed name and metadata");
    auto standard = Catalog(); NameResolutionContext names; names.standardObjects = &standard; names.types = &types;
    Expect(ResolveName(names, "hfoo", "UnitAlias").displayName == "原版步兵", "common resolver canonicalizes typed object aliases");
    document.argumentNodes[1].type = 0; document.argumentNodes[1].value.text = "Paladin";
    const auto preset = Trigger::PresentEca(document, 0, *catalog.value, options);
    Expect(preset.arguments[1].rawValue == "Paladin" && preset.arguments[1].displayName == "圣骑士"
        && preset.arguments[1].iconPath == "Icons/Paladin.blp", "typed presets show current name/icon and preserve preset identity");
    document.argumentNodes[1].type = 3; document.argumentNodes[1].value.text = "Hpal";
    options.showRawValues = true;
    const auto raw = Trigger::PresentEca(document, 0, *catalog.value, options);
    Expect(raw.text.find("typename08_unitcode") != std::string::npos && raw.text.find("Hpal") != std::string::npos
        && raw.arguments[1].iconPath.empty(), "raw toggle bypasses all resolution");
    std::filesystem::remove_all(root);
}
void StringTransactions() {
    MapArchiveMember wts; wts.name = "war3map.wts"; wts.data = Bytes("// keep\r\nSTRING 1\r\n{\r\n原文\r\n}\r\n");
    auto map = MapDocument::FromMembers(std::span(&wts, 1));
    MapData::MapDataEditSession edits(map);
    Trigger::TriggerDocument triggers;
    Trigger::ArgumentNode first; first.id = 1; first.type = 3; first.value.text = "TRIGSTR_1";
    Trigger::ArgumentNode second = first; second.id = 2;
    triggers.argumentNodes = {first, second};
    NameResolutionContext context; context.mapDocument = &map; context.triggers = &triggers;
    Expect(CountTriggerStringUsage(context, "\"TRIGSTR_1\"").count == 2, "quoted and bare sharing count equally");
    triggers.argumentNodes[1].value.text = "\"TRIGSTR_0001\"";
    Expect(CountTriggerStringUsage(context, "TRIGSTR_001").count == 2, "zero-padded decimal IDs share the same entry");
    triggers.argumentNodes[1].value.text = "TRIGSTR_1";
    auto plan = PrepareTriggerStringEdit(context, 1, "新正文", TriggerStringEditScope::OnlyThisArgument);
    Expect(plan.Succeeded() && plan.value->Usage().count == 2 && plan.value->ChangedReference(), "detach planner counts sharing without mutation");
    auto receipt = ApplyTriggerStringEdit(triggers, edits, *plan.value);
    Expect(receipt.Succeeded() && triggers.argumentNodes[0].value.text == "TRIGSTR_2" && triggers.argumentNodes[1].value.text == "TRIGSTR_1",
        "detach changes only selected reference");
    auto snapshot = edits.Snapshot(MapDataKind::Strings);
    Expect(snapshot.value->ResolveString("TRIGSTR_2").value == std::optional<std::string>("新正文"), "new WTS entry roundtrips");
    Expect(ReplayTriggerStringEdit(triggers, edits, *receipt.value, false).Succeeded() && triggers.argumentNodes[0].value.text == "TRIGSTR_1",
        "one undo restores WTG and WTS");
    Expect(ReplayTriggerStringEdit(triggers, edits, *receipt.value, true).Succeeded(), "one redo restores joint edit");
    triggers.argumentNodes[0].value.text = "TRIGSTR_99";
    const auto before = edits.Snapshot(MapDataKind::Strings).value->Strings()->Serialize();
    Expect(!ReplayTriggerStringEdit(triggers, edits, *receipt.value, false).Succeeded(), "stale argument rejects joint undo");
    Expect(edits.Snapshot(MapDataKind::Strings).value->Strings()->Serialize().value == before.value, "stale undo leaves WTS untouched");
    // 共享修改保留原引用及原 WTS 注释/换行，回读正文一致。
    Trigger::TriggerDocument shared; shared.argumentNodes = {first, second};
    MapData::MapDataEditSession sharedEdits(map); context.triggers = &shared;
    auto all = PrepareTriggerStringEdit(context, 1, "全部正文", TriggerStringEditScope::AllReferences);
    auto changed = ApplyTriggerStringEdit(shared, sharedEdits, *all.value);
    Expect(changed.Succeeded() && shared.argumentNodes[0].value.text == "TRIGSTR_1", "all scope preserves reference");
    Expect(sharedEdits.Snapshot(MapDataKind::Strings).value->ResolveString("TRIGSTR_1").value == std::optional<std::string>("全部正文"), "all scope changes WTS text");
    Expect(!ApplyTriggerStringEdit(shared, sharedEdits, *all.value).Succeeded(), "stale WTS plan rejected");
    // 外层地图撤销历史有后续 WTS 修改时共同撤销拒绝，两份现值都保留。
    auto external = *sharedEdits.Snapshot(MapDataKind::Strings).value->Strings();
    external.entries.front().value = "外部修改";
    Expect(sharedEdits.EditStrings(external).Succeeded(), "external WTS overlay applies");
    Expect(!ReplayTriggerStringEdit(shared, sharedEdits, *changed.value, false).Succeeded()
        && shared.argumentNodes[0].value.text == "TRIGSTR_1"
        && sharedEdits.Snapshot(MapDataKind::Strings).value->ResolveString("TRIGSTR_1").value == std::optional<std::string>("外部修改"),
        "stale WTS joint replay preserves both current values");
    for (const auto encoding : {Trigger::TextEncoding::Gb18030, Trigger::TextEncoding::Latin1}) {
        Trigger::TriggerDocument encoded; encoded.argumentNodes = {first, second};
        encoded.argumentNodes[0].value.encoding = encoding;
        encoded.argumentNodes[0].value.originalBytes = Bytes("TRIGSTR_1");
        const auto beforeBytes = encoded.argumentNodes[1].value.ToBytes();
        context.triggers = &encoded;
        auto isolated = PrepareTriggerStringEdit(context, 1, "新条目", TriggerStringEditScope::OnlyThisArgument);
        Expect(isolated.Succeeded() && isolated.value->After().value.encoding == encoding
            && isolated.value->After().value.originalBytes == Bytes("TRIGSTR_1"), "detach preserves original encoding provenance");
        MapData::MapDataEditSession encodedEdits(map);
        auto encodedReceipt = ApplyTriggerStringEdit(encoded, encodedEdits, *isolated.value);
        Expect(encodedReceipt.Succeeded() && encoded.argumentNodes[0].value.ToBytes().value == std::optional(Bytes("TRIGSTR_2"))
            && encoded.argumentNodes[1].value.ToBytes().value == beforeBytes.value, "only selected wire text changes");
        Expect(ReplayTriggerStringEdit(encoded, encodedEdits, *encodedReceipt.value, false).Succeeded()
            && encoded.argumentNodes[0].value.ToBytes().value == std::optional(Bytes("TRIGSTR_1")), "undo restores source bytes");
    }
}
void LegacyStringBytes() {
    Trigger::RawText body; body.text = "旧中文\r\n第二行"; body.encoding = Trigger::TextEncoding::Gb18030;
    const auto encoded = body.ToBytes(); Expect(encoded.Succeeded(), "encode original GB18030 body");
    auto wire = Bytes("// keep\r\nSTRING 1\r\n{\r\n");
    wire.insert(wire.end(), encoded.value->begin(), encoded.value->end());
    const auto tail = Bytes("\r\n}\r\nSTRING 2\r\n{\r\nASCII\r\n}\r\nSTRING 3\r\n{\r\nUTF8正文\r\n}\r\n");
    wire.insert(wire.end(), tail.begin(), tail.end());
    const MapArchiveMember member{"war3map.wts", wire};
    auto map = MapDocument::FromMembers(std::span(&member, 1));
    Trigger::TriggerDocument triggers;
    Trigger::ArgumentNode argument; argument.id = 1; argument.type = 3; argument.value.text = "TRIGSTR_1";
    triggers.argumentNodes.push_back(argument);
    NameResolutionContext context; context.mapDocument = &map; context.triggers = &triggers;
    Expect(ResolveName(context, "TRIGSTR_1").displayName == "旧中文\r\n第二行", "legacy body is UTF8 display without changing wire");
    Expect(map.Strings()->Serialize().value == std::optional(wire), "name lookup preserves legacy WTS bytes");
    Expect(!PrepareTriggerStringEdit(context, 1, body.text, TriggerStringEditScope::AllReferences).Succeeded(), "unchanged Unicode body does not create history");
    auto plan = PrepareTriggerStringEdit(context, 1, "新中文\r\n第三行", TriggerStringEditScope::AllReferences);
    Expect(plan.Succeeded(), "legacy body plan supports same source encoding");
    MapData::MapDataEditSession edits(map);
    auto receipt = ApplyTriggerStringEdit(triggers, edits, *plan.value);
    Expect(receipt.Succeeded(), "legacy joint edit applies");
    auto current = edits.Snapshot(); context.mapDocument = &current;
    const auto decoded = Trigger::DecodeSourceText(Bytes(current.Strings()->entries[0].value));
    Expect(decoded.encoding == Trigger::TextEncoding::Gb18030 && decoded.text == "新中文\r\n第三行"
        && current.Strings()->entries[1].value == "ASCII", "edited body and other entries retain source encoding/bytes");
    Expect(ReplayTriggerStringEdit(triggers, edits, *receipt.value, false).Succeeded()
        && edits.Snapshot().Strings()->Serialize().value == std::optional(wire), "joint undo restores complete legacy WTS bytes");
    triggers.argumentNodes[0].value.text = "TRIGSTR_2"; context.mapDocument = &map;
    auto ascii = PrepareTriggerStringEdit(context, 1, "独立中文", TriggerStringEditScope::OnlyThisArgument);
    Expect(ascii.Succeeded(), "ASCII source adopts known legacy table encoding");
    MapData::MapDataEditSession asciiEdits(map);
    auto asciiReceipt = ApplyTriggerStringEdit(triggers, asciiEdits, *ascii.value);
    Expect(asciiReceipt.Succeeded(), "ASCII detach applies");
    const auto detached = Trigger::DecodeSourceText(Bytes(asciiEdits.Snapshot().Strings()->entries.back().value));
    Expect(detached.encoding == Trigger::TextEncoding::Gb18030 && detached.text == "独立中文", "new entry is not UTF8 mixed into legacy table");
    triggers.argumentNodes[0].value.text = "TRIGSTR_3";
    auto utf8 = PrepareTriggerStringEdit(context, 1, "UTF8修改", TriggerStringEditScope::AllReferences);
    Expect(utf8.Succeeded(), "mixed table UTF8 body edits independently");
    MapData::MapDataEditSession mixedEdits(map);
    auto mixedReceipt = ApplyTriggerStringEdit(triggers, mixedEdits, *utf8.value);
    const auto mixed = mixedEdits.Snapshot();
    Expect(mixedReceipt.Succeeded() && mixed.Strings()->entries[0].value == map.Strings()->entries[0].value
        && mixed.Strings()->entries[2].value == "UTF8修改", "editing UTF8 entry preserves neighboring GB bytes");
    auto latinWire = Bytes("STRING 1\n{\n"); latinWire.push_back(std::byte{0xff});
    const auto latinTail = Bytes("\n}\n"); latinWire.insert(latinWire.end(), latinTail.begin(), latinTail.end());
    const MapArchiveMember latinMember{"war3map.wts", latinWire};
    const auto latinMap = MapDocument::FromMembers(std::span(&latinMember, 1));
    triggers.argumentNodes[0].value.text = "TRIGSTR_1"; context.mapDocument = &latinMap;
    MapData::MapDataEditSession latinEdits(latinMap);
    Expect(Trigger::DecodeSourceText(Bytes(latinMap.Strings()->entries[0].value)).encoding == Trigger::TextEncoding::Latin1,
        "actual undecodable source body retains Latin1 provenance");
    Expect(!PrepareTriggerStringEdit(context, 1, "不能用Latin1表示", TriggerStringEditScope::AllReferences).Succeeded()
        && latinEdits.Revision() == 0 && latinEdits.Snapshot().Strings()->Serialize().value == std::optional(latinWire),
        "unrepresentable body edit refuses before either history or source changes");
}
void SharedCoverage() {
    const auto settingsSource = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}});
    Expect(settingsSource.Settings().has_value(), "settings fixture parses");
    auto settings = *settingsSource.Settings();
    settings.mapName = settings.loadingScreen.title = settings.prologueScreen.text = settings.environment.soundEnvironment = "TRIGSTR_001";
    std::size_t expected = 7 + 4;
    for (auto& player : settings.players) { player.name = "TRIGSTR_1"; ++expected; }
    for (auto& force : settings.forces) { force.name = "TRIGSTR_1"; ++expected; }
    if (settings.custom) {
        for (auto& table : settings.custom->randomUnits) { table.name = "TRIGSTR_1"; ++expected; }
        for (auto& table : settings.custom->randomItems) { table.name = "TRIGSTR_1"; ++expected; }
    }
    MapData::RegionsDocument regions; MapData::RegionRecord region{};
    region.name = region.ambientSound = "TRIGSTR_1"; regions.entries.push_back(region); expected += 2;
    MapData::CamerasDocument cameras; MapData::CameraRecord camera{};
    camera.name = "TRIGSTR_1"; cameras.entries.push_back(camera); ++expected;
    MapData::SoundsDocument sounds; MapData::SoundRecord sound{};
    sound.name = sound.filePath = sound.eaxSetting = "TRIGSTR_1"; sound.pitch = 1; sounds.entries.push_back(sound); expected += 3;
    const auto sb = settings.Serialize(), rb = regions.Serialize(), cb = cameras.Serialize(), ab = sounds.Serialize();
    Expect(sb.Succeeded() && rb.Succeeded() && cb.Succeeded() && ab.Succeeded(), "all shared auxiliary fixtures serialize");
    const auto map = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", *sb.value},
        {"war3map.w3r", *rb.value}, {"war3map.w3c", *cb.value}, {"war3map.w3s", *ab.value}, {"war3map.wts", Bytes("STRING 1\n{\nShared\n}\n")}});
    std::array<ObjectRecord, 7> objects;
    std::array<ProjectSearchObjectSource, 7> sources;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objects[index].key = {static_cast<ObjectType>(index), "A001"};
        ObjectModification modification; modification.key.fieldId = "name"; modification.value = std::string("TRIGSTR_1");
        objects[index].modifications.push_back(modification);
        sources[index] = {static_cast<ObjectType>(index), std::span(&objects[index], 1), ProjectSearchState::Ready};
    }
    Trigger::TriggerDocument triggers;
    NameResolutionContext context; context.mapDocument = &map; context.objects = sources; context.triggers = &triggers;
    auto usage = CountTriggerStringUsage(context, "\"TRIGSTR_0001\"");
    Expect(usage.count == expected, "sharing count includes seven object classes, W3I, regions, cameras and sounds");
    Expect(usage.complete, "classic auxiliary records do not require absent extended-version fields");
    sources[4].state = ProjectSearchState::Partial;
    Expect(!CountTriggerStringUsage(context, "TRIGSTR_1").complete, "partial object coverage is explicitly incomplete");
}
} // namespace
int main() {
    try { Names(); PlacedClassification(); DynamicPresentation(); StringTransactions(); LegacyStringBytes(); SharedCoverage(); std::cout << "name resolution and joint string transactions passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
