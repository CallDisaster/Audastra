#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapData/MapDataFieldPatchCodec.h"
#include "War3MapCore/Hash.h"
#include "LniMapSettingsFixtures.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& item) { return item.code == code; });
}
std::string Sha(const MapDocument& document, MapDataKind kind) {
    const auto value = Sha256(document.Member(kind).sources.front().data); Require(value.Succeeded(), "hash actual native member"); return *value.value;
}
std::vector<MapArchiveMember> Members(std::size_t regionCount = 2) {
    RegionsDocument regions;
    for (std::size_t row = 0; row < regionCount; ++row) {
        RegionRecord region{}; region.left = -10; region.bottom = -20; region.right = 30; region.top = 40;
        region.name = row == 0 ? "TRIGSTR_0042" : "Region " + std::to_string(row);
        region.creationNumber = static_cast<std::int32_t>(row + 1); regions.entries.push_back(std::move(region));
    }
    CamerasDocument cameras;
    CameraRecord camera{}; camera.name = "Original Camera"; camera.targetDistance = 1600; camera.fieldOfView = 70;
    camera.farClippingPlane = 5000; camera.nearClippingPlane = 100; cameras.entries.push_back(std::move(camera));
    UnitPlacements units;
    UnitPlacement unit; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; unit.creationId = 17;
    unit.unknownOwnerBytes = {0xa5, 0x5a}; unit.targetAcquisition = -1; units.units.push_back(unit);
    unit.creationId = 23; unit.rawcode = "hbar"; units.units.push_back(unit);
    const auto rb = regions.Serialize(), cb = cameras.Serialize(), ub = units.Serialize();
    Require(rb.Succeeded() && cb.Succeeded() && ub.Succeeded(), "serialize original synthetic typed members");
    std::vector<std::byte> strings;
    for (const auto c : std::string_view("STRING 42\n{\nResolved Region\n}\n")) strings.push_back(static_cast<std::byte>(c));
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3mapUnits.doo", *ub.value},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}, {"war3map.w3r", *rb.value}, {"war3map.w3c", *cb.value}, {"war3map.wts", strings}};
}
MapFieldEdit Edit(const MapDocument& document, MapDataKind kind, std::size_t row, std::string_view fieldId, std::string_view value) {
    const auto* field = FindMapField(kind, fieldId); Require(field != nullptr, "existing shared field descriptor");
    const auto key = MapFieldRowKey(document, kind, row);
    const auto before = ReadMapField(document, kind, row, *field);
    const auto after = ParseMapFieldInput(*field, value);
    Require(key.Succeeded() && before.Succeeded() && after.Succeeded(), "create actual row/value-bound edit");
    return {kind, row, *key.value, std::string(fieldId), *before.value, *after.value};
}
PreparedMapDataFields Plan(const MapDocument& original, const MapDocument& current, std::vector<MapFieldEdit> edits) {
    const auto plan = PrepareMapDataFields(original, current, Sha(current, edits.front().kind), edits);
    Require(plan.Succeeded(), "prepare real typed field plan"); return *plan.value;
}
MapDocument View(const MapDocument& original, const MapDataEditSession& session) {
    auto projected = ProjectMapData(original, session); Require(projected.Succeeded(), "project original plus typed overlay"); return std::move(*projected.value);
}

std::vector<MapArchiveMember> PositionMembers(bool startLocation = false) {
    auto members = Members();
    const auto parsed = ParseUnitPlacements(members[1].data);
    Require(parsed.Succeeded(), "位置夹具从合法经典单位字节建立");
    auto units = *parsed.value;
    auto& unit = units.units.front();
    unit.position = {1.25f, 2.5f, 3.75f}; unit.rotation = 0.75f;
    unit.itemSets = {{{{"I000", 37}, {"I001", 63}}}, {{{"I002", 100}}}};
    unit.inventory = {{0, "I003"}, {5, "I004"}};
    unit.abilities = {{"A000", true, 2}, {"A001", false, 4}};
    unit.random = RandomCustomTable{{{"hpea", 40}, {"hrif", 60}}};
    units.units[1].position = {-40, 80, 20}; units.units[1].random = RandomGlobalTable{5, 2};
    if (startLocation) {
        UnitPlacement start; start.rawcode = "sloc"; start.creationId = 101;
        start.position = {100, 200, 300}; start.scale = {1, 1, 1}; units.units.push_back(start);
    }
    const auto bytes = units.Serialize(); Require(bytes.Succeeded(), "嵌套掉落、背包、技能和随机表须正常写出");
    members[1] = {"WAR3MAPUNITS.DOO", *bytes.value};
    return members;
}

void RequireOtherMembers(const MapDocument& original, const MapDocument& current) {
    Require(original.SourcePath() == current.SourcePath(), "位置字段须保留源路径");
    for (const auto& before : original.Members()) {
        const auto& after = current.Member(before.kind);
        Require(before.state == after.state && before.memberName == after.memberName
            && before.sources.size() == after.sources.size(), "位置字段须保留所有成员状态和来源个数");
        for (std::size_t index = 0; index < before.sources.size(); ++index) {
            Require(before.sources[index].name == after.sources[index].name, "位置字段不得规范化原来源别名");
            if (before.kind != MapDataKind::Units)
                Require(before.sources[index].data == after.sources[index].data, "位置字段不得改变 W3I/WTS 或其他 native 成员");
        }
    }
}

void PatchFloat(std::vector<std::byte>& bytes, std::size_t offset, float value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "独立 f32 字节窗口在完整成员内");
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::byte>((bits >> (index * 8)) & 0xff);
}

void UnitPositionChangesOnlyNativeWindowAndOriginalUndo() {
    const auto original = MapDocument::FromMembers(PositionMembers(), "position-native-fixture");
    Require(original.Member(MapDataKind::Units).state == MapDataState::Ready && original.Units()->units.size() == 2,
        "实际经典单位来源须完整可解析");
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Units, 0, "x", "125.5"),
        Edit(original, MapDataKind::Units, 0, "y", "-23.25"), Edit(original, MapDataKind::Units, 0, "z", "64.125")});
    auto expected = original.Member(MapDataKind::Units).sources.front().data;
    // W3do 头 16 字节，首行 rawcode/variation 后紧接 3 个 f32。只补这 12 字节，
    // 不调用 typed writer 构造期望值，整成员比较覆盖所有嵌套表和后续变长记录。
    PatchFloat(expected, 24, 125.5f); PatchFloat(expected, 28, -23.25f); PatchFloat(expected, 32, 64.125f);
    Require(plan.After().Member(MapDataKind::Units).sources.front().data == expected,
        "完整单位 native 只能在首行 XYZ 的 12 字节窗口改变");
    RequireOtherMembers(original, plan.After());
    MapDataEditSession session(original);
    std::uint64_t deniedIdentity = 0;
    const auto denied = session.ApplyPreparedFields(plan, [&](const auto&, const auto& after, const auto& receipt) {
        deniedIdentity = receipt.CommandIdentity();
        Require(after.Member(MapDataKind::Units).sources.front().data == expected && session.Revision() == 0
            && !session.CanUndo() && session.Replacements().empty(), "XYZ 回调在完整候选发布前验证原会话");
        return Status::Failure("Test", "POSITION_DENIED", "拒绝位置候选");
    });
    Require(HasCode(denied, "POSITION_DENIED") && session.Revision() == 0 && !session.IsDirty(),
        "位置候选拒绝须保留原字节、修订和历史");
    Require(session.ApplyPreparedFields(plan).Succeeded() && session.Revision() == 1, "三个位置分量只发布一个原命令");
    const auto receipt = session.LastEdit();
    Require(receipt.CommandIdentity() == deniedIdentity && receipt.SessionIdentity() == session.SessionIdentity()
        && receipt.HasPlacementLineage() && receipt.PlacementLineage().size() == 1, "失败不消耗命令身份，成功回执属于原会话");
    const auto& lineage = receipt.PlacementLineage().front();
    Require(lineage.beforeCount == 2 && lineage.afterCount == 2 && lineage.beforeToAfter[0] == 0u
        && lineage.beforeToAfter[1] == 1u, "XYZ 保留两个原实例的 i 到 i 谱系");
    auto current = View(original, session);
    Require(current.Member(MapDataKind::Units).sources.front().data == expected, "原命令实际发布的完整 native 与独立窗口 oracle 一致");
    RequireOtherMembers(original, current);
    Require(session.Undo() && !session.IsDirty()
        && View(original, session).Member(MapDataKind::Units).sources.front().data == original.Member(MapDataKind::Units).sources.front().data,
        "原 Undo 恢复完整单位源字节");
    Require(session.Redo() && View(original, session).Member(MapDataKind::Units).sources.front().data == expected,
        "原 Redo 恢复同一完整位置候选");
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty(), "原位置回执可回放撤销");
    MapDataEditSession foreign(original); Require(!foreign.Replay(receipt, true).Succeeded(), "位置回执不得进入其他会话");
}

void UnitPositionInputAndAtomicRowGuards() {
    const auto original = MapDocument::FromMembers(PositionMembers(true));
    for (const auto id : {"x", "y", "z"}) {
        const auto* field = FindMapField(MapDataKind::Units, id);
        Require(field && field->writable && MapFieldRowAllowsWrite(original, 0, *field)
            && !MapFieldRowAllowsWrite(original, 2, *field) && !MapFieldRowAllowsWrite(original, 3, *field),
            "只有实际普通行可写；sloc 的全部 XYZ 与越界行只读");
        for (const auto text : {"1000000", "-1000000", "-0", "f32:80000000"}) {
            const auto value = ParseMapFieldInput(*field, text);
            Require(value.Succeeded() && ValidateMapFieldValue(MapDataKind::Units, id, *value.value).Succeeded(),
                "坐标边界和精确负零可按 native 位型保存");
        }
        for (const auto text : {"1000000.001", "-1000000.001", "1000001", "nan", "inf"})
            Require(HasCode(ParseMapFieldInput(*field, text), "MAP_FIELD_INPUT_INVALID"),
                "超界 double 须在转 float 前拒绝，不能舍入回允许的边界");
        for (const auto text : {"f32:7f800000", "f32:ff800000", "f32:7fc00001", "f32:49742401"}) {
            const auto raw = ParseMapFieldInput(*field, text);
            Require(raw.Succeeded() && HasCode(ValidateMapFieldValue(MapDataKind::Units, id, *raw.value), "MAP_FIELD_VALUE_INVALID"),
                "精确 raw 的无穷、NaN 和超界 f32 仍须拒绝");
        }
        const auto start = Edit(original, MapDataKind::Units, 2, id, "101");
        Require(HasCode(ApplyMapFieldEdits(original, std::span(&start, 1)), "MAP_FIELD_POSITION_READ_ONLY"),
            "字段入口拒绝 sloc 各坐标，不能绕过 W3I 联动");
    }
    const auto* region = FindMapField(MapDataKind::Regions, "left");
    Require(ParseMapFieldInput(*region, "1000000.001").Succeeded(), "既有区域自然输入转换语义保持不变");
    auto malformed = Edit(original, MapDataKind::Units, 0, "z", "f32:7fc00001");
    const auto bad = PrepareMapDataFields(original, original, Sha(original, MapDataKind::Units),
        std::vector<MapFieldEdit>{Edit(original, MapDataKind::Units, 0, "x", "2"), malformed});
    Require(HasCode(bad, "MAP_FIELD_VALUE_INVALID"), "合法 X 前缀加非法 Z 尾项须全批拒绝");
    const auto startLate = PrepareMapDataFields(original, original, Sha(original, MapDataKind::Units),
        std::vector<MapFieldEdit>{Edit(original, MapDataKind::Units, 0, "x", "2"), Edit(original, MapDataKind::Units, 2, "z", "1")});
    Require(HasCode(startLate, "MAP_FIELD_POSITION_READ_ONLY"), "普通位置前缀加 sloc 尾项不得返回部分候选");
    auto stale = Edit(original, MapDataKind::Units, 0, "x", "2"); stale.expectedRowKey += "wrong";
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Units), std::span(&stale, 1)), "MAP_FIELD_ROW_STALE"),
        "新位置字段仍严格绑定原行身份");
    stale = Edit(original, MapDataKind::Units, 0, "x", "2"); stale.expected.floatBits ^= 1;
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Units), std::span(&stale, 1)), "MAP_FIELD_VALUE_STALE"),
        "新位置字段仍逐位核对 expected_raw");
    MapDataEditSession untouched(original);
    Require(untouched.Revision() == 0 && !untouched.IsDirty() && !untouched.CanUndo()
        && original.Member(MapDataKind::Settings).sources.front().data == PositionMembers(true)[0].data,
        "所有非法位置批次保留原历史和 W3I 字节");
}

// 英雄等级可写：不小于 1 的整数，按行身份写入并只改该字段。
void HeroLevelIsWritableWithLowerBound() {
    const auto original = MapDocument::FromMembers(PositionMembers());
    const auto* field = FindMapField(MapDataKind::Units, "hero_level");
    Require(field && field->writable && field->type == MapFieldType::Int32, "英雄等级为可写整数字段");
    for (const auto text : {"0", "-1"}) {
        const auto value = ParseMapFieldInput(*field, text);
        Require(!value.Succeeded() || HasCode(ValidateMapFieldValue(MapDataKind::Units, "hero_level", *value.value), "MAP_FIELD_VALUE_INVALID"),
            "英雄等级小于 1 须拒绝");
    }
    const auto edit = Edit(original, MapDataKind::Units, 0, "hero_level", "7");
    const auto planned = ApplyMapFieldEdits(original, std::span(&edit, 1));
    Require(planned.Succeeded() && planned.value->units && planned.value->units->units[0].heroLevel == 7,
        "英雄等级写入目标行");
    auto expected = *original.Units();
    expected.units[0].heroLevel = 7;
    Require(*planned.value->units->Serialize().value == *expected.Serialize().value, "只改英雄等级，其余字节不变");
}

void UnitPositionReadonlyLayoutsAndReentrantPublication() {
    auto members = PositionMembers();
    const auto parsed = ParseUnitPlacements(members[1].data); auto duplicated = *parsed.value;
    duplicated.units[1].creationId = duplicated.units[0].creationId;
    const auto duplicateBytes = duplicated.Serialize(); Require(duplicateBytes.Succeeded(), "真实重复 ID wire 夹具");
    members[1].data = *duplicateBytes.value;
    const auto partial = MapDocument::FromMembers(members);
    const auto* x = FindMapField(MapDataKind::Units, "x");
    Require(partial.Units() && partial.Member(MapDataKind::Units).state == MapDataState::Partial
        && !MapFieldRowAllowsWrite(partial, 0, *x), "重复实例的 Partial 来源不可编辑 XYZ");
    const auto partialEdit = Edit(partial, MapDataKind::Units, 0, "x", "3");
    Require(HasCode(ApplyMapFieldEdits(partial, std::span(&partialEdit, 1)), "MAP_FIELD_LAYOUT_READ_ONLY")
        && HasCode(PrepareMapDataFields(partial, partial, Sha(partial, MapDataKind::Units), std::span(&partialEdit, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
        "公开字段入口和 prepared 入口都须拒绝 Partial XYZ");
    auto settings = MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(settings.Succeeded(), "Skin 来源须从真实 W3I 版本布局建立");
    settings.value->formatVersion = 31; settings.value->gameVersion = {1, 32, 10, 20456};
    UnitPlacements skinUnits; UnitPlacement skinUnit; skinUnit.rawcode = "hfoo"; skinUnit.creationId = 77;
    skinUnit.scale = {1, 1, 1}; skinUnit.random = RandomAnyUnit{4, 0}; skinUnits.units.push_back(skinUnit);
    const auto classic = skinUnits.Serialize(), settingsBytes = settings.value->Serialize();
    Require(classic.Succeeded() && settingsBytes.Succeeded(), "Skin 夹具的经典基础和版本证明须正常往返");
    auto skinBytes = *classic.value;
    const std::array<std::byte, 4> skinId{std::byte{'H'}, std::byte{'f'}, std::byte{'o'}, std::byte{'o'}};
    skinBytes.insert(skinBytes.begin() + 52, skinId.begin(), skinId.end());
    const auto skin = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3map.w3i", *settingsBytes.value}, {"war3mapUnits.doo", skinBytes}});
    Require(skin.Units() && skin.Units()->recordLayout == PlacementRecordLayout::SkinId
        && skin.Units()->units.front().skinId == "Hfoo" && !MapFieldRowAllowsWrite(skin, 0, *x), "实际 Skin 行须保留可读和位置只读状态");
    const auto skinEdit = Edit(skin, MapDataKind::Units, 0, "x", "3");
    Require(HasCode(ApplyMapFieldEdits(skin, std::span(&skinEdit, 1)), "MAP_FIELD_LAYOUT_READ_ONLY")
        && HasCode(PrepareMapDataFields(skin, skin, Sha(skin, MapDataKind::Units), std::span(&skinEdit, 1)), "MAP_FIELD_LAYOUT_READ_ONLY")
        && skin.Member(MapDataKind::Units).sources.front().data == skinBytes, "Skin 不得以经典 XYZ writer 重建");
    const auto original = MapDocument::FromMembers(PositionMembers());
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Units, 0, "x", "7")});
    MapDataEditSession nested(original);
    auto author = *original.Units(); author.units[1].heroLevel = 5;
    const auto refused = nested.ApplyPreparedFields(plan, [&](const auto&, const auto&, const auto&) {
        Require(nested.EditUnits(author).Succeeded(), "回调中原会话真实作者命令可成功提交"); return Status::Ok();
    });
    auto after = View(original, nested);
    Require(HasCode(refused, "MAP_EDIT_PREPUBLISH_STALE") && nested.Revision() == 1
        && after.Units()->units[0].position == original.Units()->units[0].position && after.Units()->units[1].heroLevel == 5,
        "过期外层 XYZ 不得覆盖重入成功的原命令");
    Require(nested.Undo() && !nested.IsDirty() && nested.Redo() && View(original, nested).Units()->units[1].heroLevel == 5,
        "拒绝外层候选须保留内层原 Undo/Redo");
    MapDataEditSession sameBytes(original);
    const auto sameRefused = sameBytes.ApplyPreparedFields(plan, [&](const auto&, const auto&, const auto&) {
        Require(sameBytes.EditUnits(author).Succeeded() && sameBytes.Undo(), "重入真实编辑再 Undo 恢复相同源字节"); return Status::Ok();
    });
    Require(HasCode(sameRefused, "MAP_EDIT_PREPUBLISH_STALE") && sameBytes.Revision() == 2 && !sameBytes.IsDirty()
        && sameBytes.CanRedo() && sameBytes.Redo() && View(original, sameBytes).Units()->units[1].heroLevel == 5,
        "字节恢复相同但修订已变也须拒绝外层位置候选，保留内层 redo");
}

void UnitPositionFreezePreservesAliasesLniAndPriorAuthorEdits() {
    auto members = PositionMembers();
    const auto lniBytes = LniMapSettingsFixtures::Bytes(LniMapSettingsFixtures::Complete());
    members[0] = {"table/w3i.ini", lniBytes};
    const auto original = MapDocument::FromMembers(members, "position-lni-fixture");
    Require(original.LniSettings() && original.Member(MapDataKind::Units).state == MapDataState::Ready, "真实 LNI 来源可读取经典单位");
    MapDataEditSession session(original);
    const auto first = Plan(original, original, {Edit(original, MapDataKind::Units, 0, "x", "10"),
        Edit(original, MapDataKind::Units, 0, "y", "20"), Edit(original, MapDataKind::Units, 0, "z", "30")});
    Require(session.ApplyPreparedFields(first).Succeeded(), "原会话发布三个绝对位置字段");
    auto current = View(original, session);
    Require(session.ApplyPreparedFields(Plan(original, current, {Edit(current, MapDataKind::Units, 0, "x", "11")})).Succeeded(), "重复 X 更新保留最终输入值");
    current = View(original, session);
    const auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 3 && frozen.value->Members().size() == 1
        && frozen.value->Members().front().name == "war3mapUnits.doo"
        && frozen.value->Members().front().data == current.Member(MapDataKind::Units).sources.front().data,
        "累计 XYZ 合并为三个原绑定字段，规范输出须等于完整当前 native");
    std::vector<MapFieldEdit> replay;
    for (const auto& operation : frozen.value->Operations()) {
        Require(operation.expectedMemberSha256 == Sha(original, MapDataKind::Units), "XYZ 保存操作绑定原来源成员 SHA");
        replay.push_back(operation.edit);
    }
    const auto tables = ApplyMapFieldEdits(original, replay);
    Require(tables.Succeeded() && *SerializeMapFieldTable(*tables.value, MapDataKind::Units).value == frozen.value->Members().front().data,
        "既有 fieldEdits 原重放精确生成累计 native，无第二套位置保存格式");
    RequireOtherMembers(original, current);
    Require(current.LniSettings() && current.LniSettings()->originalBytes == lniBytes
        && current.Member(MapDataKind::Settings).memberName == "table/w3i.ini", "XYZ 不改变 LNI 设置文本和来源语义");
    std::vector<MapFieldEdit> restore;
    for (const auto id : {"x", "y", "z"}) {
        const auto* field = FindMapField(MapDataKind::Units, id);
        const auto before = ReadMapField(original, MapDataKind::Units, 0, *field);
        restore.push_back(Edit(current, MapDataKind::Units, 0, id, FormatMapFieldRaw(*before.value)));
    }
    Require(session.ApplyPreparedFields(Plan(original, current, restore)).Succeeded(), "三个原 f32 坐标可精确恢复");
    current = View(original, session);
    const auto empty = FreezeOriginalMapDataFields(original, current);
    Require(empty.Succeeded() && empty.value->Operations().empty() && empty.value->Members().empty()
        && current.Member(MapDataKind::Units).sources.front().data == original.Member(MapDataKind::Units).sources.front().data,
        "恢复原位置后累计 Freeze 净零且字节完全相同");
    RequireOtherMembers(original, current);
    auto regions = *original.Regions(); regions.entries.front().name = "作者先前改名";
    MapDataEditSession authorSession(original);
    Require(authorSession.EditRegions(regions).Succeeded(), "GUI 作者先前修改原历史");
    const auto authorBefore = View(original, authorSession);
    Require(authorSession.ApplyPreparedFields(Plan(original, authorBefore, {Edit(authorBefore, MapDataKind::Units, 0, "x", "9")})).Succeeded(),
        "GUI XYZ 命令不得套用 standalone 全会话字段冻结限制");
    const auto authorAfter = View(original, authorSession);
    Require(authorAfter.Regions()->entries.front().name == "作者先前改名"
        && authorAfter.Units()->units[0].position[0] == 9
        && HasCode(FreezeOriginalMapDataFields(original, authorAfter), "MAP_FIELD_PROJECTION_SCOPE"),
        "原 GUI 作者编辑保留，而 standalone 字段冻结仍拒绝未授权改名");
    Require(authorSession.Undo() && View(original, authorSession).Regions()->entries.front().name == "作者先前改名"
        && View(original, authorSession).Units()->units[0].position == original.Units()->units[0].position,
        "原 Undo 只撤销 XYZ，不擦除作者既有编辑");
}

void AtomicFieldsUseOriginalReceiptsAndIdentityLineage() {
    const auto original = MapDocument::FromMembers(Members()); MapDataEditSession session(original);
    auto plan = Plan(original, original, {Edit(original, MapDataKind::Units, 0, "owner", "2"), Edit(original, MapDataKind::Units, 0, "hit_points", "65")});
    std::uint64_t preparedIdentity = 0;
    const auto denied = session.ApplyPreparedFields(plan, [&](const auto& before, const auto& after, const auto& receipt) {
        Require(before.Units()->units[0].owner == 0 && after.Units()->units[0].owner == 2, "validator sees complete actual before and after");
        Require(session.Revision() == 0 && !session.CanUndo() && session.Replacements().empty(), "validator runs before original publication");
        preparedIdentity = receipt.CommandIdentity(); return Status::Failure("Test", "DENIED", "拒绝候选");
    });
    Require(HasCode(denied, "DENIED") && session.Revision() == 0 && !session.IsDirty() && session.LastAppliedEdit().CommandIdentity() == 0,
        "denied candidate preserves bytes, history, revision and last receipt");
    Require(session.ApplyPreparedFields(plan).Succeeded(), "publish the same prepared original command");
    const auto receipt = session.LastEdit();
    Require(receipt.SessionIdentity() == session.SessionIdentity() && receipt.CommandIdentity() == preparedIdentity
        && receipt.HasPlacementLineage() && receipt.PlacementLineage().size() == 1, "failure consumes no identity and real receipt owns lineage");
    const auto& lineage = receipt.PlacementLineage().front();
    Require(lineage.beforeCount == 2 && lineage.afterCount == 2
        && lineage.beforeToAfter[0] == std::uint32_t{0} && lineage.beforeToAfter[1] == std::uint32_t{1},
        "field updates preserve explicit unit instance identities");
    const auto changed = View(original, session);
    Require(changed.Units()->units[0].unknownOwnerBytes == original.Units()->units[0].unknownOwnerBytes
        && changed.Units()->units[1].owner == 0 && original.Units()->units[0].owner == 0, "preserve opaque owner bytes, other row and source");
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty() && session.Replay(receipt, true).Succeeded(), "original receipt replays exact bytes");
    MapDataEditSession other(original); Require(!other.Replay(receipt, false).Succeeded(), "foreign original receipt still rejected");
    Require(HasCode(session.ApplyPreparedFields(plan), "MAP_FIELD_PLAN_STALE"), "prepared before image cannot replay as a fresh edit");
}

void RegionBatchChecksFinalBoundsAndPreservesRedoOnDenial() {
    const auto original = MapDocument::FromMembers(Members()); MapDataEditSession session(original);
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Regions, 0, "left", "32"), Edit(original, MapDataKind::Regions, 0, "right", "45")});
    Require(session.ApplyPreparedFields(plan).Succeeded() && session.Revision() == 1, "two jointly valid bounds commit once despite invalid intermediate left");
    Require(session.Undo() && session.CanRedo(), "seed original redo history");
    const auto revision = session.Revision();
    auto denied = session.ApplyPreparedFields(plan, [](const auto&, const auto&, const auto&) { return Status::Failure("Test", "DENIED", "拒绝候选"); });
    Require(HasCode(denied, "DENIED") && session.Revision() == revision && session.CanRedo() && !session.IsDirty(), "guard denial does not erase redo history");
    bool threw = false;
    try { (void)session.ApplyPreparedFields(plan, [](const auto&, const auto&, const auto&) -> Status { throw std::runtime_error("synthetic validator failure"); }); }
    catch (const std::runtime_error&) { threw = true; }
    Require(threw && session.Revision() == revision && session.CanRedo() && !session.IsDirty(), "throwing guard retains the same original state");
    const auto bad = PrepareMapDataFields(original, original, Sha(original, MapDataKind::Regions),
        std::vector<MapFieldEdit>{Edit(original, MapDataKind::Regions, 0, "left", "31")});
    Require(HasCode(bad, "MAP_FIELD_ROW_INVALID") && session.Redo(), "invalid final bounds fail without losing the prior command");
}

void FreezeCoalescesOriginalBoundFieldsAndProvesNativeBytes() {
    const auto original = MapDocument::FromMembers(Members()); MapDataEditSession session(original);
    const auto first = Plan(original, original, {Edit(original, MapDataKind::Regions, 0, "left", "-9")});
    Require(session.ApplyPreparedFields(first).Succeeded(), "first current field value");
    auto current = View(original, session);
    const auto second = Plan(original, current, {Edit(current, MapDataKind::Regions, 0, "left", "-8")});
    Require(session.ApplyPreparedFields(second).Succeeded(), "second current field value"); current = View(original, session);
    auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 1 && frozen.value->Members().size() == 1,
        "repeated updates coalesce into one original-bound field");
    const auto& operation = frozen.value->Operations().front();
    Require(operation.expectedMemberSha256 == Sha(original, MapDataKind::Regions)
        && FormatMapFieldRaw(operation.edit.expected) == FormatMapFieldRaw(Edit(original, MapDataKind::Regions, 0, "left", "-8").expected), "save operation uses original SHA and original before bits");
    const auto projected = ApplyMapFieldEdits(original, std::span(&operation.edit, 1));
    Require(projected.Succeeded() && *SerializeMapFieldTable(*projected.value, MapDataKind::Regions).value == current.Member(MapDataKind::Regions).sources.front().data,
        "existing field save input exactly regenerates the actual cumulative native bytes");
    const auto restore = Plan(original, current, {Edit(current, MapDataKind::Regions, 0, "left", "-10")});
    Require(session.ApplyPreparedFields(restore).Succeeded(), "restore original value");
    frozen = FreezeOriginalMapDataFields(original, View(original, session));
    Require(frozen.Succeeded() && frozen.value->Operations().empty() && frozen.value->Members().empty(), "restoration produces an empty successful freeze");
    auto regions = *original.Regions(); regions.entries[0].ambientSound = "Changed sound";
    MapDataEditSession outsider(original); Require(outsider.EditRegions(regions).Succeeded(), "seed actual readonly-field native change through wider original API");
    Require(HasCode(FreezeOriginalMapDataFields(original, View(original, outsider)), "MAP_FIELD_PROJECTION_SCOPE"), "whole-byte proof rejects unapproved readonly member changes");
    regions = *original.Regions(); regions.entries.erase(regions.entries.begin()); MapDataEditSession removed(original);
    Require(removed.EditRegions(regions).Succeeded()
        && HasCode(FreezeOriginalMapDataFields(original, View(original, removed)), "MAP_FIELD_PROJECTION_SCOPE"), "row deletion cannot be hidden as a fields-only projection");
    MapDataEditSession seeded(original); Require(seeded.SetSerialized(MapDataKind::Regions, second.AfterMemberBytes()).Succeeded(), "standalone seeds approved cumulative bytes");
    auto seededView = View(original, seeded);
    Require(seeded.ApplyPreparedFields(Plan(original, seededView, {Edit(seededView, MapDataKind::Regions, 0, "right", "41")})).Succeeded(), "seeded isolated session applies the current before image");
}

void CumulativeLimitRejectsBeforePublication() {
    const auto original = MapDocument::FromMembers(Members(33)); MapDataEditSession session(original);
    std::vector<MapFieldEdit> edits;
    for (std::size_t row = 0; row < 32; ++row) edits.push_back(Edit(original, MapDataKind::Regions, row, "left", "-11"));
    Require(session.ApplyPreparedFields(Plan(original, original, edits)).Succeeded(), "32 cumulative fields fit original save branch");
    const auto current = View(original, session); const auto revision = session.Revision(); const auto receipt = session.LastEdit();
    const auto over = Plan(original, current, {Edit(current, MapDataKind::Regions, 32, "left", "-11")});
    const auto status = session.ApplyPreparedFields(over, [&](const auto&, const auto& after, const auto&) {
        const auto frozen = FreezeOriginalMapDataFields(original, after); return Status{frozen.diagnostics};
    });
    Require(HasCode(status, "MAP_FIELD_CUMULATIVE_COUNT") && session.Revision() == revision
        && session.LastEdit().CommandIdentity() == receipt.CommandIdentity() && Sha(View(original, session), MapDataKind::Regions) == Sha(current, MapDataKind::Regions),
        "33rd cumulative field is rejected before counters, history or bytes change");
}

void MapTextFieldsPreserveWireAndOriginalUndo() {
    auto members = Members();
    const std::string wire = "// preserved lead\r\nSTRING 42\r\n{\r\nResolved Region\r\n}\r\n// preserved tail\r\nSTRING 7\r\n{\r\nUntouched\r\n}\r\n";
    members.back().data.assign(reinterpret_cast<const std::byte*>(wire.data()), reinterpret_cast<const std::byte*>(wire.data() + wire.size()));
    const auto original = MapDocument::FromMembers(members);
    Require(original.ResolveString(original.Settings()->description).value == std::optional<std::string>("Resolved Region"),
        "真实W3I wire引用从WTS解析，不替代精确原值");
    MapDataEditSession session(original);
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Strings, 0, "value", "共享文字\nsecond line")});
    auto expectedWire = wire; expectedWire.replace(expectedWire.find("Resolved Region"), std::string("Resolved Region").size(), "共享文字\nsecond line");
    const std::vector<std::byte> expected(reinterpret_cast<const std::byte*>(expectedWire.data()), reinterpret_cast<const std::byte*>(expectedWire.data() + expectedWire.size()));
    Require(plan.After().Member(MapDataKind::Strings).sources.front().data == expected
        && plan.After().Member(MapDataKind::Settings).sources.front().data == members.front().data,
        "WTS仅正文跨度改变；BOM/注释/换行/ID与W3I wire引用不改变");
    const auto refused = session.ApplyPreparedFields(plan, [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "TEXT_REJECTED", "文字回执拒绝");
    });
    Require(HasCode(refused, "TEXT_REJECTED") && session.Revision() == 0 && !session.CanUndo() && !session.IsDirty(),
        "发布前拒绝保留原文字字节、身份和历史");
    Require(session.ApplyPreparedFields(plan).Succeeded(), "WTS密封候选向原会话提交");
    const auto receipt = session.LastEdit();
    auto current = View(original, session);
    Require(current.ResolveString(current.Settings()->description).value == std::optional<std::string>("共享文字\nsecond line"),
        "共享ID的既有真实引用解析为新正文");
    auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 1 && frozen.value->Members().size() == 1,
        "WTS最终差异由同一原来源字段冻结表达");
    std::vector<MapFieldEdit> operations;
    for (const auto& operation : frozen.value->Operations()) operations.push_back(operation.edit);
    auto tables = ApplyMapFieldEdits(original, operations);
    MapDataEditSession saving(original);
    Require(tables.Succeeded() && tables.value->strings && saving.EditStrings(*tables.value->strings).Succeeded()
        && saving.Replacements().size() == 1 && saving.Replacements().front().data == expected,
        "冻结操作通过原保存提交函数重放得到相同完整WTS，未注入任意成员");
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty()
        && View(original, session).Member(MapDataKind::Strings).sources.front().data == members.back().data,
        "原Undo回执恢复含注释与CRLF的全部WTS字节");
    Require(session.Replay(receipt, true).Succeeded() && View(original, session).Member(MapDataKind::Strings).sources.front().data == expected,
        "原Redo回执恢复相同WTS正文候选");
}

void MapLiteralTextFieldsPreserveWireAndOriginalUndo() {
    auto members = Members();
    auto settings = MapSettings::Parse(members.front().data);
    Require(settings.Succeeded(), "literal 回归由原经典 W3I 建立");
    settings.value->description = "Original literal description";
    const auto settingsWire = settings.value->Serialize(); Require(settingsWire.Succeeded(), "literal W3I 完整写出");
    members.front().data = *settingsWire.value;
    const std::string wire = "// preserved lead\r\nSTRING 42\r\n{\r\nResolved Region\r\n}\r\n// preserved tail\r\nSTRING 7\r\n{\r\nUntouched\r\n}\r\n";
    members.back().data.assign(reinterpret_cast<const std::byte*>(wire.data()), reinterpret_cast<const std::byte*>(wire.data() + wire.size()));
    const auto original = MapDocument::FromMembers(members);
    Require(!original.Settings()->description.starts_with("TRIGSTR_"), "原 literal 断言须在真实非引用来源上执行");
    MapDataEditSession session(original);
    const auto shared = Plan(original, original, {Edit(original, MapDataKind::Strings, 0, "value", "共享文字\nsecond line")});
    Require(session.ApplyPreparedFields(shared).Succeeded(), "literal 回归仍保留先前的独立 WTS 命令");
    auto expectedWire = wire; expectedWire.replace(expectedWire.find("Resolved Region"), std::string("Resolved Region").size(), "共享文字\nsecond line");
    const std::vector<std::byte> expected(reinterpret_cast<const std::byte*>(expectedWire.data()), reinterpret_cast<const std::byte*>(expectedWire.data() + expectedWire.size()));
    auto current = View(original, session);
    auto frozen = FreezeOriginalMapDataFields(original, current);
    const auto literal = Plan(original, current, {Edit(current, MapDataKind::Settings, 0, "description", "独立描述"),
        Edit(current, MapDataKind::Settings, 0, "author", ""), Edit(current, MapDataKind::Settings, 0, "recommended_players", "3–5")});
    Require(session.ApplyPreparedFields(literal).Succeeded(), "地图设置多个文字字段一次原命令");
    current = View(original, session);
    Require(current.Settings()->description == "独立描述" && current.Settings()->author.empty()
        && current.Member(MapDataKind::Strings).sources.front().data == expected && current.ResolveString("TRIGSTR_42").value == std::optional<std::string>("共享文字\nsecond line"),
        "设置literal仅改本字段wire，不改变共享WTS及其他引用");
    frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 4 && frozen.value->Members().size() == 2,
        "W3I与WTS累计冻结沿原保存身份与32项预算");
    Require(session.Undo() && View(original, session).Member(MapDataKind::Settings).sources.front().data == members.front().data
        && View(original, session).Member(MapDataKind::Strings).sources.front().data == expected,
        "地图设置Undo只恢复该原命令，保留前一WTS编辑");
}

std::vector<MapArchiveMember> ReferencedSettingsMembers() {
    auto members = Members();
    auto settings = MapSettings::Parse(members.front().data);
    Require(settings.Succeeded(), "引用回归从实际经典 W3I 字节建立");
    settings.value->author = "TRIGSTR_00042";
    settings.value->recommendedPlayers = "TRIGSTR_7";
    const auto bytes = settings.value->Serialize(); Require(bytes.Succeeded(), "三项引用的真实 W3I 写出");
    members.front().data = *bytes.value;
    const std::string wire = "\xef\xbb\xbf// keep lead\r\nSTRING 42\r\n{\r\nResolved Region\r\n}\r\n// keep between\r\nSTRING 7\r\n{\r\nUntouched\r\n}\r\n// keep tail\r\n";
    members.back().name = "WAR3MAP.WTS";
    members.back().data.assign(reinterpret_cast<const std::byte*>(wire.data()), reinterpret_cast<const std::byte*>(wire.data() + wire.size()));
    return members;
}

void SettingsReferencesUseUniqueSharedWtsAndOriginalHistory() {
    const auto members = ReferencedSettingsMembers();
    const auto original = MapDocument::FromMembers(members, "settings-reference-fixture");
    const std::vector<MapFieldEdit> changes{Edit(original, MapDataKind::Settings, 0, "author", "共享作者与描述\nsecond line"),
        Edit(original, MapDataKind::Settings, 0, "description", "共享作者与描述\nsecond line"),
        Edit(original, MapDataKind::Settings, 0, "recommended_players", "3–5")};
    const auto prepared = PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings), changes);
    Require(prepared.Succeeded() && prepared.value && HasCode(prepared, "MAP_FIELD_SHARED_STRING"),
        "三项设置引用共用原字段计划，明确提示同编号的其它引用也会改变");
    const auto& plan = *prepared.value;
    auto wire = std::string(reinterpret_cast<const char*>(members.back().data.data()), members.back().data.size());
    wire.replace(wire.find("Resolved Region"), std::string("Resolved Region").size(), "共享作者与描述\nsecond line");
    wire.replace(wire.find("Untouched"), std::string("Untouched").size(), "3–5");
    const std::vector<std::byte> expected(reinterpret_cast<const std::byte*>(wire.data()), reinterpret_cast<const std::byte*>(wire.data() + wire.size()));
    Require(plan.Kind() == MapDataKind::Settings && plan.BeforeMemberSha256() == plan.AfterMemberSha256()
        && plan.After().Member(MapDataKind::Settings).sources.front().data == members.front().data
        && plan.After().Member(MapDataKind::Strings).sources.front().data == expected
        && plan.After().Member(MapDataKind::Strings).sources.front().name == "WAR3MAP.WTS",
        "只替换两段 WTS 正文，完整 W3I 与引用前导零、WTS BOM/注释/CRLF/来源别名保持原字节");
    Require(plan.After().ResolveString(plan.After().Settings()->author).value == std::optional<std::string>("共享作者与描述\nsecond line")
        && plan.After().ResolveString(plan.After().Settings()->description).value == std::optional<std::string>("共享作者与描述\nsecond line")
        && plan.After().ResolveString(plan.After().Regions()->entries.front().name).value == std::optional<std::string>("共享作者与描述\nsecond line")
        && plan.After().ResolveString(plan.After().Settings()->recommendedPlayers).value == std::optional<std::string>("3–5"),
        "同编号的作者、描述和区域读取同一新正文；建议玩家绑定独立既有编号");
    MapDataEditSession session(original);
    std::uint64_t deniedIdentity = 0;
    const auto denied = session.ApplyPreparedFields(plan, [&](const auto&, const auto& after, const auto& receipt) {
        deniedIdentity = receipt.CommandIdentity();
        Require(after.Member(MapDataKind::Strings).sources.front().data == expected && session.Revision() == 0
            && !session.IsDirty() && !session.CanUndo(), "完整 WTS 候选在原会话发布前接受原验证回调");
        return Status::Failure("Test", "REFERENCE_DENIED", "拒绝共享文字候选");
    });
    Require(HasCode(denied, "REFERENCE_DENIED") && session.Revision() == 0 && !session.IsDirty() && !session.CanUndo(),
        "拒绝引用候选不消费命令身份、不改变任何字节和历史");
    Require(session.ApplyPreparedFields(plan).Succeeded() && session.Revision() == 1 && session.Replacements().size() == 1
        && session.LastEdit().CommandIdentity() == deniedIdentity, "三项引用与两个共享编号合成一个原命令，拒绝未消费身份");
    const auto receipt = session.LastEdit();
    auto current = View(original, session);
    const auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 2 && frozen.value->Members().size() == 1
        && frozen.value->Members().front().name == "war3map.wts", "三项引用冻结为两个原 WTS 正文操作，原 W3I 没有替换");
    std::vector<MapFieldEdit> replay;
    for (const auto& operation : frozen.value->Operations()) {
        Require(operation.expectedMemberSha256 == Sha(original, MapDataKind::Strings)
            && operation.edit.kind == MapDataKind::Strings && operation.edit.field == "value"
            && operation.edit.expectedRowKey == "string:" + std::to_string(original.Strings()->entries[operation.edit.row].id),
            "冻结绑定原 WTS 完整 SHA 与唯一 string:<id> 身份");
        replay.push_back(operation.edit);
    }
    const auto tables = ApplyMapFieldEdits(original, replay);
    Require(tables.Succeeded() && *SerializeMapFieldTable(*tables.value, MapDataKind::Strings).value == expected,
        "原保存字段重放逐字节等于两段正文独立 oracle");
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty()
        && View(original, session).Member(MapDataKind::Strings).sources.front().data == members.back().data,
        "原 Undo 一次恢复两个 WTS 正文和完整原格式");
    Require(session.Replay(receipt, true).Succeeded() && View(original, session).Member(MapDataKind::Strings).sources.front().data == expected,
        "原 Redo 保持同一命令和 WTS 候选");
    current = View(original, session);
    Require(session.ApplyPreparedFields(Plan(original, current, {
        Edit(current, MapDataKind::Settings, 0, "author", "Resolved Region"),
        Edit(current, MapDataKind::Settings, 0, "recommended_players", "Untouched")})).Succeeded(),
        "按引用入口写回两个原正文仍不改 W3I 引用");
    const auto empty = FreezeOriginalMapDataFields(original, View(original, session));
    Require(empty.Succeeded() && empty.value->Operations().empty() && empty.value->Members().empty() && !session.IsDirty(),
        "正文恢复原值后累计冻结为空，完整来源字节恢复");
    auto namedMembers = members; auto namedSettings = *original.Settings(); namedSettings.mapName = "TRIGSTR_0042";
    const auto namedWire = namedSettings.Serialize(); Require(namedWire.Succeeded(), "地图名引用夹具采用真实 W3I 编码");
    namedMembers.front().data = *namedWire.value;
    const auto named = MapDocument::FromMembers(namedMembers);
    const auto renamed = Plan(named, named, {Edit(named, MapDataKind::Settings, 0, "map_name", "独立地图名")});
    Require(renamed.After().Settings()->mapName == "独立地图名"
        && renamed.After().Member(MapDataKind::Strings).sources.front().data == members.back().data,
        "地图名保持既有 literal 规则，不扩大三项设置引用修复到地图名 WTS 自动修改");
}

void SettingsReferencesRejectConflictsUnsafeAndDriftingSources() {
    const auto members = ReferencedSettingsMembers();
    const auto original = MapDocument::FromMembers(members);
    const auto description = Edit(original, MapDataKind::Settings, 0, "description", "新描述");
    auto author = Edit(original, MapDataKind::Settings, 0, "author", "另一个正文");
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings),
        std::vector<MapFieldEdit>{description, author}), "MAP_FIELD_STRING_CONFLICT"), "同编号的两个设置字段不能按顺序相互覆盖");
    const auto direct = Edit(original, MapDataKind::Strings, 0, "value", "另一个正文");
    for (const auto edits : {std::vector<MapFieldEdit>{description, direct}, std::vector<MapFieldEdit>{direct, description}})
        Require(HasCode(ApplyMapFieldEdits(original, edits), "MAP_FIELD_STRING_CONFLICT"), "直接 WTS 与设置引用的冲突与输入顺序无关");
    auto stale = description; stale.expected.bytes = "TRIGSTR_7";
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings), std::span(&stale, 1)), "MAP_FIELD_VALUE_STALE"),
        "引用仍绑定 W3I 精确 raw 前值，不能只凭解析后正文授权");
    stale = description; stale.expectedRowKey += "wrong";
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings), std::span(&stale, 1)), "MAP_FIELD_ROW_STALE")
        && HasCode(PrepareMapDataFields(original, original, std::string(64, '0'), std::span(&description, 1)), "MAP_FIELD_MEMBER_STALE"),
        "设置行身份和原 W3I SHA 门禁保持有效");
    const auto unchanged = Edit(original, MapDataKind::Settings, 0, "description", "Resolved Region");
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings), std::span(&unchanged, 1)), "MAP_FIELD_NO_CHANGE"),
        "相同的解析正文不得生成伪修改命令");
    for (const auto reference : {"TRIGSTR_99", "TRIGSTR_", "TRIGSTR_x", "TRIGSTR_4294967296", "TRIGSTR_42x"}) {
        auto unsafeMembers = members; auto settings = *original.Settings(); settings.description = reference;
        const auto wire = settings.Serialize(); Require(wire.Succeeded(), "错误引用也须通过真实 W3I 编码构造");
        unsafeMembers.front().data = *wire.value;
        const auto unsafe = MapDocument::FromMembers(unsafeMembers);
        const auto edit = Edit(unsafe, MapDataKind::Settings, 0, "description", "new");
        Require(HasCode(PrepareMapDataFields(unsafe, unsafe, Sha(unsafe, MapDataKind::Settings), std::span(&edit, 1)),
            std::string_view(reference) == "TRIGSTR_99" ? "WTS_REFERENCE_MISSING" : "WTS_REFERENCE_INVALID"),
            "缺失与不完整、溢出引用都明确拒绝，不能覆盖成 literal");
    }
    auto missingMembers = members; missingMembers.pop_back();
    const auto missing = MapDocument::FromMembers(missingMembers);
    const auto missingEdit = Edit(missing, MapDataKind::Settings, 0, "description", "new");
    Require(HasCode(PrepareMapDataFields(missing, missing, Sha(missing, MapDataKind::Settings), std::span(&missingEdit, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
        "WTS 缺失不能由设置引用写入重建");
    auto duplicateMembers = members;
    const std::string duplicate = "STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\nSTRING 7\n{\nUntouched\n}\n";
    duplicateMembers.back().data.assign(reinterpret_cast<const std::byte*>(duplicate.data()), reinterpret_cast<const std::byte*>(duplicate.data() + duplicate.size()));
    const auto ambiguous = MapDocument::FromMembers(duplicateMembers);
    const auto duplicateEdit = Edit(ambiguous, MapDataKind::Settings, 0, "description", "new");
    Require(HasCode(PrepareMapDataFields(ambiguous, ambiguous, Sha(ambiguous, MapDataKind::Settings), std::span(&duplicateEdit, 1)), "WTS_REFERENCE_AMBIGUOUS"),
        "重复 WTS 编号拒绝，不能任选第一条或新建编号");
    duplicateMembers = members; duplicateMembers.push_back({"war3map.wts", members.back().data});
    const auto duplicateSource = MapDocument::FromMembers(duplicateMembers);
    const auto duplicateSourceEdit = Edit(duplicateSource, MapDataKind::Settings, 0, "description", "new");
    Require(HasCode(PrepareMapDataFields(duplicateSource, duplicateSource, Sha(duplicateSource, MapDataKind::Settings), std::span(&duplicateSourceEdit, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
        "重复大小写别名 WTS 来源也不能选取任一成员");
    auto driftMembers = members;
    const std::string reordered = "STRING 7\n{\nUntouched\n}\nSTRING 42\n{\nResolved Region\n}\n";
    driftMembers.back().data.assign(reinterpret_cast<const std::byte*>(reordered.data()), reinterpret_cast<const std::byte*>(reordered.data() + reordered.size()));
    const auto drift = MapDocument::FromMembers(driftMembers);
    const auto driftEdit = Edit(drift, MapDataKind::Settings, 0, "description", "new");
    Require(HasCode(PrepareMapDataFields(original, drift, Sha(drift, MapDataKind::Settings), std::span(&driftEdit, 1)), "MAP_FIELD_STRING_SOURCE_STALE"),
        "当前 WTS 行顺序漂移不能沿用原来源身份");
    MapDataEditSession session(original);
    const auto plan = Plan(original, original, {description});
    auto changedStrings = *original.Strings(); changedStrings.entries[1].value = "External text";
    Require(session.EditStrings(changedStrings).Succeeded(), "计划准备后用原命令真实改变 WTS 来源");
    const auto beforeRevision = session.Revision();
    Require(HasCode(session.ApplyPreparedFields(plan), "MAP_FIELD_PLAN_STALE") && session.Revision() == beforeRevision
        && View(original, session).Strings()->entries[1].value == "External text", "过期设置计划不得覆盖来源漂移或消费额外修订");
    Require(session.Undo() && !session.IsDirty(), "来源漂移拒绝保留成功作者命令的原 Undo");
}

void SettingsReferencePatchBindsTheOriginalWtsSource() {
    const auto original = MapDocument::FromMembers(ReferencedSettingsMembers());
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Settings, 0, "description", "引用保存正文")});
    WorkspaceSnapshot source;
    for (const auto kind : {MapDataKind::Settings, MapDataKind::Strings}) {
        const auto& member = original.Member(kind);
        source.files.push_back({member.memberName, member.sources.front().data.size(), 0, Sha(original, kind)});
    }
    const std::array kinds{MapDataKind::Settings};
    Require(MapDataFieldPatchCodec::Seal(original, plan.After(), source, kinds).Succeeded(),
        "Settings 请求可密封原 W3I 与其引用的 WTS 正文依赖");
    source.files.back().sha256 = std::string(64, '0');
    Require(HasCode(MapDataFieldPatchCodec::Seal(original, plan.After(), source, kinds), "MAP_FIELD_PATCH_SOURCE"),
        "仅请求 Settings 时仍独立核原 WTS SHA，来源漂移拒绝完整 patch");
    Require(HasCode(MapDataFieldPatchCodec::Seal(original, original, source, kinds), "MAP_FIELD_PATCH_SOURCE"),
        "引用正文恢复原值仍绑定 WTS 成员，可清除既有 typed overlay 而非残留旧正文");
    source.files.pop_back();
    Require(HasCode(MapDataFieldPatchCodec::Seal(original, plan.After(), source, kinds), "MAP_FIELD_PATCH_SOURCE"),
        "目录缺失引用 WTS 不得密封只含未变 W3I 的假成功 patch");
}

void MapTextFieldsRejectUnsafeSourcesAndLateValues() {
    const auto original = MapDocument::FromMembers(Members());
    const auto good = Edit(original, MapDataKind::Settings, 0, "author", "New author");
    auto bad = Edit(original, MapDataKind::Settings, 0, "description", "Valid");
    for (const auto& value : {std::string("TRIGSTR_999"), std::string(4097, 'x'), std::string("bad\0value", 9), std::string(1, static_cast<char>(0xff))}) {
        bad.value.bytes = value;
        Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Settings), std::vector<MapFieldEdit>{good, bad}), "MAP_FIELD_VALUE_INVALID"),
            "合法文字前项加非法引用、长度、NUL或编码尾项须全批拒绝");
    }
    MapDataEditSession untouched(original);
    Require(untouched.Revision() == 0 && !untouched.CanUndo() && untouched.Replacements().empty(), "非法纯候选从未发布会话");
    auto duplicate = Members();
    const std::string duplicateText = "STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\n";
    duplicate.back().data.assign(reinterpret_cast<const std::byte*>(duplicateText.data()), reinterpret_cast<const std::byte*>(duplicateText.data() + duplicateText.size()));
    const auto ambiguous = MapDocument::FromMembers(duplicate);
    Require(ambiguous.Member(MapDataKind::Strings).state == MapDataState::Partial
        && !ambiguous.ResolveString("TRIGSTR_42").Succeeded(), "真实重复ID保持Partial及解析不明");
    const auto duplicateEdit = Edit(ambiguous, MapDataKind::Strings, 0, "value", "new");
    Require(HasCode(PrepareMapDataFields(ambiguous, ambiguous, Sha(ambiguous, MapDataKind::Strings), std::span(&duplicateEdit, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
        "重复ID不能经prepared覆盖为唯一字符串表");
    auto missingMembers = Members(); missingMembers.pop_back();
    const auto missing = MapDocument::FromMembers(missingMembers);
    Require(!missing.ResolveString("TRIGSTR_42").Succeeded() && missing.Member(MapDataKind::Strings).state == MapDataState::Missing,
        "WTS缺失保持真实来源状态和未解析引用");
    for (bool omitted : {false, true}) {
        auto unsafeMembers = Members();
        if (omitted) {
            unsafeMembers.front().data.resize(MapDataFixtures::FullMapSettingsV25().offsets.at("upgrades"));
            unsafeMembers.front().data.push_back(std::byte{0xff});
        }
        else unsafeMembers.front().data.push_back(std::byte{0x7f});
        const auto unsafe = MapDocument::FromMembers(unsafeMembers);
        const auto edit = Edit(unsafe, MapDataKind::Settings, 0, "author", "new");
        Require(unsafe.Member(MapDataKind::Settings).state == MapDataState::Partial
            && HasCode(PrepareMapDataFields(unsafe, unsafe, Sha(unsafe, MapDataKind::Settings), std::span(&edit, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
            "未知尾部或省略custom的W3I只读，不能把未知理解为空表");
    }
}

struct SoundWireFixture {
    std::vector<std::byte> bytes;
    // 每行的 volume、fade_in、fade_out 原始四字节窗口；期望值不由产品 writer 生成。
    std::vector<std::array<std::size_t, 3>> offsets;
};

SoundWireFixture SoundWire(std::vector<std::string> names = {"Raw Sound", "gg_snd_Unchanged"}, std::uint32_t version = 1) {
    SoundWireFixture result;
    const auto u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) result.bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    };
    const auto text = [&](std::string_view value) {
        for (const char byte : value) result.bytes.push_back(static_cast<std::byte>(byte));
        result.bytes.push_back(std::byte{0});
    };
    u32(version); u32(static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names) {
        text(name); text("war3mapImported\\preserved.wav"); text("DefaultEAXON"); u32(7);
        const auto fadeIn = result.bytes.size(); u32(11);
        const auto fadeOut = result.bytes.size(); u32(22);
        const auto volume = result.bytes.size(); u32(0xffffffffu);
        result.offsets.push_back({volume, fadeIn, fadeOut});
        u32(kSoundDefaultRealBits); u32(0x80000000u); u32(8); u32(10);
        for (const float value : {100.0f, 2000.0f, 2500.0f, 90.0f, 180.0f}) u32(std::bit_cast<std::uint32_t>(value));
        u32(67);
        for (const float value : {0.5f, -0.25f, 0.75f}) u32(std::bit_cast<std::uint32_t>(value));
        if (version >= 2 && version <= 3) {
            text(name); text("preserved label"); text("war3mapImported\\preserved.wav");
            u32(0xfffffff1u); text("opaque2"); u32(0xffffffe9u); u32(0);
            text("opaque6"); text("opaque7"); text("Face"); text("Group"); text("Face.fdf");
            if (version == 3) u32(0xffffffe1u);
        }
    }
    return result;
}

std::vector<MapArchiveMember> SoundMembers(const SoundWireFixture& wire, std::string name = "WAR3MAP.W3S") {
    auto members = Members(); members.push_back({std::move(name), wire.bytes}); return members;
}

void PatchInteger(std::vector<std::byte>& bytes, std::size_t offset, std::int32_t value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "声音整数窗口须位于原成员内");
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::byte>((bits >> (index * 8)) & 0xff);
}

void SoundFieldsUseExactNativeWindowsAndOriginalHistory() {
    const auto wire = SoundWire();
    const auto original = MapDocument::FromMembers(SoundMembers(wire), "sound-alias-fixture");
    if (!original.Sounds() || original.Member(MapDataKind::Sounds).state != MapDataState::Ready) {
        for (const auto& member : original.Members()) {
            std::cerr << "sound-fixture member=" << MapDataKindId(member.kind) << " state=" << static_cast<unsigned>(member.state)
                << " sources=" << member.sources.size() << " records=" << (member.recordCount ? std::to_string(*member.recordCount) : "unknown") << '\n';
            for (const auto& source : member.sources) std::cerr << "  source=" << source.name << " bytes=" << source.data.size() << '\n';
            for (const auto& diagnostic : member.diagnostics) std::cerr << "  diagnostic=" << diagnostic.code << " " << diagnostic.message << '\n';
        }
        const auto parsed = ParseSounds(wire.bytes);
        std::cerr << "sound-fixture direct-parser ok=" << parsed.Succeeded() << " value=" << parsed.value.has_value() << " bytes=" << wire.bytes.size() << '\n';
        for (const auto& diagnostic : parsed.diagnostics) std::cerr << "  parser-diagnostic=" << diagnostic.code << " " << diagnostic.message << '\n';
    }
    Require(original.Sounds() && original.Member(MapDataKind::Sounds).state == MapDataState::Ready,
        "原创完整 classic W3S v1 来源须可读取");
    const auto key = MapFieldRowKey(original, MapDataKind::Sounds, 0);
    Require(key.Succeeded() && *key.value == "sound:52617720536f756e64", "声音稳定行键须按原名字 bytes 编码，保留空格");
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Sounds, 0, "volume", "80"),
        Edit(original, MapDataKind::Sounds, 0, "fade_in", "-1"), Edit(original, MapDataKind::Sounds, 0, "fade_out", "2147483647")});
    auto expected = wire.bytes;
    PatchInteger(expected, wire.offsets[0][0], 80); PatchInteger(expected, wire.offsets[0][1], -1);
    PatchInteger(expected, wire.offsets[0][2], std::numeric_limits<std::int32_t>::max());
    const auto& after = plan.After();
    Require(after.SourcePath() == original.SourcePath() && after.Member(MapDataKind::Sounds).sources.front().name == "WAR3MAP.W3S"
        && after.Member(MapDataKind::Sounds).sources.front().data == expected,
        "声音候选只改三个独立整数窗口，保留原路径、来源别名及其它所有字段和第二行");
    for (const auto& member : original.Members()) if (member.kind != MapDataKind::Sounds)
        Require(after.Member(member.kind).sources.size() == member.sources.size()
            && (member.sources.empty() || after.Member(member.kind).sources.front().data == member.sources.front().data),
            "声音候选不得改变其它地图成员");
    Require(std::bit_cast<std::uint32_t>(after.Sounds()->entries[0].pitch) == kSoundDefaultRealBits
        && std::bit_cast<std::uint32_t>(after.Sounds()->entries[0].pitchVariance) == 0x80000000u,
        "未修改的真实默认 sentinel 和负零须逐位保留");
    MapDataEditSession session(original);
    Require(session.ApplyPreparedFields(plan).Succeeded() && session.Revision() == 1 && session.Replacements().size() == 1,
        "三个声音字段只发布一个原会话命令");
    const auto receipt = session.LastEdit();
    Require(receipt.SessionIdentity() == session.SessionIdentity() && !receipt.HasPlacementLineage(),
        "声音回执属于原会话，不伪造摆放谱系");
    Require(session.Undo() && !session.IsDirty() && View(original, session).Member(MapDataKind::Sounds).sources.front().data == wire.bytes,
        "原 Undo 恢复完整 W3S 原字节");
    const auto revision = session.Revision();
    const auto refused = session.ApplyPreparedFields(plan, [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "SOUND_PUBLISH_DENIED", "拒绝声音发布");
    });
    Require(HasCode(refused, "SOUND_PUBLISH_DENIED") && session.Revision() == revision && session.CanRedo() && !session.IsDirty()
        && session.LastAppliedEdit().CommandIdentity() == receipt.CommandIdentity(), "声音发布拒绝保留原 redo、修订和命令身份");
    Require(session.Redo() && View(original, session).Member(MapDataKind::Sounds).sources.front().data == expected,
        "原 Redo 恢复同一完整 W3S 候选");
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty() && session.Replay(receipt, true).Succeeded(),
        "声音沿原 receipt 回放完整前后字节");
    MapDataEditSession foreign(original); Require(!foreign.Replay(receipt, true).Succeeded(), "声音回执拒绝跨会话回放");
    MapDataEditSession shared(original);
    Require(shared.ApplyPreparedFields(Plan(original, original, {Edit(original, MapDataKind::Units, 0, "owner", "2")})).Succeeded(),
        "声音之前的原单位命令可进入同一历史");
    const auto unitView = View(original, shared);
    Require(shared.ApplyPreparedFields(Plan(original, unitView, {Edit(unitView, MapDataKind::Sounds, 0, "volume", "80")})).Succeeded()
        && shared.Undo() && View(original, shared).Units()->units[0].owner == 2
        && View(original, shared).Member(MapDataKind::Sounds).sources.front().data == wire.bytes
        && shared.Undo() && !shared.IsDirty() && shared.Redo() && shared.Redo(),
        "声音与既有单位命令保持原共用 Undo/Redo 顺序，声音 Undo 保留前一域编辑");
}

void SoundDirectoryAliasesUseRealSnapshotsAndOriginalHistory() {
    const auto wire = SoundWire();
    const auto root = std::filesystem::temp_directory_path() / ("war3-field-sound-alias-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    for (const auto& member : SoundMembers(wire, "map/WAR3MAP.W3S")) {
        const auto path = root / member.name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(member.data.data()), static_cast<std::streamsize>(member.data.size()));
        Require(stream.good(), "声音目录别名夹具须写完整原成员字节");
    }
    const auto directory = DirectoryWorkspace::Open(root); Require(directory.Succeeded(), "声音目录别名须取得真实原快照");
    const auto loaded = MapDocument::FromDirectory(*directory.value); Require(loaded.Succeeded(), "声音目录别名通过真实 FromDirectory 读入");
    const auto& original = *loaded.value;
    Require(original.Sounds() && original.Member(MapDataKind::Sounds).state == MapDataState::Ready
        && original.Member(MapDataKind::Sounds).sources.size() == 1 && original.Member(MapDataKind::Sounds).sources.front().name == "map/WAR3MAP.W3S"
        && original.Member(MapDataKind::Sounds).sources.front().data == wire.bytes, "目录入口识别 map/ 大小写别名并保留完整来源身份");
    const auto plan = Plan(original, original, {Edit(original, MapDataKind::Sounds, 0, "volume", "80"),
        Edit(original, MapDataKind::Sounds, 0, "fade_in", "-1"), Edit(original, MapDataKind::Sounds, 0, "fade_out", "2147483647")});
    auto expected = wire.bytes;
    PatchInteger(expected, wire.offsets[0][0], 80); PatchInteger(expected, wire.offsets[0][1], -1);
    PatchInteger(expected, wire.offsets[0][2], std::numeric_limits<std::int32_t>::max());
    Require(plan.After().SourcePath() == original.SourcePath() && plan.After().Member(MapDataKind::Sounds).sources.front().name == "map/WAR3MAP.W3S"
        && plan.After().Member(MapDataKind::Sounds).sources.front().data == expected,
        "目录声音 Prepared 仅改独立整数窗口，不丢 map/ 来源别名、默认 sentinel、负零和另一行");
    for (const auto& member : original.Members()) if (member.kind != MapDataKind::Sounds)
        Require(plan.After().Member(member.kind).sources.front().name == member.sources.front().name
            && plan.After().Member(member.kind).sources.front().data == member.sources.front().data, "目录声音 Prepared 保留其它六成员原名字与完整字节");
    MapDataEditSession session(original); Require(session.ApplyPreparedFields(plan).Succeeded(), "目录声音沿同一原 Prepared 命令发布");
    const auto receipt = session.LastEdit(); const auto changed = View(original, session);
    const auto frozen = FreezeOriginalMapDataFields(original, changed);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 3 && frozen.value->Members().size() == 1
        && changed.Member(MapDataKind::Sounds).sources.front().name == "map/WAR3MAP.W3S" && changed.Member(MapDataKind::Sounds).sources.front().data == expected,
        "真实目录声音投影和累计冻结保留完整目标字节及来源别名");
    Require(session.Undo() && session.LastAppliedEdit().CommandIdentity() == receipt.CommandIdentity()
        && View(original, session).Member(MapDataKind::Sounds).sources.front().data == wire.bytes, "目录声音原 Undo 恢复完整 W3S 原字节");
    Require(session.Redo() && session.LastAppliedEdit().CommandIdentity() == receipt.CommandIdentity()
        && View(original, session).Member(MapDataKind::Sounds).sources.front().data == expected, "目录声音原 Redo 恢复同一命令完整修改字节");
    const auto sourceAgain = MapDocument::FromDirectory(*directory.value); Require(sourceAgain.Succeeded(), "原目录快照再次读入");
    for (const auto& member : original.Members())
        Require(sourceAgain.value->Member(member.kind).sources.front().name == member.sources.front().name
            && sourceAgain.value->Member(member.kind).sources.front().data == member.sources.front().data,
            "目录 Prepared、冻结、Undo 和 Redo 不改原目录任何成员字节");
}

void SoundValuesAndLateFailuresAreAtomic() {
    const auto original = MapDocument::FromMembers(SoundMembers(SoundWire()));
    for (const auto id : {"volume", "fade_in", "fade_out"}) {
        const auto* field = FindMapField(MapDataKind::Sounds, id);
        Require(field && field->type == MapFieldType::Int32 && field->writable, "E0 仅开放原声音整数目录");
        const auto sentinel = ParseMapFieldRaw(field->type, "-1");
        Require(sentinel.Succeeded() && IsMapFieldDefault(*field, *sentinel.value) && FormatMapFieldRaw(*sentinel.value) == "-1",
            "声音 -1 默认哨兵须 exact raw 往返");
        for (const auto value : {-1, 0, 127}) {
            auto edit = Edit(original, MapDataKind::Sounds, 0, id, std::to_string(value));
            Require(ValidateMapFieldValue(MapDataKind::Sounds, id, edit.value).Succeeded(), "声音默认和整数边界受原字段校验");
        }
        for (const auto text : {"1.5", "2147483648", "nan", "true", "f32:3f800000"})
            Require(!ParseMapFieldInput(*field, text).Succeeded(), "声音整数不接受小数、越界、布尔文本或浮点 raw");
    }
    const auto good = Edit(original, MapDataKind::Sounds, 0, "volume", "80");
    auto bad = Edit(original, MapDataKind::Sounds, 0, "fade_out", "-2");
    for (const auto value : {-2ll, 2147483648ll}) {
        bad.value.integer = value;
        Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::vector<MapFieldEdit>{good, bad}),
            "MAP_FIELD_VALUE_INVALID"), "合法声音前项加非法尾项须全批拒绝");
    }
    bad = good; bad.value.integer = 128;
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::span(&bad, 1)),
        "MAP_FIELD_VALUE_INVALID"), "音量 128 不能写回 W3S");
    bad = good; bad.value.type = MapFieldType::Float32; bad.value.floatBits = 0x3f800000u;
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::span(&bad, 1)),
        "MAP_FIELD_VALUE_INVALID"), "伪造浮点 typed value 不能穿过声音整数合同");
    bad = good; bad.expected.integer = 0;
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::span(&bad, 1)),
        "MAP_FIELD_VALUE_STALE"), "声音 expected_raw 必须与原 -1 精确相等");
    bad = good; bad.expectedRowKey = "sound:5261775f536f756e64";
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::span(&bad, 1)),
        "MAP_FIELD_ROW_STALE"), "编译符号规范化后的名字不能替代声音原行键");
    Require(HasCode(PrepareMapDataFields(original, original, std::string(64, '0'), std::span(&good, 1)), "MAP_FIELD_MEMBER_STALE")
        && HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::vector<MapFieldEdit>{good, good}), "MAP_FIELD_DUPLICATE"),
        "声音原成员 SHA 和同批重复字段仍由原门禁拒绝");
    const auto readonly = Edit(original, MapDataKind::Sounds, 0, "pitch", "1.5");
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), std::span(&readonly, 1)), "MAP_FIELD_READ_ONLY"),
        "E0 不扩大音调写权限");
    MapDataEditSession untouched(original);
    Require(!untouched.IsDirty() && untouched.Revision() == 0 && !untouched.CanUndo() && untouched.Replacements().empty(),
        "非法声音规划从未发布部分字节或历史");
}

void SoundFreezeProvesOriginalRawReplayAndRejectsWiderEdits() {
    const auto original = MapDocument::FromMembers(SoundMembers(SoundWire())); MapDataEditSession session(original);
    Require(session.ApplyPreparedFields(Plan(original, original, {Edit(original, MapDataKind::Sounds, 0, "volume", "80"),
        Edit(original, MapDataKind::Sounds, 0, "fade_in", "0"), Edit(original, MapDataKind::Sounds, 0, "fade_out", "-1")})).Succeeded(),
        "声音三个原字段发布一次");
    auto current = View(original, session);
    Require(session.ApplyPreparedFields(Plan(original, current, {Edit(current, MapDataKind::Sounds, 0, "volume", "81")})).Succeeded(),
        "同一声音字段可按当前 exact 前值再次修改");
    current = View(original, session);
    const auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 3 && frozen.value->Members().size() == 1,
        "声音重复修改合并为三个绑定原来源的冻结差异");
    std::vector<MapFieldEdit> replay;
    for (const auto& operation : frozen.value->Operations()) {
        const auto* field = FindMapField(MapDataKind::Sounds, operation.edit.field);
        const auto before = ReadMapField(original, MapDataKind::Sounds, 0, *field);
        Require(operation.expectedMemberSha256 == Sha(original, MapDataKind::Sounds) && before.Succeeded()
            && operation.edit.expected == *before.value && operation.edit.expectedRowKey == "sound:52617720536f756e64",
            "冻结声音操作绑定最初 W3S SHA、原行键和 exact 原值");
        replay.push_back(operation.edit);
    }
    const auto tables = ApplyMapFieldEdits(original, replay); MapDataEditSession saving(original);
    Require(tables.Succeeded() && tables.value->sounds && saving.EditSounds(*tables.value->sounds).Succeeded()
        && saving.Replacements().size() == 1 && saving.Replacements().front().data == current.Member(MapDataKind::Sounds).sources.front().data,
        "冻结操作沿原保存 CommitTable 等效入口回放完整 W3S，与密封候选逐字节相等");
    for (const auto change : {0, 1, 2, 3, 4}) {
        auto sounds = *original.Sounds();
        if (change == 0) sounds.entries[0].filePath = "Changed.wav";
        if (change == 1) sounds.entries[0].name = "Other Sound";
        if (change == 2) std::swap(sounds.entries[0], sounds.entries[1]);
        if (change == 3) sounds.entries.pop_back();
        if (change == 4) { auto added = sounds.entries[0]; added.name = "gg_snd_Added"; sounds.entries.push_back(std::move(added)); }
        MapDataEditSession wider(original); Require(wider.EditSounds(sounds).Succeeded(), "原更宽声音入口可建立真实未授权差异");
        Require(HasCode(FreezeOriginalMapDataFields(original, View(original, wider)), "MAP_FIELD_PROJECTION_SCOPE"),
            "字段冻结不掩盖路径、名字、顺序或行增删的额外变化");
    }
    current = View(original, session);
    Require(session.ApplyPreparedFields(Plan(original, current, {Edit(current, MapDataKind::Sounds, 0, "volume", "-1"),
        Edit(current, MapDataKind::Sounds, 0, "fade_in", "11"), Edit(current, MapDataKind::Sounds, 0, "fade_out", "22")})).Succeeded(),
        "三个声音字段可恢复原值");
    const auto zero = FreezeOriginalMapDataFields(original, View(original, session));
    Require(zero.Succeeded() && zero.value->Operations().empty() && zero.value->Members().empty() && !session.IsDirty(),
        "声音净零恢复得到空冻结和完整原字节");
}

void SoundSourceVersionAndNameGuardsStayReadonly() {
    const auto safe = MapDocument::FromMembers(SoundMembers(SoundWire()));
    const auto good = Edit(safe, MapDataKind::Sounds, 0, "volume", "80");
    for (const auto version : {2u, 3u}) {
        const auto wire = SoundWire({"Raw Sound", "gg_snd_Unchanged"}, version);
        const auto extended = MapDocument::FromMembers(SoundMembers(wire));
        Require(extended.Sounds() && extended.Sounds()->version == version && extended.Sounds()->entries[0].extended
            && extended.Member(MapDataKind::Sounds).sources.front().data == wire.bytes, "真实 v2/v3 扩展只读解析并保留原成员");
        Require(!PrepareMapDataFields(extended, extended, Sha(extended, MapDataKind::Sounds), std::span(&good, 1)).Succeeded()
            && !ApplyMapFieldEdits(extended, std::span(&good, 1)).Succeeded(),
            "prepared 和原公开字段入口均不能将扩展 W3S 重建为 v1");
    }
    for (const auto damage : {0, 1, 2, 3}) {
        auto members = SoundMembers(SoundWire());
        if (damage == 0) members.pop_back();
        if (damage == 1) members.back().data.push_back(std::byte{0x7f});
        if (damage == 2) PatchInteger(members.back().data, 0, 99);
        if (damage == 3) members.push_back({"war3map.w3s", members.back().data});
        const auto unsafe = MapDocument::FromMembers(members);
        Require(unsafe.Member(MapDataKind::Sounds).state != MapDataState::Ready
            && HasCode(PrepareMapDataFields(unsafe, unsafe, Sha(safe, MapDataKind::Sounds), std::span(&good, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
            "缺失、未知尾部、未知版本和重复 native 来源不能补建为声音空表");
    }
    const auto duplicate = MapDocument::FromMembers(SoundMembers(SoundWire({"Raw Sound", "Raw Sound"})));
    Require(duplicate.Sounds() && duplicate.Member(MapDataKind::Sounds).state == MapDataState::Partial
        && HasCode(PrepareMapDataFields(duplicate, duplicate, Sha(duplicate, MapDataKind::Sounds), std::span(&good, 1)), "MAP_FIELD_SOURCE_UNSAFE"),
        "重复原名字继续可读但不授予写权限");
    for (const auto& names : {std::vector<std::string>{"Ambient Bell", "Ambient_Bell"},
        std::vector<std::string>{"Ambient Bell", "gg_snd_Ambient_Bell"}}) {
        const auto ambiguous = MapDocument::FromMembers(SoundMembers(SoundWire(names)));
        const auto edit = Edit(ambiguous, MapDataKind::Sounds, 0, "volume", "80");
        Require(ambiguous.Member(MapDataKind::Sounds).state == MapDataState::Ready
            && HasCode(PrepareMapDataFields(ambiguous, ambiguous, Sha(ambiguous, MapDataKind::Sounds), std::span(&edit, 1)), "MAP_FIELD_SOUND_NAME_AMBIGUOUS")
            && HasCode(ApplyMapFieldEdits(ambiguous, std::span(&edit, 1)), "MAP_FIELD_SOUND_NAME_AMBIGUOUS"),
            "原名字唯一仍须由 prepared 和公开字段入口共同拒绝实际 gg_snd 符号碰撞");
        auto changed = *ambiguous.Sounds(); changed.entries[0].volume = 80;
        MapDataEditSession wider(ambiguous); Require(wider.EditSounds(changed).Succeeded(), "较宽入口可建立碰撞名字的数值候选");
        Require(HasCode(FreezeOriginalMapDataFields(ambiguous, View(ambiguous, wider)), "MAP_FIELD_SOUND_NAME_AMBIGUOUS"),
            "累计冻结复用同一个声音名字资格，不放行绕过 prepared 的差异");
    }
}

void SoundCumulativeBudgetRejectsBeforePublication() {
    std::vector<std::string> names;
    for (std::size_t row = 0; row < 33; ++row) names.push_back("gg_snd_Budget" + std::to_string(row));
    const auto original = MapDocument::FromMembers(SoundMembers(SoundWire(names))); MapDataEditSession session(original);
    std::vector<MapFieldEdit> edits;
    for (std::size_t row = 0; row < 32; ++row) edits.push_back(Edit(original, MapDataKind::Sounds, row, "volume", "80"));
    auto tooMany = edits; tooMany.push_back(Edit(original, MapDataKind::Sounds, 32, "volume", "80"));
    Require(HasCode(PrepareMapDataFields(original, original, Sha(original, MapDataKind::Sounds), tooMany), "MAP_FIELD_EDIT_COUNT"),
        "同请求第 33 个声音字段在规划时拒绝");
    Require(session.ApplyPreparedFields(Plan(original, original, edits)).Succeeded(), "32 个累计声音字段恰好可发布");
    const auto current = View(original, session); const auto revision = session.Revision(); const auto receipt = session.LastEdit();
    const auto frozen = FreezeOriginalMapDataFields(original, current);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 32, "32 个原来源声音差异恰好可冻结");
    const auto over = Plan(original, current, {Edit(current, MapDataKind::Sounds, 32, "volume", "80")});
    const auto denied = session.ApplyPreparedFields(over, [&](const auto&, const auto& after, const auto&) {
        const auto checked = FreezeOriginalMapDataFields(original, after); return Status{checked.diagnostics};
    });
    Require(HasCode(denied, "MAP_FIELD_CUMULATIVE_COUNT") && session.Revision() == revision
        && session.LastEdit().CommandIdentity() == receipt.CommandIdentity() && Sha(View(original, session), MapDataKind::Sounds) == Sha(current, MapDataKind::Sounds)
        && session.Undo() && !session.IsDirty() && session.Redo(),
        "累计第 33 个声音差异在发布前拒绝，保留原字节、修订、身份和完整 Undo/Redo");
}

void ProjectPreservesUnsafeSourcesAndContext() {
    auto members = Members(); members.push_back({"WAR3MAPUNITS.DOO", members[1].data}); members[2].data = {std::byte{1}, std::byte{2}};
    const auto original = MapDocument::FromMembers(members); MapDataEditSession session(original);
    Require(original.Member(MapDataKind::Units).state == MapDataState::Ambiguous
        && original.Member(MapDataKind::Doodads).state == MapDataState::Invalid, "actual conflicting and damaged wire sources");
    Require(session.ApplyPreparedFields(Plan(original, original, {Edit(original, MapDataKind::Regions, 0, "left", "-9")})).Succeeded(), "unrelated valid region field remains editable");
    const auto current = View(original, session);
    for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads}) {
        const auto& a = original.Member(kind); const auto& b = current.Member(kind);
        Require(a.state == b.state && a.sources.size() == b.sources.size() && a.diagnostics.size() == b.diagnostics.size(), "unsafe source state never becomes Missing");
        for (std::size_t i = 0; i < a.sources.size(); ++i) Require(a.sources[i].name == b.sources[i].name && a.sources[i].data == b.sources[i].data, "retain every conflicting source byte");
        for (std::size_t i = 0; i < a.diagnostics.size(); ++i) Require(a.diagnostics[i].code == b.diagnostics[i].code && a.diagnostics[i].message == b.diagnostics[i].message, "retain exact source diagnostics");
    }
    Require(current.Settings()->formatVersion == 25 && current.ResolveString("TRIGSTR_0042").value == std::optional<std::string>("Resolved Region"), "project retains W3I and WTS context");
    const auto bad = PrepareMapDataFields(original, current, std::string(64, 'a'),
        std::vector<MapFieldEdit>{{MapDataKind::Units, 0, "unproven-row", "owner", MapFieldValue{}, MapFieldValue{}}});
    Require(HasCode(bad, "MAP_FIELD_SOURCE_UNSAFE"), "ambiguous source cannot be overwritten by a fresh typed table");
    // 真实目录输入超过读取预算，而不是手工伪造 Unreadable 元数据。
    const auto root = std::filesystem::temp_directory_path() / ("war3-field-unreadable-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    std::filesystem::create_directories(root);
    { std::ofstream stream(root / "war3map.w3s", std::ios::binary); stream.seekp(128U * 1024U * 1024U); stream.put('x'); Require(stream.good(), "oversized synthetic native source"); }
    auto workspace = DirectoryWorkspace::Open(root); Require(workspace.Succeeded(), "snapshot synthetic readonly input");
    auto unreadable = MapDocument::FromDirectory(*workspace.value); Require(unreadable.Succeeded(), "read source with budget diagnostics");
    Require(unreadable.value->Member(MapDataKind::Sounds).state == MapDataState::Unreadable, "real over-budget source is Unreadable");
    MapDataEditSession isolated(*unreadable.value); const auto projected = ProjectMapData(*unreadable.value, isolated);
    Require(projected.Succeeded() && projected.value->Member(MapDataKind::Sounds).state == MapDataState::Unreadable
        && projected.value->Member(MapDataKind::Sounds).diagnostics.front().code == "MAP_MEMBER_TOO_LARGE", "projector preserves unavailable source and reason");
}
} // namespace

int main() {
    try {
        AtomicFieldsUseOriginalReceiptsAndIdentityLineage(); RegionBatchChecksFinalBoundsAndPreservesRedoOnDenial();
        FreezeCoalescesOriginalBoundFieldsAndProvesNativeBytes(); CumulativeLimitRejectsBeforePublication(); ProjectPreservesUnsafeSourcesAndContext();
        UnitPositionChangesOnlyNativeWindowAndOriginalUndo(); UnitPositionInputAndAtomicRowGuards();
        UnitPositionReadonlyLayoutsAndReentrantPublication(); UnitPositionFreezePreservesAliasesLniAndPriorAuthorEdits(); HeroLevelIsWritableWithLowerBound();
        MapTextFieldsPreserveWireAndOriginalUndo(); MapLiteralTextFieldsPreserveWireAndOriginalUndo(); MapTextFieldsRejectUnsafeSourcesAndLateValues();
        SettingsReferencesUseUniqueSharedWtsAndOriginalHistory(); SettingsReferencesRejectConflictsUnsafeAndDriftingSources();
        SettingsReferencePatchBindsTheOriginalWtsSource();
        SoundFieldsUseExactNativeWindowsAndOriginalHistory(); SoundDirectoryAliasesUseRealSnapshotsAndOriginalHistory(); SoundValuesAndLateFailuresAreAtomic();
        SoundFreezeProvesOriginalRawReplayAndRejectsWiderEdits(); SoundSourceVersionAndNameGuardsStayReadonly(); SoundCumulativeBudgetRejectsBeforePublication();
        std::cout << "地图字段规划：22 组场景通过\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
