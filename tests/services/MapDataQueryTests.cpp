#include "War3MapCore/Services/MapDataQuery.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Bytes {
    std::vector<std::byte> data;
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            data.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
    void Raw(std::string_view text) {
        for (const char c : text) data.push_back(static_cast<std::byte>(c));
    }
    void String(std::string_view text) { Raw(text); data.push_back(std::byte{}); }
    void Real(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
};

std::string FieldRaw(const MapDataQueryRow& row, std::string_view id) {
    for (const auto& field : row.fields)
        if (field.descriptor->id == id) return MapData::FormatMapFieldRaw(field.value);
    return {};
}

template<class T>
bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

MapDataQueryBinding UnitBinding() {
    return {"unit-filter-document", 4, 7, std::string(64, 'b')};
}

MapData::UnitPlacements UnitTable(std::size_t count) {
    MapData::UnitPlacements units;
    for (std::size_t i = 0; i < count; ++i) {
        MapData::UnitPlacement unit;
        unit.rawcode = "hrif";
        unit.flags = 7;
        unit.owner = 0;
        unit.creationId = static_cast<std::int32_t>(i + 1);
        unit.position = {static_cast<float>(i), 32, 0};
        unit.scale = {1, 1, 1};
        unit.random = MapData::RandomAnyUnit{4, 0};
        units.units.push_back(std::move(unit));
    }
    return units;
}

MapDataQuerySnapshot UnitSnapshot(const MapData::UnitPlacements& units,
    const MapDataQueryBinding& binding = UnitBinding()) {
    const auto wire = units.Serialize();
    Require(wire.Succeeded(), "原创单位夹具应经真实 writer 序列化");
    auto captured = CaptureMapDataSnapshot(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3mapUnits.doo", *wire.value}}), binding, false);
    Require(captured.Succeeded(), "单位筛选应取得冻结原生快照");
    const auto described = DescribeMapData(*captured.value, binding);
    Require(described.Succeeded() && described.value->members[1].state == MapDataQueryState::Ready,
        "单位夹具须为完整可查询的经典来源");
    return std::move(*captured.value);
}

std::vector<std::uint64_t> Indices(const MapDataUnitQueryPage& page) {
    std::vector<std::uint64_t> result;
    for (const auto& row : page.rows) result.push_back(row.sourceIndex);
    return result;
}

void TestBoundedSnapshot() {
    Bytes region;
    region.U32(5); region.U32(2);
    for (int i = 0; i < 2; ++i) {
        for (const float value : {0.f, 0.f, 512.f, 512.f}) region.Real(value);
        region.String(i == 0 ? "TRIGSTR_12" : "TRIGSTR_13");
        region.U32(42 + i); region.U32(0); region.String(""); region.U32(0);
    }
    Bytes wts;
    wts.Raw("STRING 12\n{\nNorth\n}\nSTRING 13\n{\nSouth\n}\n");
    Bytes sound;
    sound.U32(1); sound.U32(1);
    sound.String("gg_snd_GLHF"); sound.String("Sound\\GLHF.wav"); sound.String("DefaultEAXON");
    sound.U32(0); sound.U32(0xffffffffu); sound.U32(0); sound.U32(137);
    sound.Real(1.f); sound.Real(0.f); sound.U32(0); sound.U32(0);
    for (const float value : {std::bit_cast<float>(MapData::kSoundDefaultRealBits), 2000.f, 2500.f,
            90.f, 180.f}) sound.Real(value);
    sound.U32(67); sound.Real(0.f); sound.Real(0.f); sound.Real(1.f);
    std::vector<MapArchiveMember> members{{"war3map.w3r", region.data},
        {"war3map.wts", wts.data}, {"war3map.w3c", {std::byte{'x'}}},
        {"war3map.w3s", sound.data}};
    MapDataQueryBinding binding{"document-1", 7, 3, std::string(64, '0')};
    auto capture = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, true);
    Require(capture.Succeeded(), "snapshot capture");
    auto described = DescribeMapData(*capture.value, binding);
    Require(described.Succeeded(), "snapshot description");
    Require(described.value->members[3].state == MapDataQueryState::Ready
        && described.value->members[3].recordCount == 2, "present parsed regions");
    Require(described.value->members[4].state == MapDataQueryState::Invalid
        && !described.value->members[4].recordCount, "invalid member is not empty");
    Require(described.value->members[0].state == MapDataQueryState::OpaqueUnknown
        && !described.value->members[0].recordCount, "opaque missing member is unknown");
    auto sounds = QueryMapDataPage(*capture.value, binding, MapDataKind::Sounds);
    Require(sounds.Succeeded() && sounds.value->rows.size() == 1
        && sounds.value->rows[0].name.rawBytes == "gg_snd_GLHF"
        && FieldRaw(sounds.value->rows[0], "volume") == "137"
        && sounds.value->rows[0].rowKey == "sound:67675f736e645f474c4846", "sound row keeps source int32 volume and key");
    const auto checkDisplay = [&](std::string_view id, std::string_view raw, bool isDefault) {
        const auto& fields = sounds.value->rows[0].fields;
        const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& item) { return item.descriptor->id == id; });
        Require(field != fields.end() && MapData::FormatMapFieldRaw(field->value) == raw &&
            MapData::IsMapFieldDefault(*field->descriptor, field->value) == isDefault &&
            MapData::MapFieldDisplayNumber(*field->descriptor, field->value).has_value() != isDefault,
            "sound default display keeps the raw previous value");
    };
    checkDisplay("fade_in", "-1", true);
    checkDisplay("min_distance", "f32:4f800000", true);
    checkDisplay("volume", "137", false);
    Bytes extended = sound;
    extended.data[0] = std::byte{2};
    extended.String("gg_snd_GLHF"); extended.String("Dialogue"); extended.String("Sound\\GLHF.wav");
    extended.U32(0xffffffffu); extended.String("unknown2"); extended.U32(23); extended.U32(0);
    extended.String("unknown6"); extended.String("unknown7");
    extended.String("Face"); extended.String("Group"); extended.String("Animation\\Face.fdf");
    auto extendedMembers = members;
    Bytes extendedCamera;
    extendedCamera.U32(0); extendedCamera.U32(1);
    for (int value = 0; value < 10; ++value) extendedCamera.Real(static_cast<float>(value + 1));
    extendedCamera.Real(11.f); extendedCamera.Real(12.f); extendedCamera.Real(13.f); extendedCamera.String("Look");
    extendedMembers[2].data = extendedCamera.data;
    extendedMembers.back().data = extended.data;
    MapDataQueryBinding extendedBinding{"document-v2", 8, 0, std::string(64, '1')};
    auto extendedSnapshot = CaptureMapDataSnapshot(MapDocument::FromMembers(extendedMembers), extendedBinding, false);
    Require(extendedSnapshot.Succeeded(), "extended sound snapshot capture");
    auto extendedDescription = DescribeMapData(*extendedSnapshot.value, extendedBinding);
    auto extendedPage = QueryMapDataPage(*extendedSnapshot.value, extendedBinding, MapDataKind::Sounds);
    auto cameraPage = QueryMapDataPage(*extendedSnapshot.value, extendedBinding, MapDataKind::Cameras);
    Require(extendedDescription.Succeeded() && extendedDescription.value->members[4].state == MapDataQueryState::Partial
        && cameraPage.Succeeded() && cameraPage.value->rows.size() == 1
        && FieldRaw(cameraPage.value->rows[0], "extended_local_yaw") == "f32:41400000"
        && std::all_of(cameraPage.value->rows[0].fields.begin(), cameraPage.value->rows[0].fields.end(),
            [](const auto& field) { return !field.writable; }),
        "Agent camera query exposes extended angles without suggesting that v0 extension is writable");
    Require(extendedDescription.Succeeded() && extendedDescription.value->members[5].state == MapDataQueryState::Partial
        && extendedPage.Succeeded() && extendedPage.value->rows.size() == 1
        && FieldRaw(extendedPage.value->rows[0], "extended_dialogue_text_key") == "-1"
        && FieldRaw(extendedPage.value->rows[0], "extended_unknown4") == "0"
        && FieldRaw(extendedPage.value->rows[0], "extended_unknown5").empty()
        && FieldRaw(extendedPage.value->rows[0], "extended_unknown11").empty()
        && std::all_of(extendedPage.value->rows[0].fields.begin(), extendedPage.value->rows[0].fields.end(),
            [](const auto& field) { return !field.writable; }),
        "Agent sound query exposes v2 fields but never advertises writes on partial source");
    const auto& dialogueFields = extendedPage.value->rows[0].fields;
    const auto dialogue = std::find_if(dialogueFields.begin(), dialogueFields.end(), [](const auto& field) {
        return field.descriptor->id == "extended_dialogue_text_key";
    });
    Require(dialogue != dialogueFields.end()
        && !MapData::IsMapFieldDefault(*dialogue->descriptor, dialogue->value)
        && MapData::MapFieldDisplayNumber(*dialogue->descriptor, dialogue->value) == -1.0,
        "unknown extended integer -1 remains a raw value, not a default sentinel");
    auto strings = QueryMapDataPage(*capture.value, binding, MapDataKind::Strings, std::nullopt, 1);
    Require(strings.Succeeded() && strings.value->rows.size() == 1 &&
        strings.value->rows[0].rowKey == "string:12" &&
        FieldRaw(strings.value->rows[0], "id") == "12" &&
        FieldRaw(strings.value->rows[0], "value") == "hex:4e6f727468",
        "string-table rows expose read-only exact values through the shared field catalog");
    auto first = QueryMapDataPage(*capture.value, binding, MapDataKind::Regions, std::nullopt, 1);
    Require(first.Succeeded() && first.value->rows.size() == 1 && first.value->next,
        "bounded first page");
    Require(first.value->rows[0].sourceIndex == 0 && first.value->rows[0].creationNumber == 42
        && first.value->rows[0].name.rawBytes == "TRIGSTR_12"
        && first.value->rows[0].name.displayBytes == "North"
        && first.value->rows[0].name.state == MapDataQueryTextState::Resolved,
        "raw reference and resolved value coexist");
    Require(FieldRaw(first.value->rows[0], "left") == "f32:00000000"
        && FieldRaw(first.value->rows[0], "right") == "f32:44000000", "region bounds keep exact bits");
    auto second = QueryMapDataPage(*capture.value, binding, MapDataKind::Regions, first.value->next, 1);
    Require(second.Succeeded() && second.value->rows[0].sourceIndex == 1
        && second.value->rows[0].name.displayBytes == "South" && !second.value->next,
        "cursor continues same revision");
    auto stale = binding; ++stale.revision;
    Require(!DescribeMapData(*capture.value, stale).Succeeded()
        && !QueryMapDataPage(*capture.value, stale, MapDataKind::Regions).Succeeded(),
        "revision mismatch rejects read");
    stale = binding; stale.sourceSha256[0] = 'a';
    Require(!QueryMapDataPage(*capture.value, stale, MapDataKind::Regions).Succeeded(),
        "source SHA mismatch rejects read");
    auto badCursor = *first.value->next; ++badCursor.binding.generation;
    Require(!QueryMapDataPage(*capture.value, binding, MapDataKind::Regions, badCursor).Succeeded()
        && !QueryMapDataPage(*capture.value, binding, MapDataKind::Cameras).Succeeded()
        && !QueryMapDataPage(*capture.value, binding, MapDataKind::Settings).Succeeded(),
        "stale cursor and unavailable members fail closed");
    auto namedOnly = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, false);
    Require(namedOnly.Succeeded() && DescribeMapData(*namedOnly.value, binding).value->members[0].state
        == MapDataQueryState::Missing, "known absent member stays distinct from opaque unknown");
    Require(QueryMapDataPage(*namedOnly.value, binding, MapDataKind::Settings).Succeeded(),
        "known absent member has an explicit empty page");
    Bytes camera; camera.U32(0); camera.U32(1);
    for (const float value : {13.5f, -42.5f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f}) camera.Real(value);
    camera.String("view"); members[2].data = camera.data;
    auto withCamera = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, false);
    Require(withCamera.Succeeded(), "camera snapshot");
    auto cameras = QueryMapDataPage(*withCamera.value, binding, MapDataKind::Cameras);
    Require(cameras.Succeeded() && cameras.value->rows.size() == 1
        && FieldRaw(cameras.value->rows[0], "target_x") == "f32:41580000"
        && FieldRaw(cameras.value->rows[0], "target_y") == "f32:c22a0000",
        "camera target retains source values and bits");
    Bytes doodad; doodad.Raw("W3do"); doodad.U32(8); doodad.U32(11); doodad.U32(1);
    doodad.Raw("B00A"); doodad.U32(3);
    for (const float value : {-0.f, 64.f, 128.f, 1.f, 1.f, 1.25f, 1.f}) doodad.Real(value);
    doodad.data.push_back(std::byte{0x11}); doodad.data.push_back(std::byte{75});
    doodad.U32(0xffffffffu); doodad.U32(0); doodad.U32(7); doodad.U32(0); doodad.U32(1);
    doodad.Raw("S00A"); doodad.U32(2); doodad.U32(0xfffffffdu); doodad.U32(9);
    members.push_back({"war3map.doo", doodad.data});
    auto withDoodads = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, false);
    Require(withDoodads.Succeeded(), "doodad snapshot");
    auto normal = QueryMapDataPage(*withDoodads.value, binding, MapDataKind::Doodads, std::nullopt, 1);
    Require(normal.Succeeded() && normal.value->rows.size() == 1 && normal.value->next
        && normal.value->rows[0].rowKey == "doodad:42303041:7"
        && FieldRaw(normal.value->rows[0], "x") == "f32:80000000"
        && FieldRaw(normal.value->rows[0], "grid_x").empty(), "normal doodad has exact world fields only");
    auto special = QueryMapDataPage(*withDoodads.value, binding, MapDataKind::Doodads, normal.value->next, 1);
    Require(special.Succeeded() && special.value->rows.size() == 1 && !special.value->next
        && special.value->rows[0].rowKey == "special:53303041:-3:9"
        && FieldRaw(special.value->rows[0], "grid_x") == "-3"
        && FieldRaw(special.value->rows[0], "facing").empty(), "special doodad has exact grid fields only");
}
void TestOwnedSnapshot() {
    Bytes wts; wts.Raw("STRING 12\n{\nOwned\n}\n");
    const std::vector<MapArchiveMember> members{{"war3map.wts", wts.data}, {"war3map.w3c", {std::byte{'x'}}}};
    auto document = std::make_shared<const MapDocument>(MapDocument::FromMembers(members));
    const auto* identity = document.get();
    const MapDataQueryBinding binding{"owned-document", 3, 9, std::string(64, 'a')};
    auto captured = CaptureMapDataSnapshot(document, binding, true);
    Require(captured.Succeeded() && captured.value->OwnedDocument().get() == identity,
        "owned capture retains the supplied document without copying it");
    auto snapshot = *captured.value;
    captured.value.reset(); document.reset();
    Require(snapshot.OwnedDocument().get() == identity, "copied snapshot owns its document after callers release it");
    const auto page = QueryMapDataPage(snapshot, binding, MapDataKind::Strings);
    Require(page.Succeeded() && page.value->rows.size() == 1
        && FieldRaw(page.value->rows[0], "value") == "hex:4f776e6564", "owned source remains readable");
    const auto described = DescribeMapData(snapshot, binding);
    const auto valueCapture = CaptureMapDataSnapshot(MapDocument::FromMembers(members), binding, true);
    Require(described.Succeeded() && valueCapture.Succeeded(), "both capture forms describe the same source");
    const auto valueDescription = DescribeMapData(*valueCapture.value, binding);
    Require(valueDescription.Succeeded(), "value capture description");
    for (std::size_t i = 0; i < described.value->members.size(); ++i) {
        const auto& owned = described.value->members[i];
        const auto& copied = valueDescription.value->members[i];
        Require(owned.state == copied.state && owned.memberSha256 == copied.memberSha256
            && owned.recordCount == copied.recordCount && owned.diagnosticCount == copied.diagnosticCount,
            "ownership reuse does not bypass hashes or member-state checks");
    }
    auto stale = binding; ++stale.revision;
    Require(!QueryMapDataPage(snapshot, stale, MapDataKind::Strings).Succeeded()
        && !QueryMapDataPage(snapshot, binding, MapDataKind::Cameras).Succeeded()
        && !QueryMapDataPage(snapshot, binding, MapDataKind::Settings).Succeeded(),
        "owned capture keeps stale, invalid and opaque-member rejection");
    const auto missing = CaptureMapDataSnapshot(std::shared_ptr<const MapDocument>{}, binding, false);
    Require(!missing.Succeeded() && !missing.diagnostics.empty()
        && missing.diagnostics.front().code == "MAP_DATA_QUERY_DOCUMENT_MISSING", "missing owner fails explicitly");
}

void TestExactUnitFiltersAndReadonlySnapshot() {
    auto units = UnitTable(7);
    units.units[0].rawcode = "hfoo";
    units.units[1].rawcode = "Hfoo";
    units.units[2].rawcode = "hfoo"; units.units[2].owner = 7;
    units.units[3].owner = -3;
    units.units[4].rawcode = "hfoo"; units.units[4].owner = std::numeric_limits<std::int32_t>::min();
    units.units[5].rawcode = "hfoo"; units.units[5].owner = std::numeric_limits<std::int32_t>::max();
    units.units[6].rawcode = std::string("h\0o\xff", 4);
    const auto snapshot = UnitSnapshot(units);
    const auto* identity = snapshot.OwnedDocument().get();
    const auto nativeBefore = snapshot.OwnedDocument()->Units()->SerializeOriginal();
    const auto before = DescribeMapData(snapshot, snapshot.Binding());
    const auto check = [&](MapDataUnitFilter filter, std::vector<std::uint64_t> expected) {
        const auto page = QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter);
        Require(page.Succeeded() && Indices(*page.value) == expected && !page.value->next
            && page.value->matchedTotal == expected.size() && page.value->examined == units.units.size()
            && !page.value->scanBudgetExhausted, "精确筛选须保有原索引、可信完整总数和有界扫描");
        for (const auto& row : page.value->rows)
            Require(row.rawcodeBytes == units.units[row.sourceIndex].rawcode
                && row.owner == units.units[row.sourceIndex].owner && !row.fields.empty() && !row.rowKey.empty(),
                "命中行须复用完整原生字段 DTO");
    };
    check({"hfoo", {}}, {0, 2, 4, 5});
    check({"Hfoo", {}}, {1});
    check({"hfoo", 7}, {2});
    check({{}, -3}, {3});
    check({{}, std::numeric_limits<std::int32_t>::min()}, {4});
    check({{}, std::numeric_limits<std::int32_t>::max()}, {5});
    check({std::string("h\0o\xff", 4), {}}, {6});
    check({"none", {}}, {});
    const auto ordinary = QueryMapDataPage(snapshot, snapshot.Binding(), MapDataKind::Units);
    Require(ordinary.Succeeded() && ordinary.value->rows.size() == units.units.size(),
        "新增精确筛选不能改变原无过滤查询合同");
    const auto after = DescribeMapData(snapshot, snapshot.Binding());
    Require(before.Succeeded() && after.Succeeded() && snapshot.OwnedDocument().get() == identity
        && snapshot.Binding() == UnitBinding() && snapshot.OwnedDocument()->Units()->SerializeOriginal() == nativeBefore
        && before.value->members[1].memberSha256 == after.value->members[1].memberSha256
        && before.value->members[1].state == after.value->members[1].state,
        "所有筛选只读且不替换自有快照、身份、成员状态或原始字节");
}

void TestSparseUnitPaginationAndUnknownTotals() {
    auto units = UnitTable(10);
    units.units[4].rawcode = "hfoo";
    units.units[8].rawcode = "hfoo";
    const auto snapshot = UnitSnapshot(units);
    const MapDataUnitFilter filter{"hfoo", 0};
    std::optional<MapDataUnitQueryCursor> cursor;
    std::vector<std::uint64_t> observed;
    bool sawEmptyContinuation = false, sawFinal = false;
    for (std::size_t request = 0; request < 10; ++request) {
        const auto page = QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor, 1, 3);
        Require(page.Succeeded() && !page.value->matchedTotal && page.value->examined <= 3
            && page.value->rows.size() <= 1, "大于扫描预算的表每页都须报告未知总数且保持硬界");
        const auto rows = Indices(*page.value);
        observed.insert(observed.end(), rows.begin(), rows.end());
        if (page.value->rows.empty() && page.value->next) {
            Require(page.value->scanBudgetExhausted, "稀疏空页须明确扫描预算耗尽而非全表无匹配");
            sawEmptyContinuation = true;
        }
        if (!page.value->next) { sawFinal = true; break; }
        Require(page.value->next->memberSha256 == page.value->member.memberSha256
            && page.value->next->filter == filter && page.value->next->source.binding == snapshot.Binding()
            && (!cursor || page.value->next->source.nextIndex > cursor->source.nextIndex),
            "续页须向前推进原生索引并绑定过滤条件、成员 SHA 与身份");
        cursor = page.value->next;
    }
    Require(sawEmptyContinuation && sawFinal && observed == std::vector<std::uint64_t>{4, 8},
        "稀疏分页须无重叠、无遗漏且终页仍不伪造全局总数");
    auto small = UnitTable(5);
    small.units[1].rawcode = "hfoo"; small.units[4].rawcode = "hfoo";
    const auto smallSnapshot = UnitSnapshot(small);
    const auto first = QueryMapDataUnitsPage(smallSnapshot, smallSnapshot.Binding(), filter, {}, 1, 5);
    Require(first.Succeeded() && first.value->matchedTotal == 2 && first.value->examined == 5
        && Indices(*first.value) == std::vector<std::uint64_t>{1} && first.value->next,
        "小表即使页满也须在单次完整扫描后提供全局总数");
    const auto second = QueryMapDataUnitsPage(smallSnapshot, smallSnapshot.Binding(), filter, first.value->next, 1, 5);
    Require(second.Succeeded() && second.value->matchedTotal == 2 && second.value->examined == 5
        && Indices(*second.value) == std::vector<std::uint64_t>{4} && !second.value->next,
        "小表续页仍从零独立计数且保留原索引");
}

void TestUnitScanBoundaryAndLimits() {
    auto units = UnitTable(4097);
    units.units[4095].rawcode = "hfoo"; units.units[4096].rawcode = "hfoo";
    const auto snapshot = UnitSnapshot(units);
    const MapDataUnitFilter filter{"hfoo", {}};
    const auto first = QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter);
    Require(first.Succeeded() && first.value->examined == 4096 && first.value->scanBudgetExhausted
        && !first.value->matchedTotal && Indices(*first.value) == std::vector<std::uint64_t>{4095}
        && first.value->next && first.value->next->source.nextIndex == 4096,
        "默认硬界须包含第4096条检查并把下一条留给续页");
    const auto second = QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, first.value->next);
    Require(second.Succeeded() && second.value->examined == 1 && !second.value->matchedTotal
        && Indices(*second.value) == std::vector<std::uint64_t>{4096} && !second.value->next,
        "大表最后一条命中仍不得将未知总数升级为精确总数");
    units.units.pop_back();
    const auto bounded = UnitSnapshot(units);
    const auto complete = QueryMapDataUnitsPage(bounded, bounded.Binding(), filter);
    Require(complete.Succeeded() && complete.value->examined == 4096 && complete.value->matchedTotal == 1
        && !complete.value->next && !complete.value->scanBudgetExhausted,
        "恰好硬界大小的全表可以一次计得可信总数");
    for (const auto limit : {std::size_t{0}, std::size_t{129}})
        Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, {}, limit), "MAP_DATA_QUERY_LIMIT"),
            "单位页长不能超过原128条上限");
    for (const auto examined : {std::size_t{0}, std::size_t{4097}})
        Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, {}, 64, examined), "MAP_DATA_QUERY_LIMIT"),
            "调用者不能扩大单位扫描硬界");
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, {}, 64, 4096, 252 * 1024 + 1),
        "MAP_DATA_QUERY_LIMIT"), "单位逻辑正文不能超过252KiB");
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), {"foo", {}}), "MAP_DATA_UNIT_FILTER_INVALID")
        && HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), {"hfooo", {}}), "MAP_DATA_UNIT_FILTER_INVALID"),
        "类型筛选必须是精确四字节而非长度猜测");
}

void TestUnitCursorRejectsFilterMemberPresenceAndRevisionDrift() {
    auto units = UnitTable(3);
    const auto snapshot = UnitSnapshot(units);
    const MapDataUnitFilter filter{"hrif", 0};
    const auto first = QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, {}, 1);
    Require(first.Succeeded() && first.value->next, "游标门禁夹具须产生真实续页");
    auto cursor = *first.value->next;
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), {"Hrif", 0}, cursor), "MAP_DATA_QUERY_CURSOR_INVALID")
        && HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), {"hrif", 1}, cursor), "MAP_DATA_QUERY_CURSOR_INVALID"),
        "续页必须绑定规范化后的类型和owner精确条件");
    cursor.memberSha256 = std::string(64, '0');
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor), "MAP_DATA_QUERY_MEMBER_STALE"),
        "成员 SHA 不能只依赖宿主文档binding");
    cursor = *first.value->next; cursor.memberSha256.reset();
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor), "MAP_DATA_QUERY_MEMBER_STALE"),
        "已有成员不能接受Missing presence游标");
    cursor = *first.value->next; cursor.source.kind = MapDataKind::Doodads;
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor), "MAP_DATA_QUERY_CURSOR_INVALID"),
        "单位游标不能跨成员类别");
    cursor = *first.value->next; cursor.source.nextIndex = units.units.size() + 1;
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor), "MAP_DATA_QUERY_CURSOR_INVALID"),
        "原索引游标不能越过表尾");
    auto stale = snapshot.Binding(); ++stale.revision;
    Require(HasCode(QueryMapDataUnitsPage(snapshot, stale, filter, first.value->next), "MAP_DATA_QUERY_STALE"),
        "查询期望修订必须匹配冻结快照");
    cursor = *first.value->next; ++cursor.source.binding.generation;
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, cursor), "MAP_DATA_QUERY_STALE"),
        "续页世代不能在重新打开后沿用");
    units.units[0].owner = 1;
    const auto changed = UnitSnapshot(units, snapshot.Binding());
    Require(HasCode(QueryMapDataUnitsPage(changed, changed.Binding(), filter, first.value->next), "MAP_DATA_QUERY_MEMBER_STALE"),
        "即使宿主误复用binding，新native成员仍必须拒绝旧SHA游标");
    const auto missing = CaptureMapDataSnapshot(MapDocument::FromMembers({}), UnitBinding(), false);
    Require(missing.Succeeded(), "Missing原来源应可捕获");
    const auto absent = QueryMapDataUnitsPage(*missing.value, missing.value->Binding(), filter);
    Require(absent.Succeeded() && absent.value->member.state == MapDataQueryState::Missing
        && absent.value->rows.empty() && absent.value->matchedTotal == 0 && !absent.value->next,
        "实际Missing须显式区别于未知来源并报告零匹配");
    const MapDataUnitQueryCursor absentCursor{{missing.value->Binding(), MapDataKind::Units, 0}, filter, {}};
    Require(HasCode(QueryMapDataUnitsPage(snapshot, snapshot.Binding(), filter, absentCursor), "MAP_DATA_QUERY_MEMBER_STALE")
        && HasCode(QueryMapDataUnitsPage(*missing.value, missing.value->Binding(), filter, first.value->next),
            "MAP_DATA_QUERY_MEMBER_STALE"), "Missing与已有成员双向漂移须拒绝");
}

void TestUnitSourceStatesAndLogicalBudget() {
    const auto binding = UnitBinding();
    const MapDataUnitFilter filter{"hrif", {}};
    const auto opaque = CaptureMapDataSnapshot(MapDocument::FromMembers({}), binding, true);
    Require(opaque.Succeeded() && HasCode(QueryMapDataUnitsPage(*opaque.value, binding, filter),
        "MAP_DATA_QUERY_MEMBER_UNAVAILABLE"), "OpaqueUnknown不能伪装为空单位表");
    const auto invalid = CaptureMapDataSnapshot(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3mapUnits.doo", {std::byte{'x'}}}}), binding, false);
    Require(invalid.Succeeded() && HasCode(QueryMapDataUnitsPage(*invalid.value, binding, filter),
        "MAP_DATA_QUERY_MEMBER_UNAVAILABLE"), "Invalid不能返回伪空筛选");
    auto units = UnitTable(2);
    const auto wire = units.Serialize();
    Require(wire.Succeeded(), "冲突夹具应能写出原native");
    const auto ambiguous = CaptureMapDataSnapshot(MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3mapUnits.doo", *wire.value}, {"WAR3MAPUNITS.DOO", *wire.value}}), binding, false);
    Require(ambiguous.Succeeded() && HasCode(QueryMapDataUnitsPage(*ambiguous.value, binding, filter),
        "MAP_DATA_QUERY_MEMBER_UNAVAILABLE"), "Ambiguous不能任取一份原native筛选");
    const auto snapshot = UnitSnapshot(units);
    const auto first = QueryMapDataUnitsPage(snapshot, binding, filter, {}, 1);
    Require(first.Succeeded() && first.value->next, "正文预算夹具须有未返回命中");
    const auto bounded = QueryMapDataUnitsPage(snapshot, binding, filter, {}, 128, 4096, first.value->logicalBytes);
    Require(bounded.Succeeded() && bounded.value->rows.size() == 1 && bounded.value->matchedTotal == 2
        && bounded.value->next && bounded.value->next->source.nextIndex == 1,
        "正文停止时未返回命中必须留在原游标且小表计数仍完整");
    const auto tail = QueryMapDataUnitsPage(snapshot, binding, filter, bounded.value->next, 128, 4096, first.value->logicalBytes);
    Require(tail.Succeeded() && Indices(*tail.value) == std::vector<std::uint64_t>{1} && !tail.value->next,
        "正文预算续页不得漏掉或重复命中原行");
    Require(HasCode(QueryMapDataUnitsPage(snapshot, binding, filter, {}, 64, 4096, 512), "MAP_DATA_QUERY_BUDGET"),
        "单个命中完整字段不能容纳时必须显式拒绝");
    const auto noMatch = QueryMapDataUnitsPage(snapshot, binding, {"none", {}}, {}, 64, 4096, 512);
    Require(noMatch.Succeeded() && noMatch.value->rows.empty() && noMatch.value->matchedTotal == 0,
        "未命中单位不应展开完整字段或消耗行正文预算");
    units.units[1].creationId = units.units[0].creationId;
    const auto partialWire = units.Serialize();
    Require(partialWire.Succeeded(), "Partial来源夹具仍为完整native记录流");
    const auto partial = CaptureMapDataSnapshot(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3mapUnits.doo", *partialWire.value}}), binding, false);
    const auto page = partial.Succeeded() ? QueryMapDataUnitsPage(*partial.value, binding, filter)
        : Result<MapDataUnitQueryPage>{};
    Require(page.Succeeded() && page.value->member.state == MapDataQueryState::Partial && page.value->matchedTotal == 2
        && page.value->rows.size() == 2 && std::all_of(page.value->rows.begin(), page.value->rows.end(), [](const auto& row) {
            return std::none_of(row.fields.begin(), row.fields.end(), [](const auto& field) { return field.writable; });
        }), "Partial来源保留计数及行，不能伪装Ready或授予字段写入");
}
} // namespace

int main() {
    try {
        TestBoundedSnapshot();
        TestOwnedSnapshot();
        TestExactUnitFiltersAndReadonlySnapshot();
        TestSparseUnitPaginationAndUnknownTotals();
        TestUnitScanBoundaryAndLimits();
        TestUnitCursorRejectsFilterMemberPresenceAndRevisionDrift();
        TestUnitSourceStatesAndLogicalBudget();
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Map data bounded snapshot checks passed\n";
}
