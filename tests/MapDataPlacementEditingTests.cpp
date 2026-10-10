#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/Hash.h"
#include "MapDataFixtures.h"
#include "LniMapSettingsFixtures.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

const std::vector<std::byte>& Bytes(const MapDocument& document, MapDataKind kind) {
    const auto& member = document.Member(kind);
    Require(member.sources.size() == 1, "测试要求唯一实际 native 来源");
    return member.sources.front().data;
}

std::string Sha(const MapDocument& document, MapDataKind kind) {
    const auto sha = Sha256(Bytes(document, kind));
    Require(sha.Succeeded(), "实际 native SHA 应可计算");
    return *sha.value;
}

MapDataKind Kind(PlacementFile file) {
    return file == PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
}

// 三个单位与两个普通地物都保有明确 classic 布局；特殊尾不是普通 transform 行。
std::vector<MapArchiveMember> Members() {
    UnitPlacements units;
    UnitPlacement unit;
    unit.rawcode = "sloc";
    unit.owner = 0;
    unit.creationId = 10;
    unit.position = {100, 200, 64};
    unit.scale = {1, 1, 1};
    unit.random = RandomAnyUnit{4, 2};
    units.units.push_back(unit);
    unit.rawcode = "hfoo";
    unit.owner = 5;
    unit.creationId = 20;
    unit.position = {300, 400, 0};
    unit.scale = {1.5f, 0.75f, 2};
    unit.flags = 7;
    unit.unknownOwnerBytes = {0x12, 0x34};
    unit.hitPoints = 173;
    unit.mana = 87;
    unit.droppedItemTable = 9;
    unit.itemSets = {{{{"I000", 85}, {"I003", 15}}}, {{{"I004", 100}}}};
    unit.inventory = {{2, "I001"}, {5, "I005"}};
    unit.abilities = {{"A000", true, 3}, {"A001", false, 2}};
    unit.random = RandomCustomTable{{{"hpea", 65}, {"hrif", 35}}};
    unit.gold = 83;
    unit.targetAcquisition = -1;
    unit.heroLevel = 4;
    unit.heroStrength = 7;
    unit.heroAgility = 8;
    unit.heroIntelligence = 9;
    unit.teamColor = 5;
    unit.waygateRegionId = 17;
    units.units.push_back(unit);
    UnitPlacement worker;
    worker.rawcode = "hpea";
    worker.owner = 1;
    worker.creationId = 37;
    worker.position = {-128, 768, 16};
    worker.scale = {1, 2, 0.5f};
    worker.rotation = 1;
    worker.random = RandomAnyUnit{6, 1};
    units.units.push_back(worker);

    DoodadPlacements doodads;
    DoodadPlacement doodad;
    doodad.rawcode = "LTlt";
    doodad.creationId = 5;
    doodad.position = {-50, 60, 8};
    doodad.scale = {1.25f, 0.5f, 3};
    doodad.flags = 2;
    doodad.lifePercent = 37;
    doodad.droppedItemTable = 8;
    doodad.itemSets = {{{{"I002", 60}, {"I003", 40}}}};
    doodads.doodads.push_back(doodad);
    doodad.rawcode = "LOrk";
    doodad.creationId = 6;
    doodad.position = {70, 80, 0};
    doodad.rotation = 1;
    doodad.itemSets.clear();
    doodads.doodads.push_back(doodad);
    doodads.specialDoodads.push_back({"LTbx", 9, {3, -4}});

    RegionsDocument regions;
    RegionRecord region{};
    region.left = -10;
    region.bottom = -20;
    region.right = 30;
    region.top = 40;
    region.name = "作者区域";
    region.creationNumber = 19;
    regions.entries.push_back(region);
    const auto unitBytes = units.Serialize();
    const auto doodadBytes = doodads.Serialize();
    const auto regionBytes = regions.Serialize();
    Require(unitBytes.Succeeded() && doodadBytes.Succeeded() && regionBytes.Succeeded(),
        "实际类型化 fixture 必须经原 writer 与 reader 往返");
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value},
        {"war3map.w3r", *regionBytes.value}};
}

MapDocument Original() {
    const auto document = MapDocument::FromMembers(Members());
    Require(document.Units() && document.Doodads() && document.Regions()
        && document.Units()->recordLayout == PlacementRecordLayout::Classic
        && document.Doodads()->recordLayout == PlacementRecordLayout::Classic,
        "fixture 必须是可证明的原 classic 文档");
    return document;
}

MapDataPlacementSource Source(const MapDocument& document, PlacementFile file, std::uint32_t row) {
    const auto kind = Kind(file);
    PlacementId id;
    if (file == PlacementFile::Units) {
        Require(document.Units() && row < document.Units()->units.size(), "实际单位来源行必须存在");
        id = MakePlacementId(file, row, document.Units()->units[row]);
    } else {
        Require(document.Doodads() && row < document.Doodads()->doodads.size(), "实际普通地物来源行必须存在");
        id = MakePlacementId(file, row, document.Doodads()->doodads[row]);
    }
    const auto rowKey = MapFieldRowKey(document, kind, row);
    Require(rowKey.Succeeded(), "实际来源 row key 应可读取");
    return {std::move(id), *rowKey.value, Sha(document, kind)};
}

MapDataPlacementIntent Rotation(const MapDocument& document, PlacementFile file,
    std::uint32_t row, float radians) {
    return MapDataPlacementRotation{Source(document, file, row), radians};
}

MapDataPlacementIntent Copy(const MapDocument& document, PlacementFile file,
    std::initializer_list<std::uint32_t> rows, std::array<float, 3> offset = {64, -32, 4}) {
    MapDataPlacementCopy copy;
    copy.offset = offset;
    for (const auto row : rows) copy.sources.push_back(Source(document, file, row));
    return copy;
}

PreparedMapDataPlacement Plan(const MapDocument& original, const MapDocument& current,
    const MapDataPlacementIntent& intent, PlacementCommandLimits limits = {}) {
    const auto plan = PrepareMapDataPlacement(original, current, intent, limits);
    Require(plan.Succeeded() && plan.value, "共用 placement planner 应生成完整候选");
    return *plan.value;
}

MapDocument View(const MapDocument& original, const MapDataEditSession& session) {
    auto view = ProjectMapData(original, session);
    Require(view.Succeeded(), "应投影原来源与当前 typed overlay");
    return std::move(*view.value);
}

void RequireSameMembers(const MapDocument& expected, const MapDocument& actual,
    std::optional<MapDataKind> changedKind = {}) {
    for (const auto& member : expected.Members()) {
        if (changedKind && member.kind == *changedKind) continue;
        const auto& observed = actual.Member(member.kind);
        Require(observed.state == member.state && observed.memberName == member.memberName
            && observed.sources.size() == member.sources.size()
            && observed.recordCount == member.recordCount
            && observed.diagnostics.size() == member.diagnostics.size(), "未编辑来源状态和 presence 必须保留");
        for (std::size_t row = 0; row < member.sources.size(); ++row) {
            Require(observed.sources[row].name == member.sources[row].name
                && observed.sources[row].data == member.sources[row].data, "未编辑来源 native bytes 必须完全保留");
        }
    }
}

std::vector<std::byte> RowBytes(const UnitPlacement& unit) {
    UnitPlacements table;
    table.units.push_back(unit);
    auto bytes = table.Serialize();
    Require(bytes.Succeeded(), "完整单位行必须经原 writer 序列化");
    return std::move(*bytes.value);
}

std::vector<std::byte> RowBytes(const DoodadPlacement& doodad) {
    DoodadPlacements table;
    table.doodads.push_back(doodad);
    auto bytes = table.Serialize();
    Require(bytes.Succeeded(), "完整普通地物行必须经原 writer 序列化");
    return std::move(*bytes.value);
}

void RequireLineage(std::span<const PlacementRowLineage> lineage, MapDataKind kind,
    std::uint32_t beforeCount, std::uint32_t afterCount) {
    Require(lineage.size() == 1 && lineage.front().kind == kind
        && lineage.front().beforeCount == beforeCount && lineage.front().afterCount == afterCount
        && lineage.front().beforeToAfter.size() == beforeCount, "应保有真实普通行谱系而非按 rawcode 推测");
    for (std::uint32_t row = 0; row < beforeCount; ++row)
        Require(lineage.front().beforeToAfter[row] == row, "保留行映射必须逐行保持身份，新行不能冒充旧行");
}

void RotationPreservesCompleteRecordsAndStartLocationSettings() {
    const auto original = Original();
    MapDataEditSession identityBefore(original);
    const auto purePlan = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 2));
    Require(purePlan.OriginalReplayIntent().has_value(), "纯 planner 应保有原来源重放意图");
    const auto pureFreeze = FreezeOriginalMapDataPlacement(original, purePlan.After(), *purePlan.OriginalReplayIntent());
    Require(pureFreeze.Succeeded(), "纯冻结应只重放 native 候选");
    MapDataEditSession identityAfter(original);
    Require(identityAfter.SessionIdentity() == identityBefore.SessionIdentity() + 1,
        "Prepare 与 Freeze 不得创建隔离会话或消耗全局 session identity");
    for (const auto& selection : std::array<std::pair<PlacementFile, std::uint32_t>, 3>{{
        {PlacementFile::Units, 1}, {PlacementFile::Doodads, 0}, {PlacementFile::Units, 0}}}) {
        const auto [file, row] = selection;
        const auto plan = Plan(original, original, Rotation(original, file, row, 2.5f));
        Require(plan.NewIds().empty() && plan.OriginalReplayIntent(), "绝对朝向应保留原始行定位且不新增实例");
        RequireSameMembers(original, plan.After(), plan.Kind());
        Require(plan.BeforeMemberSha256() == Sha(original, plan.Kind())
            && plan.AfterMemberSha256() == Sha(plan.After(), plan.Kind()), "plan SHA 必须绑定实际 native 前后像");
        const auto source = Source(original, file, row);
        const auto transform = FindPlacement(original, source.id);
        Require(transform.Succeeded(), "原 transform 应可读取");
        MapDataEditSession legacy(original);
        const PlacementMove move{source.id, transform.value->position, 2.5f};
        Require(MovePlacements(legacy, std::span(&move, 1)).Succeeded(), "原 MovePlacements 朝向命令应可执行");
        Require(Bytes(plan.After(), plan.Kind()) == Bytes(View(original, legacy), plan.Kind()),
            "prepared rotation 必须精确复用原绝对朝向和规范化规则");
        if (file == PlacementFile::Units) {
            auto restored = *plan.After().Units();
            Require(restored.units[row].position == original.Units()->units[row].position
                && restored.units[row].scale == original.Units()->units[row].scale, "朝向编辑不得改变位置或缩放");
            restored.units[row].rotation = original.Units()->units[row].rotation;
            const auto bytes = restored.Serialize();
            Require(bytes.Succeeded() && *bytes.value == Bytes(original, MapDataKind::Units), "单位其余全部字段应逐字节保留");
        } else {
            auto restored = *plan.After().Doodads();
            Require(restored.doodads[row].position == original.Doodads()->doodads[row].position
                && restored.doodads[row].scale == original.Doodads()->doodads[row].scale, "地物朝向不得改变位置或缩放");
            restored.doodads[row].rotation = original.Doodads()->doodads[row].rotation;
            const auto bytes = restored.Serialize();
            Require(bytes.Succeeded() && *bytes.value == Bytes(original, MapDataKind::Doodads), "地物其余字段及完整特殊尾应逐字节保留");
        }
        MapDataEditSession session(original);
        Require(session.ApplyPreparedPlacement(plan).Succeeded() && session.Revision() == 1
            && session.Replacements().size() == 1, "朝向编辑应在原会话发布一次命令，sloc 不联动 W3I");
        RequireSameMembers(plan.After(), View(original, session));
        Require(session.Undo() && !session.IsDirty() && session.Redo(), "朝向命令必须进入原 Undo/Redo 历史");
        RequireSameMembers(plan.After(), View(original, session));
    }
}

void CopyPreservesNestedRecordsRequestOrderAndOriginalReceipts() {
    const auto original = Original();
    const auto plan = Plan(original, original, Copy(original, PlacementFile::Units, {2, 1}));
    Require(plan.NewIds().size() == 2 && plan.NewIds()[0].rawcode == "hpea" && plan.NewIds()[0].index == 3
        && plan.NewIds()[0].creationId == 38 && plan.NewIds()[1].rawcode == "hfoo"
        && plan.NewIds()[1].index == 4 && plan.NewIds()[1].creationId == 39,
        "新身份应依请求顺序返回并使用真实下一创建编号");
    RequireLineage(plan.Lineage(), MapDataKind::Units, 3, 5);
    RequireSameMembers(original, plan.After(), MapDataKind::Units);
    Require(plan.OriginalReplayIntent().has_value(), "原始来源 copy 应有绑定原 SHA 的保存意图");
    const auto copyFreeze = FreezeOriginalMapDataPlacement(original, plan.After(), *plan.OriginalReplayIntent());
    Require(copyFreeze.Succeeded() && copyFreeze.value->Members().size() == 1
        && copyFreeze.value->Members().front().data == Bytes(plan.After(), MapDataKind::Units),
        "原单次 copy 意图必须精确重放完整单位 native");
    for (std::size_t row = 0; row < original.Units()->units.size(); ++row)
        Require(RowBytes(plan.After().Units()->units[row]) == RowBytes(original.Units()->units[row]), "复制不得改写源行");
    const std::array<std::uint32_t, 2> sourceRows{2, 1};
    for (std::size_t index = 0; index < sourceRows.size(); ++index) {
        const auto& source = original.Units()->units[sourceRows[index]];
        auto restored = plan.After().Units()->units[3 + index];
        Require(restored.position == std::array<float, 3>{source.position[0] + 64, source.position[1] - 32, source.position[2] + 4},
            "复制坐标应使用请求 XYZ 偏移");
        restored.position = source.position;
        restored.creationId = source.creationId;
        Require(RowBytes(restored) == RowBytes(source), "完整 clone 必须保留掉落、物品、技能、随机配置和未知字节");
    }
    MapDataEditSession session(original);
    Require(session.ApplyPreparedPlacement(plan).Succeeded(), "prepared copy 应提交原会话");
    const auto receipt = session.LastEdit();
    Require(receipt.SessionIdentity() == session.SessionIdentity() && receipt.HasPlacementLineage(), "应返回原会话真实回执");
    RequireLineage(receipt.PlacementLineage(), MapDataKind::Units, 3, 5);
    const auto receiptBytes = receipt.PlacementMemberBytes(MapDataKind::Units, true);
    Require(receiptBytes && std::equal(receiptBytes->begin(), receiptBytes->end(),
        plan.AfterMemberBytes().begin(), plan.AfterMemberBytes().end()), "回执必须拥有实际 after native");
    Require(session.Undo() && !session.IsDirty() && session.Redo(), "一条原命令应完整撤销及恢复复制批次");
    RequireSameMembers(plan.After(), View(original, session));
    Require(session.Replay(receipt, false).Succeeded() && !session.IsDirty()
        && session.Replay(receipt, true).Succeeded(), "同一原回执应可精确双向重放");
    MapDataEditSession foreign(original);
    Require(!foreign.Replay(receipt, false).Succeeded(), "外来会话不得重放该回执");
    Require(!session.ApplyPreparedPlacement(plan).Succeeded(), "已发布 plan 的 before image 应被判陈旧");

    const auto doodadPlan = Plan(original, original, Copy(original, PlacementFile::Doodads, {1, 0}));
    Require(doodadPlan.NewIds().size() == 2 && doodadPlan.NewIds()[0].rawcode == "LOrk"
        && doodadPlan.NewIds()[0].creationId == 7 && doodadPlan.NewIds()[1].rawcode == "LTlt"
        && doodadPlan.NewIds()[1].creationId == 8, "普通地物复制也应保有请求顺序和独立创建编号");
    RequireLineage(doodadPlan.Lineage(), MapDataKind::Doodads, 2, 4);
    Require(doodadPlan.OriginalReplayIntent().has_value(), "原始普通地物 copy 应有原来源保存意图");
    const auto doodadFreeze = FreezeOriginalMapDataPlacement(original, doodadPlan.After(), *doodadPlan.OriginalReplayIntent());
    Require(doodadFreeze.Succeeded() && doodadFreeze.value->Members().size() == 1
        && doodadFreeze.value->Members().front().data == Bytes(doodadPlan.After(), MapDataKind::Doodads),
        "地物 copy 重放应包含完整特殊尾 native");
    auto restoredDoodads = *doodadPlan.After().Doodads();
    restoredDoodads.doodads.resize(2);
    const auto restoredBytes = restoredDoodads.Serialize();
    Require(restoredBytes.Succeeded() && *restoredBytes.value == Bytes(original, MapDataKind::Doodads),
        "普通地物复制不得改动原行或特殊尾");
    auto copiedTree = doodadPlan.After().Doodads()->doodads[3];
    copiedTree.position = original.Doodads()->doodads[0].position;
    copiedTree.creationId = original.Doodads()->doodads[0].creationId;
    Require(RowBytes(copiedTree) == RowBytes(original.Doodads()->doodads[0]), "普通地物嵌套掉落表应完整复制");
}

void InvalidSourcesAndLateFailuresPublishNothing() {
    const auto original = Original();
    MapDataEditSession untouched(original);
    std::vector<MapDataPlacementIntent> rejected;
    auto lateBad = std::get<MapDataPlacementCopy>(Copy(original, PlacementFile::Units, {1, 2}));
    lateBad.sources.back().expectedMemberSha256 = std::string(64, '0');
    rejected.push_back(lateBad);
    auto wrongKey = std::get<MapDataPlacementRotation>(Rotation(original, PlacementFile::Units, 1, 2));
    wrongKey.source.expectedRowKey += ":stale";
    rejected.push_back(wrongKey);
    auto wrongIdentity = wrongKey;
    wrongIdentity.source = Source(original, PlacementFile::Units, 1);
    wrongIdentity.source.id.creationId = 999;
    rejected.push_back(wrongIdentity);
    rejected.push_back(Copy(original, PlacementFile::Units, {1, 1}));
    rejected.push_back(Copy(original, PlacementFile::Units, {0}));
    rejected.push_back(MapDataPlacementCopy{});
    rejected.push_back(Copy(original, PlacementFile::Units, {1}, {1000000, 0, 0}));
    auto mixed = std::get<MapDataPlacementCopy>(Copy(original, PlacementFile::Units, {1}));
    mixed.sources.push_back(Source(original, PlacementFile::Doodads, 0));
    rejected.push_back(mixed);
    const auto specialRow = static_cast<std::uint32_t>(original.Doodads()->doodads.size());
    const auto specialKey = MapFieldRowKey(original, MapDataKind::Doodads, specialRow);
    Require(specialKey.Succeeded(), "只读特殊尾应有实际 row key");
    const MapDataPlacementSource special{{PlacementFile::Doodads, specialRow, "LTbx", 0},
        *specialKey.value, Sha(original, MapDataKind::Doodads)};
    rejected.push_back(MapDataPlacementRotation{special, 2});
    rejected.push_back(MapDataPlacementCopy{{special}, {1, 0, 0}});
    for (const auto radians : {-1.0f, 6.28318530718f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
        rejected.push_back(Rotation(original, PlacementFile::Units, 1, radians));
    for (const auto& intent : rejected) {
        const auto failure = PrepareMapDataPlacement(original, original, intent);
        Require(!failure.Succeeded() && !failure.value, "无效或后出现的来源必须整批拒绝且不暴露半成品");
        Require(untouched.Revision() == 0 && !untouched.CanUndo() && !untouched.CanRedo()
            && !untouched.IsDirty() && untouched.LastAppliedEdit().CommandIdentity() == 0,
            "纯准备失败不得发布 bytes、历史、修订或命令身份");
        RequireSameMembers(original, View(original, untouched));
    }
    auto cappedMembers = Members();
    auto cappedUnits = *original.Units();
    cappedUnits.units[2].creationId = (std::numeric_limits<std::int32_t>::max)();
    const auto cappedBytes = cappedUnits.Serialize();
    Require(cappedBytes.Succeeded(), "创建编号极限来源仍是合法已有 native");
    cappedMembers[1].data = *cappedBytes.value;
    const auto capped = MapDocument::FromMembers(cappedMembers);
    Require(!PrepareMapDataPlacement(capped, capped, Copy(capped, PlacementFile::Units, {1})).Succeeded(),
        "无下一创建编号时不得生成 copy 候选");
}

void ValidatorFailureKeepsRedoAndConsumesNoCommandIdentity() {
    const auto original = Original();
    MapDataEditSession session(original);
    const auto first = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 1));
    Require(session.ApplyPreparedPlacement(first).Succeeded() && session.Undo() && session.CanRedo(), "预先建立原 redo 命令");
    const auto before = View(original, session);
    const auto revision = session.Revision();
    const auto lastAppliedIdentity = session.LastAppliedEdit().CommandIdentity();
    const auto next = Plan(original, before, Rotation(before, PlacementFile::Units, 1, 2));
    std::uint64_t preparedIdentity = 0;
    const auto denied = session.ApplyPreparedPlacement(next, [&](const auto& actualBefore, const auto& actualAfter, const auto& receipt) {
        RequireSameMembers(before, actualBefore);
        RequireSameMembers(next.After(), actualAfter);
        Require(session.Revision() == revision && session.CanRedo() && !session.IsDirty(), "validator 必须在原发布之前运行");
        RequireLineage(receipt.PlacementLineage(), MapDataKind::Units, 3, 3);
        preparedIdentity = receipt.CommandIdentity();
        return Status::Failure("Test", "DENIED", "拒绝候选");
    });
    Require(HasCode(denied, "DENIED") && preparedIdentity != 0 && session.Revision() == revision
        && !session.CanUndo() && session.CanRedo() && !session.IsDirty()
        && session.LastAppliedEdit().CommandIdentity() == lastAppliedIdentity, "拒绝不得消耗身份或丢弃 redo");
    RequireSameMembers(before, View(original, session));
    bool threw = false;
    try {
        (void)session.ApplyPreparedPlacement(next, [](const auto&, const auto&, const auto&) -> Status {
            throw std::runtime_error("测试 validator 抛出异常");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw && session.Revision() == revision && session.CanRedo()
        && session.LastAppliedEdit().CommandIdentity() == lastAppliedIdentity, "validator 异常也应保留原历史与计数");
    RequireSameMembers(before, View(original, session));
    Require(session.ApplyPreparedPlacement(next).Succeeded()
        && session.LastEdit().CommandIdentity() == preparedIdentity, "重新发布应复用未被失败消耗的下一身份");
}

void ReentrantValidatorKeepsNestedEditAndUndo() {
    const auto original = Original();
    MapDataEditSession edited(original);
    const auto outer = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 2));
    auto nestedUnits = *original.Units();
    nestedUnits.units[2].mana = 42;
    const auto nestedBytes = nestedUnits.Serialize();
    Require(nestedBytes.Succeeded(), "嵌套命令来源应可完整序列化");
    const auto denied = edited.ApplyPreparedPlacement(outer, [&](const auto&, const auto&, const auto&) {
        Require(edited.EditUnits(nestedUnits).Succeeded(), "validator内真实EditUnits应能独立发布");
        return Status::Ok();
    });
    Require(HasCode(denied, "MAP_EDIT_PREPUBLISH_STALE") && edited.Revision() == 1 && edited.CanUndo()
        && !edited.CanRedo() && edited.LastAppliedEdit().CommandIdentity() == 1
        && Bytes(View(original, edited), MapDataKind::Units) == *nestedBytes.value,
        "外层过期候选不得覆盖嵌套编辑或消耗另一身份");
    Require(edited.Undo() && edited.CanRedo() && !edited.IsDirty() && edited.Redo(), "嵌套原命令的UndoRedo应完整保留");
    Require(Bytes(View(original, edited), MapDataKind::Units) == *nestedBytes.value, "嵌套redo应恢复全部native字节");

    MapDataEditSession undone(original);
    const auto first = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 1));
    Require(undone.ApplyPreparedPlacement(first).Succeeded(), "建立回调可撤销的原命令");
    const auto before = View(original, undone);
    const auto pending = Plan(original, before, Rotation(before, PlacementFile::Units, 1, 2));
    const auto undoDenied = undone.ApplyPreparedPlacement(pending, [&](const auto&, const auto&, const auto&) {
        Require(undone.Undo(), "validator内真实Undo应成功");
        return Status::Ok();
    });
    Require(HasCode(undoDenied, "MAP_EDIT_PREPUBLISH_STALE") && undone.Revision() == 2
        && !undone.CanUndo() && undone.CanRedo() && !undone.IsDirty()
        && undone.LastAppliedEdit().CommandIdentity() == 1, "外层拒绝应保留嵌套Undo与redo尾");
    RequireSameMembers(original, View(original, undone));
    Require(undone.Redo(), "被保留的redo应仍可执行");
    const auto restored = View(original, undone);
    const auto next = Plan(original, restored, Rotation(restored, PlacementFile::Units, 1, 2));
    Require(undone.ApplyPreparedPlacement(next).Succeeded() && undone.LastEdit().CommandIdentity() == 2,
        "外层过期候选不得消耗下一命令身份");
}

void TightenedWireAndGlobalRecordBudgetsRejectProjection() {
    const auto original = Original();
    const auto measured = MeasureClassicPlacementWire(*original.Units());
    Require(measured.Succeeded() && measured.value->byteLength == Bytes(original, MapDataKind::Units).size()
        && measured.value->placementCount == 3, "计量须吻合实际原 writer 长度与实例数");
    PlacementCommandLimits limit;
    limit.maximumWireBytes = measured.value->byteLength;
    Require(!PrepareMapDataPlacement(original, original, Copy(original, PlacementFile::Units, {1}), limit).Succeeded(),
        "只容纳原 native 的预算应在复制候选展开前拒绝追加");
    Require(PrepareMapDataPlacement(original, original, Rotation(original, PlacementFile::Units, 1, 2), limit).Succeeded(),
        "相同 wire 长度的 rotation 应能使用原成员预算");
    --limit.maximumWireBytes;
    Require(!PrepareMapDataPlacement(original, original, Rotation(original, PlacementFile::Units, 1, 2), limit).Succeeded(),
        "原来源已超预算时不得开始 typed 候选复制");
    limit = {};
    limit.maximumSources = 1;
    Require(!PrepareMapDataPlacement(original, original, Copy(original, PlacementFile::Units, {1, 2}), limit).Succeeded(),
        "显式收紧 source 数应在展开前生效");
    const std::array<std::uint32_t, 1> sourceRows{1};
    PlacementReadLimits readLimits;
    readLimits.maxNestedRecords = measured.value->nestedRecordCount;
    Require(MeasureClassicPlacementWire(*original.Units(), {}, readLimits).Succeeded(), "原表恰好位于全局 nested 预算");
    const auto nestedOver = MeasureClassicPlacementWire(*original.Units(), sourceRows, readLimits);
    Require(HasCode(nestedOver, "PLACEMENT_RECORD_LIMIT"), "每个 vector 合法但全局 nested 超限时必须拒绝");
    readLimits = {};
    readLimits.maxPlacements = 3;
    const auto doodads = MeasureClassicPlacementWire(*original.Doodads(), {}, readLimits);
    Require(doodads.Succeeded() && doodads.value->placementCount == 3
        && doodads.value->byteLength == Bytes(original, MapDataKind::Doodads).size(), "normal 加 special 应共同计入总数");
    Require(HasCode(MeasureClassicPlacementWire(*original.Doodads(), sourceRows, readLimits), "PLACEMENT_RECORD_LIMIT"),
        "追加普通行不能绕过 special 占用的全局实例预算");
    const auto copy = std::get<MapDataPlacementCopy>(Copy(original, PlacementFile::Units, {1}));
    const auto projected = MeasureClassicPlacementWire(*original.Units(), sourceRows);
    const auto plan = Plan(original, original, copy);
    Require(projected.Succeeded() && projected.value->byteLength == plan.AfterMemberBytes().size()
        && projected.value->placementCount == 4, "零 buffer 计量必须等于实际完整候选 native");
}

void OriginalReplayProvesWholeNativeAndCoalescesAbsoluteFacing() {
    const auto original = Original();
    MapDataEditSession unchanged(original);
    const auto noOp = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 0));
    const auto noOpStatus = unchanged.ApplyPreparedPlacement(noOp);
    Require(HasCode(noOpStatus, "MAP_PLACEMENT_NO_CHANGE") && unchanged.Revision() == 0
        && unchanged.LastAppliedEdit().CommandIdentity() == 0 && !unchanged.CanUndo() && !unchanged.CanRedo()
        && !unchanged.IsDirty(), "零差异rotation不得发布新命令或消耗修订身份");
    RequireSameMembers(original, View(original, unchanged));
    MapDataEditSession session(original);
    const auto first = Plan(original, original, Rotation(original, PlacementFile::Units, 1, 1));
    Require(first.OriginalReplayIntent() && session.ApplyPreparedPlacement(first).Succeeded(), "首个原来源 rotation 应可发布");
    auto current = View(original, session);
    auto frozen = FreezeOriginalMapDataPlacement(original, current, *first.OriginalReplayIntent());
    Require(frozen.Succeeded() && frozen.value->HasChanges() && frozen.value->Members().size() == 1
        && frozen.value->Members().front().data == Bytes(current, MapDataKind::Units), "原意图应精确重放完整 native");
    const auto second = Plan(original, current, Rotation(current, PlacementFile::Units, 1, 2));
    Require(second.OriginalReplayIntent().has_value(), "重复同原始目标应仍能生成 original-bound 意图");
    const auto& last = std::get<MapDataPlacementRotation>(*second.OriginalReplayIntent());
    Require(last.facingRadians == 2 && last.source.expectedMemberSha256 == Sha(original, MapDataKind::Units)
        && last.source.id == Source(original, PlacementFile::Units, 1).id, "保存意图应保留最后输入并绑定原 SHA 与身份");
    Require(session.ApplyPreparedPlacement(second).Succeeded(), "重复 absolute facing 应在当前 revision 发布");
    current = View(original, session);
    frozen = FreezeOriginalMapDataPlacement(original, current, *second.OriginalReplayIntent());
    Require(frozen.Succeeded() && frozen.value->Members().front().data == Bytes(current, MapDataKind::Units),
        "原单次 rotation 应精确表示同目标最后朝向");
    auto tampered = *current.Units();
    tampered.units[2].mana = 42;
    Require(session.EditUnits(tampered).Succeeded()
        && !FreezeOriginalMapDataPlacement(original, View(original, session), *second.OriginalReplayIntent()).Succeeded(),
        "目标以外 native 变化不得藏在单次 rotation 保存意图中");
    MapDataEditSession removed(original);
    Require(removed.ApplyPreparedPlacement(first).Succeeded() && removed.Remove(MapDataKind::Regions).Succeeded()
        && !FreezeOriginalMapDataPlacement(original, View(original, removed), *first.OriginalReplayIntent()).Succeeded(),
        "whole original 重放必须核对其他成员 presence");
    MapDataEditSession restored(original);
    Require(restored.ApplyPreparedPlacement(first).Succeeded(), "建立待恢复的朝向");
    const auto changed = View(original, restored);
    const auto restore = Plan(original, changed, Rotation(changed, PlacementFile::Units, 1, 0));
    Require(restore.OriginalReplayIntent() && restored.ApplyPreparedPlacement(restore).Succeeded() && !restored.IsDirty(),
        "恢复原朝向必须真正恢复原 native");
    frozen = FreezeOriginalMapDataPlacement(original, View(original, restored), *restore.OriginalReplayIntent());
    Require(frozen.Succeeded() && !frozen.value->HasChanges() && frozen.value->Members().empty(),
        "零净变化应生成空 freeze 而不是虚构候选成功");
    const auto& restoredIntent = std::get<MapDataPlacementRotation>(frozen.value->Intent());
    Require(restoredIntent.facingRadians == 0 && restoredIntent.source.expectedMemberSha256 == Sha(original, MapDataKind::Units)
        && restoredIntent.source.id == Source(original, PlacementFile::Units, 1).id,
        "清净freeze仍须保留最后请求float和可信原来源意图");
}

void GuiPriorAuthorChangesAndNewRowsDoNotRequireWholeOriginalFreeze() {
    const auto original = Original();
    MapDataEditSession author(original);
    auto regions = *original.Regions();
    regions.entries[0].name = "作者先前改名";
    auto units = *original.Units();
    units.units[1].owner = 6;
    Require(author.EditRegions(regions).Succeeded() && author.EditUnits(units).Succeeded(), "建立 GUI 原有作者历史");
    const auto current = View(original, author);
    const auto plan = Plan(original, current, Rotation(current, PlacementFile::Units, 1, 2));
    Require(author.ApplyPreparedPlacement(plan).Succeeded(), "GUI 应直接使用原会话命令而非 whole freeze 拒绝作者编辑");
    const auto after = View(original, author);
    Require(after.Regions()->entries[0].name == "作者先前改名" && after.Units()->units[1].owner == 6
        && plan.OriginalReplayIntent()
        && !FreezeOriginalMapDataPlacement(original, after, *plan.OriginalReplayIntent()).Succeeded(),
        "GUI 保留作者改动；独立单操作重放仍拒绝不完整表示");
    Require(author.Undo() && View(original, author).Units()->units[1].owner == 6
        && View(original, author).Regions()->entries[0].name == "作者先前改名", "一条 rotation undo 不得撤销先前作者编辑");
    UnitPlacement added;
    added.rawcode = "hrif";
    added.owner = 2;
    added.position = {11, 22, 33};
    added.scale = {1, 1, 1};
    added.random = RandomAnyUnit{4, 0};
    const auto newId = AddUnitPlacement(author, added);
    Require(newId.Succeeded() && newId.value->index == 3, "作者可通过原 API 新增当前行");
    auto newCurrent = View(original, author);
    const auto newRotation = Plan(original, newCurrent, Rotation(newCurrent, PlacementFile::Units, 3, 2));
    Require(!newRotation.OriginalReplayIntent() && author.ApplyPreparedPlacement(newRotation).Succeeded(),
        "新 current 行没有原 index 身份，但 GUI prepared rotation 仍须成功");
    newCurrent = View(original, author);
    const auto newCopy = Plan(original, newCurrent, Copy(newCurrent, PlacementFile::Units, {3}));
    Require(!newCopy.OriginalReplayIntent() && author.ApplyPreparedPlacement(newCopy).Succeeded()
        && View(original, author).Units()->units.size() == 5, "复制 GUI 新行不应伪造 original 意图或拒绝原完整保存路径");
    MapDataEditSession shifted(original);
    const auto deleted = Source(original, PlacementFile::Units, 1).id;
    Require(DeletePlacements(shifted, std::span(&deleted, 1)).Succeeded(), "作者可通过原 API 删除并改变行位置");
    const auto shiftedCurrent = View(original, shifted);
    const auto shiftedRotation = Plan(original, shiftedCurrent, Rotation(shiftedCurrent, PlacementFile::Units, 1, 2));
    Require(!shiftedRotation.OriginalReplayIntent() && shifted.ApplyPreparedPlacement(shiftedRotation).Succeeded(),
        "相同行号但不同 rawcode/creation 身份不得绑定错误原行");
}

void SkinLayoutsRemainReadonlyAndUnsafeSourcesStayPresent() {
    // MapDocument 按真实 W3I 来源选择版本化 widget 读取，SkinId 字节自身不授予新版来源。
    auto settings = MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(settings.Succeeded(), "SkinId 路由须从合法设置夹具建立版本来源");
    settings.value->formatVersion = 31;
    settings.value->gameVersion = {1, 32, 10, 20456};
    const auto versionedSettings = settings.value->Serialize();
    Require(versionedSettings.Succeeded(), "合法 v31 W3I 应能提供版本化读取来源");
    UnitPlacements units;
    UnitPlacement unit;
    unit.rawcode = "hfoo";
    unit.creationId = 77;
    unit.scale = {1, 1, 1};
    unit.random = RandomAnyUnit{4, 0};
    units.units.push_back(unit);
    const auto classic = units.Serialize();
    Require(classic.Succeeded(), "单行 classic fixture 应往返");
    auto skinBytes = *classic.value;
    const std::array<std::byte, 4> skin{std::byte{'H'}, std::byte{'f'}, std::byte{'o'}, std::byte{'o'}};
    skinBytes.insert(skinBytes.begin() + 52, skin.begin(), skin.end());
    const auto withoutVersion = MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3mapUnits.doo", skinBytes}});
    Require(!withoutVersion.Units() && withoutVersion.Member(MapDataKind::Units).state == MapDataState::Invalid
        && Bytes(withoutVersion, MapDataKind::Units) == skinBytes,
        "缺少新版来源证明时 SkinId 字节必须按经典入口拒绝并完整保留");
    const auto skinDocument = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3map.w3i", *versionedSettings.value}, {"war3mapUnits.doo", skinBytes}});
    Require(skinDocument.Settings() && skinDocument.Settings()->formatVersion == 31
        && skinDocument.Units() && skinDocument.Units()->recordLayout == PlacementRecordLayout::SkinId
        && skinDocument.Units()->units.size() == 1
        && skinDocument.Units()->units[0].skinId == "Hfoo"
        && skinDocument.Units()->SerializeOriginal() == skinBytes && !skinDocument.Units()->Serialize().Succeeded(),
        "实际 SkinId wire fixture 应仅按版本化读取布局成立");
    Require(HasCode(PrepareMapDataPlacement(skinDocument, skinDocument,
            Rotation(skinDocument, PlacementFile::Units, 0, 2)), "MAP_PLACEMENT_LAYOUT_READ_ONLY")
        && HasCode(PrepareMapDataPlacement(skinDocument, skinDocument,
            Copy(skinDocument, PlacementFile::Units, {0})), "MAP_PLACEMENT_LAYOUT_READ_ONLY"),
        "可读取 SkinId 布局不能被当作 classic 可写来源");
    DoodadPlacements doodads;
    DoodadPlacement doodad;
    doodad.rawcode = "LTlt";
    doodad.creationId = 88;
    doodad.flags = 2;
    doodad.scale = {1, 1, 1};
    doodads.doodads.push_back(doodad);
    const auto classicDoodads = doodads.Serialize();
    Require(classicDoodads.Succeeded(), "单行 classic 地物 fixture 应往返");
    auto skinDoodadBytes = *classicDoodads.value;
    skinDoodadBytes.insert(skinDoodadBytes.begin() + 52, skin.begin(), skin.end());
    const auto skinDoodadDocument = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3map.w3i", *versionedSettings.value}, {"war3map.doo", skinDoodadBytes}});
    Require(skinDoodadDocument.Doodads() && skinDoodadDocument.Doodads()->recordLayout == PlacementRecordLayout::SkinId
        && skinDoodadDocument.Doodads()->doodads.size() == 1
        && skinDoodadDocument.Doodads()->doodads[0].skinId == "Hfoo"
        && skinDoodadDocument.Doodads()->SerializeOriginal() == skinDoodadBytes
        && !skinDoodadDocument.Doodads()->Serialize().Succeeded()
        && HasCode(PrepareMapDataPlacement(skinDoodadDocument, skinDoodadDocument,
            Rotation(skinDoodadDocument, PlacementFile::Doodads, 0, 2)), "MAP_PLACEMENT_LAYOUT_READ_ONLY")
        && HasCode(PrepareMapDataPlacement(skinDoodadDocument, skinDoodadDocument,
            Copy(skinDoodadDocument, PlacementFile::Doodads, {0})), "MAP_PLACEMENT_LAYOUT_READ_ONLY"),
        "地物 SkinId wire 也必须保留只读");
    // LNI 设置拥有自己的解析语义；单位原来源大小写别名也不能在投影时丢失。
    auto lniMembers = Members();
    const auto lniText = LniMapSettingsFixtures::Bytes(LniMapSettingsFixtures::Complete());
    lniMembers[0] = {"table/w3i.ini", lniText};
    lniMembers[1].name = "WAR3MAPUNITS.DOO";
    const auto lniOriginal = MapDocument::FromMembers(lniMembers, "lni-settings-fixture");
    Require(lniOriginal.LniSettings() && lniOriginal.Settings()
        && lniOriginal.Settings()->formatVersion == 25
        && lniOriginal.Member(MapDataKind::Settings).state == MapDataState::Ready,
        "真实完整 LNI 设置应保有已解析经典版本来源");
    for (const bool copy : {false, true}) {
        const auto intent = copy ? Copy(lniOriginal, PlacementFile::Units, {1})
            : Rotation(lniOriginal, PlacementFile::Units, 1, 2);
        const auto lniPlan = Plan(lniOriginal, lniOriginal, intent);
        MapDataEditSession lniSession(lniOriginal);
        Require(lniSession.ApplyPreparedPlacement(lniPlan).Succeeded(), "LNI 来源应支持原会话经典单位旋转和复制");
        const auto lniAfter = View(lniOriginal, lniSession);
        Require(lniAfter.Units() && lniAfter.Units()->recordLayout == PlacementRecordLayout::Classic
            && lniAfter.Member(MapDataKind::Units).state == MapDataState::Ready
            && lniAfter.Member(MapDataKind::Units).sources.front().name == "WAR3MAPUNITS.DOO"
            && lniAfter.SourcePath() == lniOriginal.SourcePath()
            && lniAfter.LniSettings() && lniAfter.LniSettings()->originalBytes == lniText
            && lniAfter.Member(MapDataKind::Settings).memberName == "table/w3i.ini"
            && lniAfter.Member(MapDataKind::Settings).sources.front().name == "table/w3i.ini"
            && Bytes(lniAfter, MapDataKind::Settings) == lniText,
            "旋转和复制须保有经典单位、LNI 语义、原 path、来源别名和原文本");
        RequireSameMembers(lniOriginal, lniAfter, MapDataKind::Units);
        RequireSameMembers(lniPlan.After(), lniAfter);
        Require(lniPlan.OriginalReplayIntent().has_value(), "LNI 来源的原始单位意图应可冻结");
        const auto frozen = FreezeOriginalMapDataPlacement(lniOriginal, lniAfter, *lniPlan.OriginalReplayIntent());
        Require(frozen.Succeeded() && frozen.value->Members().size() == 1
            && frozen.value->Members().front().name == "war3mapUnits.doo"
            && frozen.value->Members().front().data == Bytes(lniAfter, MapDataKind::Units),
            "LNI 原重放须精确匹配全 native，并只输出规范单位成员");
    }
    auto members = Members();
    members.push_back({"WAR3MAPUNITS.DOO", members[1].data});
    const auto ambiguous = MapDocument::FromMembers(members);
    Require(ambiguous.Member(MapDataKind::Units).state == MapDataState::Ambiguous, "制造实际来源冲突");
    const auto doodadPlan = Plan(ambiguous, ambiguous, Rotation(ambiguous, PlacementFile::Doodads, 0, 2));
    RequireSameMembers(ambiguous, doodadPlan.After(), MapDataKind::Doodads);
    MapDataEditSession session(ambiguous);
    Require(session.ApplyPreparedPlacement(doodadPlan).Succeeded(), "无关来源冲突不得阻止有效普通地物原命令");
    RequireSameMembers(doodadPlan.After(), View(ambiguous, session));
}

} // namespace

int main() {
    try {
        RotationPreservesCompleteRecordsAndStartLocationSettings();
        CopyPreservesNestedRecordsRequestOrderAndOriginalReceipts();
        InvalidSourcesAndLateFailuresPublishNothing();
        ValidatorFailureKeepsRedoAndConsumesNoCommandIdentity();
        ReentrantValidatorKeepsNestedEditAndUndo();
        TightenedWireAndGlobalRecordBudgetsRejectProjection();
        OriginalReplayProvesWholeNativeAndCoalescesAbsoluteFacing();
        GuiPriorAuthorChangesAndNewRowsDoNotRequireWholeOriginalFreeze();
        SkinLayoutsRemainReadonlyAndUnsafeSourcesStayPresent();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "placement prepared editing tests passed\n";
    return 0;
}
