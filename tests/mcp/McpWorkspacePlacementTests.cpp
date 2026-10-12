#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataPlacementPatchCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

static_assert(!std::is_constructible_v<MapDataPatch, std::vector<BinaryFilePatch>>,
    "placement patch 只能经来源绑定的私有 codec 构造");

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
void Succeeded(const T& result, const char* message) {
    if (result.Succeeded()) return;
    std::string detail(message);
    for (const auto& diagnostic : result.diagnostics) detail += " [" + diagnostic.code + "]";
    throw std::runtime_error(detail);
}

template<class T>
bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(stream), "写入自有临时 fixture 失败");
}

void Write(const std::filesystem::path& path, std::string_view text) {
    Write(path, std::as_bytes(std::span(text)));
}

Bytes Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream), "读取自有 fixture 失败");
    const std::vector<char> chars{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    Bytes bytes;
    bytes.reserve(chars.size());
    for (const auto value : chars) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    return bytes;
}

const Bytes& Native(const MapDocument& document, MapDataKind kind = MapDataKind::Units) {
    const auto& member = document.Member(kind);
    Require(member.sources.size() == 1, "要求唯一真实 native 来源");
    return member.sources.front().data;
}

bool SameBytes(std::span<const std::byte> left, std::span<const std::byte> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

void SameMembers(const MapDocument& expected, const MapDocument& actual,
    std::optional<MapDataKind> changed = {}) {
    for (const auto& member : expected.Members()) {
        if (changed && member.kind == *changed) continue;
        const auto& observed = actual.Member(member.kind);
        Require(observed.state == member.state && observed.memberName == member.memberName
            && observed.sources.size() == member.sources.size() && observed.recordCount == member.recordCount
            && observed.diagnostics.size() == member.diagnostics.size(), "来源状态、presence 或记录数发生非授权变化");
        for (std::size_t index = 0; index < member.sources.size(); ++index) {
            if (observed.sources[index].name != member.sources[index].name)
                throw std::runtime_error("来源成员名发生变化: " + member.memberName + " expected="
                    + member.sources[index].name + " actual=" + observed.sources[index].name
                    + " bytes_equal=" + (SameBytes(observed.sources[index].data, member.sources[index].data) ? "true" : "false"));
            if (!SameBytes(observed.sources[index].data, member.sources[index].data)) {
                const auto before = Sha256(member.sources[index].data), after = Sha256(observed.sources[index].data);
                Succeeded(before, "未编辑来源原字节 SHA"); Succeeded(after, "未编辑来源当前字节 SHA");
                throw std::runtime_error("未编辑来源 native 字节发生变化: " + member.memberName
                    + " expected_sha=" + *before.value + " actual_sha=" + *after.value);
            }
        }
    }
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "固定 fixture 字节槽越界");
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
}

struct Transaction {
    TransactionId id;
    ProjectRevision base;
};

struct Observation {
    ProjectRevision revision;
    std::string cas;
    TransactionId transaction;
    Mcp::McpWorkspaceMapDataContext context;
};

struct Fixture {
    static constexpr std::string_view Defines = "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n";
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("audastra-placement-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<Mcp::McpWorkspace> workspace;
    UnitPlacements units;
    DoodadPlacements doodads;
    Bytes originalUnits, originalDoodads, originalRegions, originalWtg, originalWct;

    Fixture() {
        const auto ui = root / "ui";
        Write(ui / "config", "base\n");
        Write(ui / "base/define.txt", Defines);
        for (const auto* name : {"action.txt", "call.txt", "event.txt", "condition.txt"}) Write(ui / "base" / name, "");
        const auto catalog = Trigger::UiCatalog::Load(ui);
        Succeeded(catalog, "加载真实 fixture catalog");
        const auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
        Succeeded(empty, "构造真实空 WTG/WCT");
        const auto pair = Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value);
        Succeeded(pair, "序列化真实配对触发器");
        originalWtg = pair.value->wtg; originalWct = pair.value->wct;
        Write(root / "source/map/war3map.wtg", originalWtg);
        Write(root / "source/map/war3map.wct", originalWct);

        UnitPlacement unit;
        unit.rawcode = "hfoo"; unit.variation = 3; unit.creationId = 17; unit.owner = 5;
        unit.position = {100.25f, -200.5f, 3.75f}; unit.rotation = 0.75f; unit.scale = {1.5f, 0.75f, 2.f};
        unit.flags = 7; unit.unknownOwnerBytes = {0x12, 0xa4};
        unit.hitPoints = 73; unit.mana = 87; unit.droppedItemTable = 9;
        unit.itemSets = {{{{"I000", 85}, {"I003", 15}}}, {{{"I004", 100}}}};
        unit.inventory = {{2, "I001"}, {5, "I005"}};
        unit.abilities = {{"A000", true, 3}, {"A001", false, 2}};
        unit.random = RandomCustomTable{{{"hpea", 65}, {"hrif", 35}}};
        unit.gold = 83; unit.targetAcquisition = -1; unit.heroLevel = 4;
        unit.heroStrength = 7; unit.heroAgility = 8; unit.heroIntelligence = 9;
        unit.teamColor = 5; unit.waygateRegionId = 19;
        units.units.push_back(unit);
        unit.rawcode = "hpea"; unit.creationId = 41; unit.owner = 2;
        unit.position = {-128.f, 768.f, 16.f}; unit.rotation = 1.f;
        unit.itemSets.resize(1); unit.inventory.resize(1); unit.abilities.resize(1);
        unit.random = RandomAnyUnit{-1, 2};
        units.units.push_back(unit);
        const auto serialized = units.Serialize();
        Succeeded(serialized, "完整 nested classic units fixture");
        originalUnits = *serialized.value;
        Write(root / "source/map/war3mapUnits.doo", originalUnits);
        DoodadPlacement doodad;
        doodad.rawcode = "LTlt"; doodad.creationId = 5; doodad.position = {-50.f, 60.f, 8.f};
        doodad.scale = {1.25f, 0.5f, 3.f}; doodad.flags = 2; doodad.lifePercent = 37;
        doodad.droppedItemTable = 8; doodad.itemSets = {{{{"I002", 60}, {"I003", 40}}}};
        doodads.doodads.push_back(doodad);
        const auto doodadBytes = doodads.Serialize(); Succeeded(doodadBytes, "真实 nested classic doodad fixture");
        originalDoodads = *doodadBytes.value;
        Write(root / "source/map/war3map.doo", originalDoodads);
        RegionsDocument regions;
        regions.entries.push_back({-128.f, -64.f, 128.f, 64.f, "Original region", 31, {}, "", {12, 34, 56, 78}});
        const auto regionBytes = regions.Serialize(); Succeeded(regionBytes, "真实区域 fixture");
        originalRegions = *regionBytes.value;
        Write(root / "source/map/war3map.w3r", originalRegions);
        Write(root / "source/map/war3map.w3s", "bad");
        auto opened = Mcp::McpWorkspace::Open(root / "source", ui);
        Succeeded(opened, "打开真实 workspace"); workspace = std::move(*opened.value);
        const auto context = Context();
        Require(context.document->Units() && context.document->Units()->recordLayout == PlacementRecordLayout::Classic
            && context.document->Units()->units.size() == 2
            && context.document->Doodads() && context.document->Doodads()->recordLayout == PlacementRecordLayout::Classic
            && context.document->Doodads()->doodads.size() == 1
            && context.document->Member(MapDataKind::Sounds).state == MapDataState::Invalid,
            "fixture 必须为两条可编辑 classic unit 和保留的 invalid sounds");
        Require(context.document->Member(MapDataKind::Units).sources.front().name == "map/war3mapUnits.doo"
            && context.document->Member(MapDataKind::Doodads).sources.front().name == "map/war3map.doo"
            && context.document->Member(MapDataKind::Regions).sources.front().name == "map/war3map.w3r",
            "fixture 必须实际捕获 map/ 来源别名，不能以裸成员名绕过 directory alias 回归");
    }

    ~Fixture() {
        workspace.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error); // 仅固定自有临时夹具，不接收协议路径。
    }

    Mcp::McpWorkspaceMapDataContext Context() {
        auto value = workspace->MapDataContext("placement-owned", 1);
        Succeeded(value, "捕获当前真实 placement context"); return std::move(*value.value);
    }

    Observation Observe() { return {workspace->Revision(), workspace->ObjectRevision(), workspace->ActiveTransactionId(), Context()}; }

    void Unchanged(const Observation& before) {
        const auto after = Observe();
        Require(after.revision == before.revision && after.cas == before.cas && after.transaction == before.transaction
            && after.context.includesActiveTransactionOverlay == before.context.includesActiveTransactionOverlay
            && after.context.binding.documentId == before.context.binding.documentId
            && after.context.binding.generation == before.context.binding.generation
            && after.context.binding.revision == before.context.binding.revision
            && after.context.binding.sourceSha256 == before.context.binding.sourceSha256,
            "失败不得改变工程身份、opaque CAS、事务或查询 binding");
        SameMembers(*before.context.document, *after.context.document);
    }

    Transaction Begin() {
        const auto revision = workspace->Revision();
        const auto begun = workspace->BeginEdit(revision); Succeeded(begun, "显式 begin placement 事务");
        return {*begun.value, revision};
    }

    MapDataPlacementSource Source(const MapDocument& document, std::uint32_t row) {
        Require(document.Units() && row < document.Units()->units.size(), "来源必须是实际 unit 行");
        const auto key = MapFieldRowKey(document, MapDataKind::Units, row);
        const auto sha = Sha256(Native(document));
        Succeeded(key, "实际来源 row key"); Succeeded(sha, "实际来源 member SHA");
        return {MakePlacementId(PlacementFile::Units, row, document.Units()->units[row]), *key.value, *sha.value};
    }

    PreparedMapDataPlacement Placement(bool copying, std::uint32_t row = 0, float facing = 1.5f) {
        const auto context = Context();
        MapDataPlacementIntent intent = MapDataPlacementRotation{Source(*context.document, row), facing};
        if (copying) intent = MapDataPlacementCopy{{Source(*context.document, row)}, {64.f, -32.f, 4.f}};
        auto plan = PrepareMapDataPlacement(*context.original, *context.document, intent);
        Succeeded(plan, "生产 prepared placement 必须产生完整候选"); return std::move(*plan.value);
    }

    PreparedMapDataFields RegionField(const char* value = "-127") {
        const auto context = Context();
        const auto* field = FindMapField(MapDataKind::Regions, "left"); Require(field != nullptr, "真实字段目录");
        const auto old = ReadMapField(*context.document, MapDataKind::Regions, 0, *field);
        const auto next = ParseMapFieldInput(*field, value);
        const auto key = MapFieldRowKey(*context.document, MapDataKind::Regions, 0);
        const auto sha = Sha256(Native(*context.document, MapDataKind::Regions));
        Succeeded(old, "区域原值"); Succeeded(next, "区域合法输入"); Succeeded(key, "区域定位"); Succeeded(sha, "区域 SHA");
        const std::array edits{MapFieldEdit{MapDataKind::Regions, 0, *key.value, "left", *old.value, *next.value}};
        auto plan = PrepareMapDataFields(*context.original, *context.document, *sha.value, edits);
        Succeeded(plan, "真实 C1 prepared fields"); return std::move(*plan.value);
    }

    PreparedMapDataPlacement DoodadCopy() {
        const auto context = Context();
        const auto key = MapFieldRowKey(*context.document, MapDataKind::Doodads, 0);
        const auto sha = Sha256(Native(*context.document, MapDataKind::Doodads));
        Succeeded(key, "真实 doodad 来源 row key"); Succeeded(sha, "真实 doodad 来源 SHA");
        const MapDataPlacementSource source{MakePlacementId(PlacementFile::Doodads, 0,
            context.document->Doodads()->doodads[0]), *key.value, *sha.value};
        auto plan = PrepareMapDataPlacement(*context.original, *context.document,
            MapDataPlacementCopy{{source}, {64.f, -32.f, 4.f}});
        Succeeded(plan, "生产 prepared doodad copy"); return std::move(*plan.value);
    }

    Bytes ExpectedCopy() const {
        // 单一固定 fixture 的独立字节预期：原两条记录保留，完整源记录 slice 追加。
        UnitPlacements single;
        single.units.push_back(units.units[0]);
        const auto serialized = single.Serialize(); Succeeded(serialized, "独立来源记录 slice");
        Bytes appended(serialized.value->begin() + 16, serialized.value->end());
        Put32(appended, 8, std::bit_cast<std::uint32_t>(164.25f));
        Put32(appended, 12, std::bit_cast<std::uint32_t>(-232.5f));
        Put32(appended, 16, std::bit_cast<std::uint32_t>(7.75f));
        Put32(appended, appended.size() - 4, 42);
        Bytes expected = originalUnits;
        Put32(expected, 12, 3);
        expected.insert(expected.end(), appended.begin(), appended.end());
        return expected;
    }

    Bytes ExpectedRotation(float facing) const {
        Bytes expected = originalUnits;
        Put32(expected, 16 + 20, std::bit_cast<std::uint32_t>(facing));
        return expected;
    }

    Bytes ExpectedDoodadCopy() const {
        auto expected = doodads;
        auto appended = doodads.doodads[0];
        appended.position = {14.f, 28.f, 12.f}; appended.creationId = 6;
        expected.doodads.push_back(std::move(appended));
        const auto serialized = expected.Serialize(); Succeeded(serialized, "独立完整 doodad 预期");
        return *serialized.value;
    }

    void SourceUntouched() const {
        Require(Read(root / "source/map/war3mapUnits.doo") == originalUnits
            && Read(root / "source/map/war3map.doo") == originalDoodads
            && Read(root / "source/map/war3map.w3r") == originalRegions
            && Read(root / "source/map/war3map.wtg") == originalWtg
            && Read(root / "source/map/war3map.wct") == originalWct
            && Read(root / "source/map/war3map.w3s") == Bytes{std::byte{'b'}, std::byte{'a'}, std::byte{'d'}},
            "workspace 操作不得回写自有来源夹具");
    }
};

template<class T>
void RefusedUnchanged(Fixture& fixture, const Observation& before, const T& result, std::string_view code) {
    Require(!result.Succeeded() && HasCode(result, code), "拒绝必须保有具体领域诊断，不能换成其它失败");
    fixture.Unchanged(before);
}

void NoImplicitBeginPrepublishCopyAndRollback() {
    Fixture f;
    const auto original = f.Context();
    const auto unopened = f.Observe();
    RefusedUnchanged(f, unopened, f.workspace->ApplyPlacementPlan(f.Placement(true)), "MCP_TRANSACTION_NOT_ACTIVE");
    Require(!f.workspace->HasActiveTransaction(), "不允许隐式 begin");
    const auto transaction = f.Begin();
    const auto before = f.Observe();
    const auto plan = f.Placement(true);
    bool callback = false;
    const auto refused = f.workspace->ApplyPlacementPlan(plan, [&](const auto& old, const auto& next, const auto& receipt) {
        callback = true;
        Require(Native(old) == f.originalUnits && Native(next) == f.ExpectedCopy()
            && receipt.SessionIdentity() && receipt.CommandIdentity()
            && receipt.AfterRevision() == receipt.BeforeRevision() + 1, "发布门禁须看到完整候选和真实单命令回执");
        return Status::Failure("Test", "PLACEMENT_RECEIPT_REJECTED", "完整回执在发布前拒绝");
    });
    Require(callback, "必须真正执行 prepublish 门禁");
    RefusedUnchanged(f, before, refused, "PLACEMENT_RECEIPT_REJECTED");
    auto preview = f.workspace->PreviewEdit(transaction.id, transaction.base);
    Succeeded(preview, "拒绝后原事务仍可 preview"); Require(preview.value->changes.empty(), "拒绝不得留下任何 overlay");
    const auto copied = f.workspace->ApplyPlacementPlan(plan); Succeeded(copied, "实际单位复制发布");
    const auto current = f.Context();
    Require(Native(*current.document) == f.ExpectedCopy() && current.document->Units()->units.size() == 3
        && current.document->Units()->units[2].creationId == 42 && current.document->Units()->units[2].rawcode == "hfoo"
        && current.document->Units()->units[2].position == std::array<float, 3>{164.25f, -232.5f, 7.75f}
        && plan.NewIds().size() == 1 && plan.NewIds()[0].index == 2 && plan.NewIds()[0].creationId == 42,
        "复制完整 native、追加索引、rawcode 和创建编号必须精确符合原记录 slice");
    Require(f.workspace->ObjectRevision() != before.cas && f.workspace->Revision() == transaction.base
        && f.workspace->ActiveTransactionId() == transaction.id && copied.value->AfterRevision() == copied.value->BeforeRevision() + 1,
        "成功仅推进原活动修订，保留事务身份和 base revision");
    Require(current.document->Member(MapDataKind::Units).sources.front().name
        == original.document->Member(MapDataKind::Units).sources.front().name,
        "typed copy 必须保持原目录来源名，不能把 map/ 别名改成裸成员名");
    Require(plan.OriginalReplayIntent().has_value(), "单位 copy freeze 需要实际 original-bound intent");
    const auto frozen = FreezeOriginalMapDataPlacement(*current.original, *current.document, *plan.OriginalReplayIntent());
    Succeeded(frozen, "带 map/ 来源别名的单位 copy freeze");
    Require(frozen.value->Members().size() == 1 && frozen.value->Members()[0].name == "war3mapUnits.doo"
        && frozen.value->Members()[0].data == f.ExpectedCopy(),
        "冻结写出必须为 canonical 单位 native 名与完整独立预期，不可把 map/ 别名写入归档");
    SameMembers(*original.document, *current.document, MapDataKind::Units);
    const auto stable = f.Observe();
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(true)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(false)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, stable, f.workspace->ApplyMapDataPlan(f.RegionField()), "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED");
    auto newRow = f.Placement(true, 2);
    Require(!newRow.OriginalReplayIntent(), "真实新行不能伪装原来源定位");
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(newRow), "MCP_PLACEMENT_ORIGINAL_SOURCE_REQUIRED");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "原事务 rollback");
    SameMembers(*original.document, *f.Context().document);
    Require(!f.workspace->HasActiveTransaction(), "rollback 必须关闭原活动事务"); f.SourceUntouched();
}

void CreateDeleteUseOriginalSealedTransaction() {
    for (const bool creating : {false, true}) {
        Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
        MapDataPlacementIntent intent = MapDataPlacementDelete{{f.Source(*original.document, 0)}};
        if (creating) {
            auto unit = original.document->Units()->units.front(); unit.rawcode = "H001"; unit.position = {32, 48, 16};
            const auto sha = Sha256(Native(*original.document)); Succeeded(sha, "新增成员绑定 SHA");
            intent = MapDataPlacementCreate{*sha.value, std::vector<UnitPlacement>{unit}};
        }
        auto plan = PrepareMapDataPlacement(*original.original, *original.document, intent);
        Succeeded(plan, "实际新增/删除密封计划");
        const auto before = f.Observe();
        RefusedUnchanged(f, before, f.workspace->ApplyPlacementPlan(*plan.value, [](const auto&, const auto&, const auto&) {
            return Status::Failure("Test", "PLACEMENT_RECEIPT_REJECTED", "新建/删除完整回执拒绝");
        }), "PLACEMENT_RECEIPT_REJECTED");
        Succeeded(f.workspace->ApplyPlacementPlan(*plan.value), "原事务应用新增/删除");
        SameMembers(plan.value->After(), *f.Context().document);
        const auto current = f.Context();
        Require(current.document->Units()->units.size() == (creating ? 3U : 1U), "新增/删除记录数量错误");
        const auto frozen = FreezeOriginalMapDataPlacement(*current.original, *current.document, *plan.value->OriginalReplayIntent());
        Succeeded(frozen, "新增/删除必须可从唯一原意图重放");
        const auto stable = f.Observe();
        RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(true)), creating
            ? "MCP_PLACEMENT_COMBINATION_UNSUPPORTED" : "MCP_PLACEMENT_ORIGINAL_SOURCE_REQUIRED");
        Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "新增/删除仍走原 rollback");
        SameMembers(*original.document, *f.Context().document); f.SourceUntouched();
    }
}

void FieldsThenPlacementsRefuseBothCommands() {
    Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
    Succeeded(f.workspace->ApplyMapDataPlan(f.RegionField()), "实际 C1 区域净变化");
    const auto stable = f.Observe();
    Require(stable.context.document->Regions()->entries[0].left == -127.f, "混合前必须是真实成功 C1 字段变化");
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(true)), "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED");
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(false)), "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "C1 混合前置事务 rollback");
    SameMembers(*original.document, *f.Context().document); f.SourceUntouched();
}

void RepeatedAbsoluteRotationFreezesLastRequestedFloat() {
    Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
    const auto first = f.workspace->ApplyPlacementPlan(f.Placement(false, 0, 1.125f)); Succeeded(first, "第一绝对弧度发布");
    Require(Native(*f.Context().document) == f.ExpectedRotation(1.125f), "第一 rotation 只改原 facing float 槽");
    const auto stable = f.Observe(); const auto last = f.Placement(false, 0, 1.5f);
    const auto rejected = f.workspace->ApplyPlacementPlan(last, [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "ROTATION_RECEIPT_REJECTED", "已有旋转覆盖上的新回执拒绝");
    });
    RefusedUnchanged(f, stable, rejected, "ROTATION_RECEIPT_REJECTED");
    const auto second = f.workspace->ApplyPlacementPlan(last); Succeeded(second, "同原行第二次绝对弧度发布");
    const auto current = f.Context();
    Require(Native(*current.document) == f.ExpectedRotation(1.5f)
        && std::bit_cast<std::uint32_t>(current.document->Units()->units[0].rotation) == std::bit_cast<std::uint32_t>(1.5f)
        && (first.value->SessionIdentity() != second.value->SessionIdentity() || first.value->CommandIdentity() != second.value->CommandIdentity()),
        "重复 rotation 必须保留最后请求 float，不累加、不伪造同一回执");
    Require(current.document->Member(MapDataKind::Units).sources.front().name
        == original.document->Member(MapDataKind::Units).sources.front().name,
        "typed rotation 必须保持原目录来源名");
    Require(last.OriginalReplayIntent().has_value(), "最后命令必须保有原来源 replay");
    const auto& intent = std::get<MapDataPlacementRotation>(*last.OriginalReplayIntent());
    Require(std::bit_cast<std::uint32_t>(intent.facingRadians) == std::bit_cast<std::uint32_t>(1.5f)
        && intent.source.id == f.Source(*current.original, 0).id
        && intent.source.expectedMemberSha256 == f.Source(*current.original, 0).expectedMemberSha256,
        "最终保存意图须绑定原行、原 member SHA 与最后输入 float");
    const auto frozen = FreezeOriginalMapDataPlacement(*current.original, *current.document, *last.OriginalReplayIntent());
    Succeeded(frozen, "原来源最终 rotation freeze");
    Require(frozen.value->Members().size() == 1 && frozen.value->Members()[0].name == "war3mapUnits.doo"
        && frozen.value->Members()[0].data == f.ExpectedRotation(1.5f),
        "冻结必须使用 canonical 单位 native 名与原 bytes 最终绝对旋转");
    SameMembers(*original.document, *current.document, MapDataKind::Units);
    const auto finalStable = f.Observe();
    RefusedUnchanged(f, finalStable, f.workspace->ApplyPlacementPlan(f.Placement(false, 1, 0.5f)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, finalStable, f.workspace->ApplyPlacementPlan(f.Placement(true)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, finalStable, f.workspace->ApplyMapDataPlan(f.RegionField()), "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "重复 rotation rollback");
    SameMembers(*original.document, *f.Context().document); f.SourceUntouched();
}

void RestoredRotationClearsRecipeAndAllowsFreshCopy() {
    Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
    Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(false, 0, 1.5f)), "先产生真实旋转净差");
    const auto restore = f.Placement(false, 0, 0.75f);
    Succeeded(f.workspace->ApplyPlacementPlan(restore), "显式恢复原 facing");
    f.SourceUntouched();
    const auto restored = f.Context(); SameMembers(*original.document, *restored.document);
    const auto freeze = FreezeOriginalMapDataPlacement(*restored.original, *restored.document, *restore.OriginalReplayIntent());
    Succeeded(freeze, "恢复原值仍须通过 original replay"); Require(!freeze.value->HasChanges(), "恢复原 native 必须无净差");
    Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(true)), "净差恢复后允许新 copy 意图");
    Require(Native(*f.Context().document) == f.ExpectedCopy(), "旧 rotation bytes 不得残留在新 copy overlay");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "恢复后 copy rollback"); f.SourceUntouched();
}

void OriginalByteNoopOverlayStillValidatesSinglePlacement(const Fixture& fixture,
    const SemanticDiff& preview, std::string_view noopName, const Bytes& originalNoop,
    std::string_view changedName, const Bytes& originalChanged, const Bytes& expectedChanged) {
    Require(preview.changes.size() == 2, "必须保有恰好两项真实 map-data overlay，不能丢弃原字节净零项");
    const auto noopSha = Sha256(originalNoop), oldSha = Sha256(originalChanged), newSha = Sha256(expectedChanged);
    Succeeded(noopSha, "净零来源 SHA"); Succeeded(oldSha, "placement 原 SHA"); Succeeded(newSha, "独立最终 SHA");
    bool noop = false, changed = false;
    for (const auto& item : preview.changes) {
        Require(item.kind == "map-data", "净零回归不得引入其它命令类型");
        const auto name = item.path.filename().generic_string();
        if (name == noopName) {
            Require(!noop && item.beforeSha256 == *noopSha.value && item.afterSha256 == *noopSha.value,
                "净零 overlay 须唯一且前后 SHA 均精确原来源");
            noop = true;
        } else if (name == changedName) {
            Require(!changed && item.beforeSha256 == *oldSha.value && item.afterSha256 == *newSha.value,
                "唯一 placement overlay 必须原来源至独立预期 native");
            changed = true;
        } else Require(false, "不得出现任意额外 native overlay");
    }
    Require(noop && changed && fixture.workspace->HasActiveTransaction(), "两项 overlay 和原事务必须完整保留");
}

void RestoredRegionOverlayAllowsUnitCopyOnlyFreeze() {
    Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
    Succeeded(f.workspace->ApplyMapDataPlan(f.RegionField("-127")), "真实 region left+1");
    Succeeded(f.workspace->ApplyMapDataPlan(f.RegionField("-128")), "真实 region left 恢复原值");
    f.SourceUntouched();
    SameMembers(*original.document, *f.Context().document);
    const auto copy = f.Placement(true);
    Succeeded(f.workspace->ApplyPlacementPlan(copy), "保留原字节 W3R overlay 后发布 unit copy");
    const auto current = f.Context();
    Require(Native(*current.document) == f.ExpectedCopy(), "净零 W3R 不得改变完整单位 copy 预期");
    SameMembers(*original.document, *current.document, MapDataKind::Units);
    const auto preview = f.workspace->PreviewEdit(transaction.id, transaction.base); Succeeded(preview, "净零 W3R + unit copy preview");
    OriginalByteNoopOverlayStillValidatesSinglePlacement(f, *preview.value, "war3map.w3r", f.originalRegions,
        "war3mapUnits.doo", f.originalUnits, f.ExpectedCopy());
    Require(f.workspace->ValidateEdit(transaction.id, transaction.base).satisfied, "原 SHA 净零 overlay 不得阻塞真实 validation");
    Require(copy.OriginalReplayIntent().has_value(), "copy-only freeze 必须实际 original-bound intent");
    const auto frozen = FreezeOriginalMapDataPlacement(*current.original, *current.document, *copy.OriginalReplayIntent());
    Succeeded(frozen, "恢复区域后的单一 placement freeze");
    Require(frozen.value->Members().size() == 1 && frozen.value->Members()[0].name == "war3mapUnits.doo"
        && frozen.value->Members()[0].data == f.ExpectedCopy(), "最终 freeze 只包含有净差的完整单位 native");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "净零区域 + copy rollback");
    SameMembers(*original.document, *f.Context().document); f.SourceUntouched();
}

void RestoredUnitRotationAllowsDoodadCopyOnlyFreeze() {
    Fixture f; const auto original = f.Context(); const auto transaction = f.Begin();
    Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(false, 0, 1.5f)), "跨类别前真实 unit rotation");
    Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(false, 0, 0.75f)), "跨类别前恢复原 unit facing");
    f.SourceUntouched();
    SameMembers(*original.document, *f.Context().document);
    const auto copy = f.DoodadCopy(); Succeeded(f.workspace->ApplyPlacementPlan(copy), "保有原字节单位 overlay 后发布 doodad copy");
    const auto current = f.Context();
    Require(Native(*current.document, MapDataKind::Doodads) == f.ExpectedDoodadCopy(), "独立完整 doodad copy native");
    Require(current.document->Member(MapDataKind::Doodads).sources.front().name
        == original.document->Member(MapDataKind::Doodads).sources.front().name,
        "typed doodad copy 必须保持 map/ 原来源名，冻结写出另用 canonical 名");
    SameMembers(*original.document, *current.document, MapDataKind::Doodads);
    const auto preview = f.workspace->PreviewEdit(transaction.id, transaction.base); Succeeded(preview, "净零 unit + doodad copy preview");
    OriginalByteNoopOverlayStillValidatesSinglePlacement(f, *preview.value, "war3mapUnits.doo", f.originalUnits,
        "war3map.doo", f.originalDoodads, f.ExpectedDoodadCopy());
    Require(f.workspace->ValidateEdit(transaction.id, transaction.base).satisfied, "原 unit SHA 净零 overlay 不得阻塞 doodad validation");
    Require(copy.OriginalReplayIntent().has_value(), "doodad freeze 必须实际 original-bound intent");
    const auto frozen = FreezeOriginalMapDataPlacement(*current.original, *current.document, *copy.OriginalReplayIntent());
    Succeeded(frozen, "恢复单位后的 doodad-only placement freeze");
    Require(frozen.value->Members().size() == 1 && frozen.value->Members()[0].name == "war3map.doo"
        && frozen.value->Members()[0].data == f.ExpectedDoodadCopy(), "最终 freeze 只含完整 doodad native，原单位零差不漏入");
    Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "跨类别净零 placement rollback");
    SameMembers(*original.document, *f.Context().document); f.SourceUntouched();
}

void DirectoryExportReseedsCopyAndKeepsPolicy() {
    Fixture f; const auto transaction = f.Begin();
    Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(true)), "目录出口前真实 copy");
    auto preview = f.workspace->PreviewEdit(transaction.id, transaction.base); Succeeded(preview, "copy 原 preview");
    Require(preview.value->changes.size() == 1 && preview.value->changes[0].kind == "map-data", "目录出口须为唯一 native patch");
    const auto validation = f.workspace->ValidateEdit(transaction.id, transaction.base); Require(validation.satisfied, "copy 原 validation");
    Succeeded(f.workspace->ExportCandidate(transaction.id, transaction.base, f.root / "output"), "原 DirectoryExport");
    Require(!f.workspace->HasActiveTransaction() && Read(f.root / "output/map/war3mapUnits.doo") == f.ExpectedCopy(), "导出真实全 native 且关闭原事务");
    Require(Read(f.root / "output/map/war3map.wtg") == f.originalWtg && Read(f.root / "output/map/war3map.wct") == f.originalWct
        && Read(f.root / "output/map/war3map.doo") == f.originalDoodads
        && Read(f.root / "output/map/war3map.w3r") == f.originalRegions && Read(f.root / "output/map/war3map.w3s") == Read(f.root / "source/map/war3map.w3s"),
        "目录候选保留触发器配对、区域及 invalid sounds 原字节");
    const auto committed = f.Observe(); const auto next = f.Begin();
    auto seeded = f.workspace->PreviewEdit(next.id, next.base); Succeeded(seeded, "下一事务 seeded preview");
    Require(seeded.value->changes.size() == 1 && seeded.value->changes[0].kind == "map-data"
        && Native(*f.Context().document) == f.ExpectedCopy(), "下一 Begin 必须 reseed 已提交完整 copy native");
    const auto stable = f.Observe();
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(true)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, stable, f.workspace->ApplyPlacementPlan(f.Placement(false)), "MCP_PLACEMENT_COMBINATION_UNSUPPORTED");
    RefusedUnchanged(f, stable, f.workspace->ApplyMapDataPlan(f.RegionField()), "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED");
    Succeeded(f.workspace->Rollback(next.id, next.base), "已提交 copy 的下一事务 rollback");
    SameMembers(*committed.context.document, *f.Context().document); f.SourceUntouched();
}

void SourceAndCatalogDriftRefuseBeforePublishAndExport() {
    for (const bool catalog : {false, true}) {
        Fixture f; const auto transaction = f.Begin();
        Succeeded(f.workspace->ApplyPlacementPlan(f.Placement(true)), "漂移前真实 copy 覆盖");
        const auto pending = f.Placement(false);
        Succeeded(f.workspace->PreviewEdit(transaction.id, transaction.base), "漂移前 preview");
        Require(f.workspace->ValidateEdit(transaction.id, transaction.base).satisfied, "漂移前 validation");
        const auto stable = f.Observe();
        if (catalog) Write(f.root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,ChangedInteger,integer\n[TriggerParams]\n");
        else {
            auto changed = f.originalUnits; changed[24] ^= std::byte{1};
            Write(f.root / "source/map/war3mapUnits.doo", changed);
        }
        const auto expected = f.workspace->EnvironmentStatus();
        const char* code = catalog ? "MCP_UI_CATALOG_CHANGED" : "SOURCE_CHANGED";
        Require(!expected.Succeeded() && HasCode(expected, code), "实际 EnvironmentStatus 必须识别真实来源或 catalog 漂移");
        const auto query = f.workspace->MapDataContext("placement-owned", 1);
        const auto apply = f.workspace->ApplyPlacementPlan(pending);
        const auto exported = f.workspace->ExportCandidate(transaction.id, transaction.base, f.root / "drift-output");
        Require(!query.Succeeded() && HasCode(query, code) && !apply.Succeeded() && HasCode(apply, code)
            && !exported.Succeeded() && HasCode(exported, code) && !std::filesystem::exists(f.root / "drift-output")
            && f.workspace->ObjectRevision() == stable.cas, "查询、发布和最终目录导出必须传播同一环境拒绝，不留下候选或推进 CAS");
        if (catalog) Write(f.root / "ui/base/define.txt", Fixture::Defines);
        else Write(f.root / "source/map/war3mapUnits.doo", f.originalUnits);
        f.Unchanged(stable);
        Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "漂移恢复后原 rollback"); f.SourceUntouched();
    }
}

void PrepublishLateDriftIsAtomic() {
    for (const bool catalog : {false, true}) {
        Fixture f; const auto transaction = f.Begin(); const auto stable = f.Observe();
        bool reached = false;
        const auto result = f.workspace->ApplyPlacementPlan(f.Placement(true), [&](const auto&, const auto&, const auto&) {
            reached = true;
            if (catalog) Write(f.root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,LateInteger,integer\n[TriggerParams]\n");
            else {
                auto changed = f.originalUnits; changed[24] ^= std::byte{1};
                Write(f.root / "source/map/war3mapUnits.doo", changed);
            }
            return Status::Ok();
        });
        Require(reached && !result.Succeeded() && HasCode(result, catalog ? "MCP_UI_CATALOG_CHANGED" : "SOURCE_CHANGED")
            && f.workspace->ObjectRevision() == stable.cas, "回执通过后发生漂移仍须在原 overlay 发布前拒绝");
        if (catalog) Write(f.root / "ui/base/define.txt", Fixture::Defines);
        else Write(f.root / "source/map/war3mapUnits.doo", f.originalUnits);
        f.Unchanged(stable);
        auto preview = f.workspace->PreviewEdit(transaction.id, transaction.base); Succeeded(preview, "晚漂移拒绝后的原 preview");
        Require(preview.value->changes.empty(), "晚漂移不得留下候选字节");
        Succeeded(f.workspace->Rollback(transaction.id, transaction.base), "晚漂移恢复后 rollback"); f.SourceUntouched();
    }
}

void SealedPlacementCodecBindsExactSnapshot() {
    Fixture f; const auto context = f.Context(); const auto plan = f.Placement(true);
    Require(plan.OriginalReplayIntent().has_value(), "密封测试需要真实 original-bound prepared copy");
    const auto directory = DirectoryWorkspace::Open(f.root / "source"); Succeeded(directory, "真实 source snapshot");
    const auto snapshot = directory.value->Snapshot();
    const auto valid = MapDataPlacementPatchCodec::Seal(*context.original, plan.After(), snapshot, *plan.OriginalReplayIntent());
    Succeeded(valid, "精确 source snapshot 才能密封 private placement patch");
    const auto found = std::find_if(snapshot.files.begin(), snapshot.files.end(), [](const auto& file) {
        return file.relativePath.filename() == "war3mapUnits.doo";
    });
    Require(found != snapshot.files.end(), "snapshot 必须包含真实 unit 来源");
    const auto index = static_cast<std::size_t>(found - snapshot.files.begin());
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto forged = snapshot;
        if (mutation == 0) forged.files[index].sha256 = std::string(64, 'f');
        else if (mutation == 1) ++forged.files[index].size;
        else if (mutation == 2) forged.files.erase(forged.files.begin() + index);
        else {
            auto duplicate = forged.files[index]; duplicate.relativePath = "war3mapUnits.doo";
            forged.files.push_back(std::move(duplicate));
        }
        const auto refused = MapDataPlacementPatchCodec::Seal(*context.original, plan.After(), forged, *plan.OriginalReplayIntent());
        Require(!refused.Succeeded() && HasCode(refused, "MAP_FIELD_PATCH_SOURCE"), "SHA、大小、缺失和 alias 多源 snapshot 必须明确拒绝");
    }
    f.SourceUntouched();
}
} // namespace

int main() {
    const char* scenario = "fixture initialization";
    const auto run = [&](const char* name, void (*test)()) { scenario = name; test(); };
    try {
        run("NoImplicitBeginPrepublishCopyAndRollback", NoImplicitBeginPrepublishCopyAndRollback);
        run("CreateDeleteUseOriginalSealedTransaction", CreateDeleteUseOriginalSealedTransaction);
        run("FieldsThenPlacementsRefuseBothCommands", FieldsThenPlacementsRefuseBothCommands);
        run("RepeatedAbsoluteRotationFreezesLastRequestedFloat", RepeatedAbsoluteRotationFreezesLastRequestedFloat);
        run("RestoredRotationClearsRecipeAndAllowsFreshCopy", RestoredRotationClearsRecipeAndAllowsFreshCopy);
        run("RestoredRegionOverlayAllowsUnitCopyOnlyFreeze", RestoredRegionOverlayAllowsUnitCopyOnlyFreeze);
        run("RestoredUnitRotationAllowsDoodadCopyOnlyFreeze", RestoredUnitRotationAllowsDoodadCopyOnlyFreeze);
        run("DirectoryExportReseedsCopyAndKeepsPolicy", DirectoryExportReseedsCopyAndKeepsPolicy);
        run("SourceAndCatalogDriftRefuseBeforePublishAndExport", SourceAndCatalogDriftRefuseBeforePublishAndExport);
        run("PrepublishLateDriftIsAtomic", PrepublishLateDriftIsAtomic);
        run("SealedPlacementCodecBindsExactSnapshot", SealedPlacementCodecBindsExactSnapshot);
    } catch (const std::exception& error) {
        std::cerr << scenario << ": " << error.what() << '\n'; return 1;
    }
    std::cout << "Actual placement workspace copy, rotation, atomic refusal, replay, directory reseed and source gates passed.\n";
    return 0;
}
