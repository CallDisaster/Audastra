#include "McpPlacementTools.h"
#include "McpMapDataJson.h"
#include "War3MapCore/Hash.h"
#include "../MapDataFixtures.h"
#include "../PlacementCreationFixtures.h"

#include <QJsonDocument>

#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

void TestRegisteredPlacementHost();

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Mcp;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& result) {
    const auto diagnostics = result.value("diagnostics").toArray();
    return diagnostics.isEmpty() ? QString{} : diagnostics.first().toObject().value("code").toString();
}
std::vector<MapArchiveMember> Members() {
    UnitPlacements units;
    for (int i = 0; i < 3; ++i) {
        UnitPlacement unit; unit.rawcode = i == 2 ? "sloc" : "hfoo"; unit.position = {float(i * 16), 32, 64};
        unit.scale = {1, 1, 1}; unit.creationId = 10 + i; unit.random = RandomAnyUnit{2, 0};
        unit.inventory.push_back({1, "ratf"}); unit.abilities.push_back({"AHbz", true, 3});
        unit.itemSets.push_back({{{"phea", 40}, {"pman", 60}}}); units.units.push_back(std::move(unit));
    }
    DoodadPlacements doodads;
    for (int i = 0; i < 2; ++i) {
        DoodadPlacement doodad; doodad.rawcode = "LTlt"; doodad.position = {float(i), 20, 30};
        doodad.scale = {1, 2, 3}; doodad.creationId = 20 + i; doodad.itemSets.push_back({{{"phea", 100}}});
        doodads.doodads.push_back(std::move(doodad));
    }
    doodads.specialDoodads.push_back({"LTlt", 2, {5, 6}});
    RegionsDocument regions; RegionRecord region{}; region.left = -10; region.bottom = -10;
    region.right = 10; region.top = 10; region.name = "作者原区域"; region.creationNumber = 1; regions.entries.push_back(region);
    const auto ub = units.Serialize(), db = doodads.Serialize(), rb = regions.Serialize();
    Require(ub.Succeeded() && db.Succeeded() && rb.Succeeded(), "原创 placements 必须可按原 writer 往返");
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3mapUnits.doo", *ub.value},
        {"war3map.doo", *db.value}, {"war3map.w3r", *rb.value}};
}
struct Fixture {
    Tests::ObjectCommandsFixture objects;
    std::array<int, 7> objectLookups{};
    std::shared_ptr<const MapDocument> original, current;
    MapDataEditSession session;
    PlacementToolHost host;
    bool independent = false, active = true, readOnly = false, rejectPublish = false;
    int attempts = 0, validations = 0, publishes = 0;
    std::uint64_t epoch = 0;
    std::optional<MapDataPlacementIntent> pending;
    QJsonObject hostChanges;
    explicit Fixture(bool standalone = false)
        : original(std::make_shared<const MapDocument>(MapDocument::FromMembers(Members()))), current(original), session(*original), independent(standalone) {
        Tests::AddPlacementCustomObjects(objects);
        Require(session.AttachTerrain(Tests::PlacementTerrainBytes()).Succeeded(), "附加真实原创地形");
        host.objectContext = [&](ObjectData::ObjectType type) {
            ++objectLookups[static_cast<std::size_t>(type)];
            return Result<Services::ObjectCommandContext>::Success(objects.Context(type));
        };
        host.terrainPoint = [&](const TerrainStrokePoint& point) { return session.InspectTerrainPoint(point); };
        host.identity = [&] {
            QJsonObject identity{{"project_identity", "synthetic-placement-map"}, {"revision", QStringLiteral("placement:%1").arg(epoch)}};
            if (independent) {
                identity.insert("transaction_active", active); identity.insert("transaction_id", "71");
                identity.insert("transaction_revision", QJsonObject{{"generation", "0"}, {"source_sha256", QString(64, 'a')}, {"ui_catalog_sha256", QString(64, 'b')}});
            }
            return identity;
        };
        host.context = [&] {
            auto projected = ProjectMapData(*original, session);
            if (!projected.Succeeded()) return Result<MapDataToolContext>{{}, projected.diagnostics};
            current = std::make_shared<const MapDocument>(std::move(*projected.value));
            return Result<MapDataToolContext>::Success({original, current, {"placements", 1, epoch, std::string(64, 'a')}, false, independent && active, !independent});
        };
        host.apply = [&](const PreparedMapDataPlacement& plan, const PlacementPrepublishValidator& validate) -> Result<MapDataEditSession::EditReceipt> {
            ++attempts;
            if (readOnly) return Result<MapDataEditSession::EditReceipt>::Failure("Test", "HOST_READ_ONLY", "只读宿主。");
            if (independent) {
                if (!active) return Result<MapDataEditSession::EditReceipt>::Failure("Test", "PLACEMENT_ACTIVE_REQUIRED", "缺少独立活动事务。");
                if (!plan.OriginalReplayIntent()) return Result<MapDataEditSession::EditReceipt>::Failure("Test", "PLACEMENT_ORIGINAL_REQUIRED", "来源不在原表。");
                if (pending) {
                    const auto* old = std::get_if<MapDataPlacementRotation>(&*pending);
                    const auto* next = std::get_if<MapDataPlacementRotation>(&*plan.OriginalReplayIntent());
                    if (!old || !next || old->source.id != next->source.id)
                        return Result<MapDataEditSession::EditReceipt>::Failure("Test", "PLACEMENT_RECIPE_CONFLICT", "独立窄配方不允许第二copy或其他原始目标。");
                }
                const auto frozen = FreezeOriginalMapDataPlacement(*original, plan.After(), *plan.OriginalReplayIntent());
                if (!frozen.Succeeded()) return {{}, frozen.diagnostics};
            }
            auto after = plan.OwnedAfter();
            const auto status = session.ApplyPreparedPlacement(plan, [&](const auto& before, const auto& next, const auto& receipt) {
                ++validations;
                const auto accepted = validate(before, next, receipt, hostChanges);
                if (!accepted.Succeeded()) return accepted;
                return rejectPublish ? Status::Failure("Test", "HOST_PUBLICATION_DENIED", "密封覆盖发布前拒绝。") : Status::Ok();
            });
            if (!status.Succeeded()) return {{}, status.diagnostics};
            current = std::move(after); ++epoch; ++publishes;
            if (independent) {
                const auto frozen = FreezeOriginalMapDataPlacement(*original, *current, *plan.OriginalReplayIntent());
                if (frozen.value->HasChanges()) pending = plan.OriginalReplayIntent(); else pending.reset();
            }
            return Result<MapDataEditSession::EditReceipt>::Success(session.LastEdit());
        };
    }
    QJsonObject Locator(MapDataKind kind, std::size_t index) {
        const auto key = MapFieldRowKey(*current, kind, index); Require(key.Succeeded(), "查询实际 row key");
        return {{"source_index", QString::number(index)}, {"row_key", QString::fromStdString(*key.value)}};
    }
    QJsonObject Base(MapDataKind kind) {
        const auto captured = host.context(); Require(captured.Succeeded(), "获取当前可信上下文");
        const auto sha = Sha256(current->Member(kind).sources.front().data); Require(sha.Succeeded(), "获取实际 member SHA");
        const auto identity = host.identity();
        return {{"project_identity", identity.value("project_identity")}, {"expected_revision", identity.value("revision")},
            {"kind", MapDataJson::MapKindName(kind)}, {"expected_member_sha256", QString::fromStdString(*sha.value)}};
    }
    QJsonObject Rotate(MapDataKind kind, std::size_t index, double facing) {
        auto request = Base(kind); request.insert("source", Locator(kind, index)); request.insert("facing_radians", facing); return request;
    }
    QJsonObject Copy(MapDataKind kind, std::initializer_list<std::size_t> rows) {
        auto request = Base(kind); QJsonArray sources; for (const auto row : rows) sources.append(Locator(kind, row));
        request.insert("sources", sources); request.insert("offset", QJsonObject{{"x", 16}, {"y", -8}, {"z", 4}}); return request;
    }
    QJsonObject Create(MapDataKind kind, const QJsonArray& records) {
        auto request = Base(kind); request.insert("placements", records); return request;
    }
    QJsonObject Delete(MapDataKind kind, std::initializer_list<std::size_t> rows) {
        auto request = Base(kind); QJsonArray sources;
        for (const auto row : rows) sources.append(Locator(kind, row));
        request.insert("sources", sources); return request;
    }
};
QJsonObject NewRecord(const char* kind, const char* id) {
    return {{"object_kind", kind}, {"object_id", id}, {"x", 64}, {"y", -128}};
}
void SchemaAndCopyReceiptUseOriginalRowsAndOrder() {
    const auto definitions = PlacementToolDefinitions();
    Require(definitions.size() == 4 && definitions[0].name == "entities_rotate" && definitions[1].name == "entities_copy"
        && definitions[2].name == "entities_create" && definitions[3].name == "entities_delete", "发现四个共用摆放工具");
    for (const auto& definition : definitions) Require(definition.permission == ToolPermission::Edit
        && definition.maxRequestIdBytes == 4096 && !definition.inputSchema.value("properties").toObject().contains("member_bytes")
        && definition.inputSchema.value("properties").toObject().value("dry_run").toObject().value("type") == "boolean",
        "摆放工具沿原编辑权限与请求ID预算，提供预览，不公开rawbytes");
    Fixture f; const auto request = f.Copy(MapDataKind::Units, {1, 0});
    const auto result = InvokePlacementTool("entities_copy", request, f.host);
    Require(result.value("ok").toBool() && f.publishes == 1 && f.session.Revision() == 1 && f.validations == 1, "复制应发布一个原session命令");
    const auto before = result.value("before").toObject().value("rows").toArray(), after = result.value("after").toObject().value("rows").toArray();
    Require(before.size() == 2 && after.size() == 2 && before[0].toObject().value("source_index") == "1"
        && before[1].toObject().value("source_index") == "0" && after[0].toObject().value("source_index") == "3"
        && after[1].toObject().value("source_index") == "4", "before/after与NewIds必须保持输入顺序");
    Require(f.current->Units()->units[3].inventory.size() == 1 && f.current->Units()->units[3].abilities.front().autocast
        && f.current->Units()->units[3].itemSets.front().items.size() == 2, "copy须保留全部嵌套字段");
    const auto items = result.value("changes").toObject().value("items").toArray();
    Require(items.size() == 2 && items[0].toObject().value("object_id").toString().isEmpty()
        && items[0].toObject().value("before") == before[0] && items[0].toObject().value("after") == after[0], "完整changes只定位member/row，不生成未交付导航");
    Require(result.value("new_ids").toArray().size() == 2 && !result.value("saved").toBool()
        && QJsonDocument(result).toJson(QJsonDocument::Compact).size() <= 256 * 1024, "新增定位与完整预算应在发布前确定");
    const auto changed = f.session.Replacements();
    Require(f.session.Undo() && !f.session.IsDirty() && f.session.Redo()
        && f.session.Replacements().front().data == changed.front().data, "facade复制沿原UndoRedo");
}
void RotationUnitsAndSpecialTailRemainExact() {
    Fixture f; const auto special = f.original->Doodads()->SerializeOriginal();
    const auto result = InvokePlacementTool("entities_rotate", f.Rotate(MapDataKind::Doodads, 0, 1.5), f.host);
    Require(result.value("ok").toBool() && result.value("rotation_unit") == "radians"
        && result.value("field_display_angle_unit") == "degrees" && result.value("new_ids").toArray().isEmpty(), "绝对朝向请求与字段显示单位分开");
    auto restored = *f.current->Doodads(); restored.doodads[0].rotation = f.original->Doodads()->doodads[0].rotation;
    Require(restored.Serialize().Succeeded() && *restored.Serialize().value == special, "rotation只修改目标朝向，特殊尾与其余字段原样");
    Fixture start; const auto settings = start.original->Member(MapDataKind::Settings).sources.front().data;
    Require(InvokePlacementTool("entities_rotate", start.Rotate(MapDataKind::Units, 2, 2.0), start.host).value("ok").toBool()
        && start.current->Member(MapDataKind::Settings).sources.front().data == settings, "sloc仅朝向不联动W3I");
}
void InvalidLateSourceAndStaleRequestsNeverPublish() {
    Fixture f; const auto original = f.original->Member(MapDataKind::Units).sources.front().data;
    auto stale = f.Copy(MapDataKind::Units, {0}); stale["expected_revision"] = "old";
    Require(Code(InvokePlacementTool("entities_copy", stale, f.host)) == "ENTITY_REVISION_CONFLICT", "陈旧修订拒绝");
    auto wrongSha = f.Copy(MapDataKind::Units, {0}); wrongSha["expected_member_sha256"] = QString(64, '0');
    Require(Code(InvokePlacementTool("entities_copy", wrongSha, f.host)) == "MAP_PLACEMENT_MEMBER_STALE", "实际memberSHA拒绝");
    auto wrongRow = f.Copy(MapDataKind::Units, {0, 1}); auto rows = wrongRow.value("sources").toArray(); auto late = rows[1].toObject(); late["row_key"] = "stale"; rows[1] = late; wrongRow["sources"] = rows;
    Require(Code(InvokePlacementTool("entities_copy", wrongRow, f.host)) == "MAP_PLACEMENT_ROW_STALE", "合法前缀加陈旧尾拒绝整批");
    Require(Code(InvokePlacementTool("entities_copy", f.Copy(MapDataKind::Units, {0, 0}), f.host)) == "PLACEMENT_DUPLICATE", "重复来源拒绝");
    Require(Code(InvokePlacementTool("entities_copy", f.Copy(MapDataKind::Units, {0, 2}), f.host)) == "PLACEMENT_START_COPY_UNSUPPORTED", "含sloc尾整体拒绝");
    Require(Code(InvokePlacementTool("entities_rotate", f.Rotate(MapDataKind::Doodads, 2, 1.0), f.host)) == "PLACEMENT_SPECIAL_SOURCE_UNSUPPORTED", "特殊地物拒绝");
    Require(Code(InvokePlacementTool("entities_rotate", f.Rotate(MapDataKind::Units, 0, -1), f.host)) == "MAP_PLACEMENT_FACING_INVALID", "负弧度拒绝");
    auto malformed = f.Rotate(MapDataKind::Units, 0, 1); malformed["facing_radians"] = "1";
    Require(Code(InvokePlacementTool("entities_rotate", malformed, f.host)) == "ENTITY_ARGUMENT_TYPE", "不转换字符串数值");
    auto offset = f.Copy(MapDataKind::Units, {0}); offset["offset"] = QJsonObject{{"x", 1000001}, {"y", 0}, {"z", 0}};
    Require(Code(InvokePlacementTool("entities_copy", offset, f.host)) == "PLACEMENT_COORDINATE_INVALID", "world offset越界拒绝");
    offset["offset"] = QJsonObject{{"x", 1000000.001}, {"y", 0}, {"z", 0}};
    Require(Code(InvokePlacementTool("entities_copy", offset, f.host)) == "PLACEMENT_COORDINATE_INVALID",
        "float窄化不能隐藏略超上限的double偏移");
    Require(f.publishes == 0 && f.session.Revision() == 0 && !f.session.CanUndo()
        && f.current->Member(MapDataKind::Units).sources.front().data == original, "全部失败保留bytes/revision/history");
    Fixture changed;
    Require(InvokePlacementTool("entities_rotate", changed.Rotate(MapDataKind::Units, 0, 1), changed.host).value("ok").toBool(),
        "建立可被负极小弧度恢复的真实朝向");
    const auto revision = changed.session.Revision();
    const auto bytes = changed.current->Member(MapDataKind::Units).sources.front().data;
    Require(Code(InvokePlacementTool("entities_rotate", changed.Rotate(MapDataKind::Units, 0, -1e-50), changed.host))
        == "MAP_PLACEMENT_FACING_INVALID" && changed.session.Revision() == revision && changed.publishes == 1
        && changed.current->Member(MapDataKind::Units).sources.front().data == bytes,
        "负极小double不能窄化为负零后提交恢复原朝向");
}
void HostDenialAndCompleteEscapedBudgetPreserveRedoAndIdentity() {
    Fixture f;
    Require(InvokePlacementTool("entities_rotate", f.Rotate(MapDataKind::Units, 0, 1.0), f.host).value("ok").toBool(), "建立真实undo命令");
    const auto command = f.session.LastEdit().CommandIdentity(); Require(f.session.Undo(), "建立redo尾"); ++f.epoch;
    const auto revision = f.session.Revision();
    f.hostChanges = {{"title", "完整历史回执"}, {"items", QJsonArray{QJsonObject{{"before", QString(140000, QChar('"'))}}}}};
    Require(Code(InvokePlacementTool("entities_copy", f.Copy(MapDataKind::Units, {0}), f.host)) == "ENTITY_RESPONSE_BUDGET", "转义后完整changes预算须在发布前拒绝");
    Require(f.session.Revision() == revision && f.session.CanRedo() && !f.session.IsDirty() && f.publishes == 1, "budget失败保留redo与当前wire");
    f.hostChanges = {}; f.rejectPublish = true;
    Require(Code(InvokePlacementTool("entities_copy", f.Copy(MapDataKind::Units, {0}), f.host)) == "HOST_PUBLICATION_DENIED"
        && f.session.Revision() == revision && f.session.CanRedo(), "宿主密封patch拒绝不得发布命令");
    f.rejectPublish = false;
    const auto accepted = InvokePlacementTool("entities_copy", f.Copy(MapDataKind::Units, {0}), f.host);
    Require(accepted.value("ok").toBool() && accepted.value("command_identity").toString().toULongLong() == command + 1,
        "拒绝候选不得消耗commandidentity");
}
void StandaloneRecipeUsesOriginalReplayAndTransactionEnvelope() {
    Fixture copied(true);
    const auto result = InvokePlacementTool("entities_copy", copied.Copy(MapDataKind::Units, {0}), copied.host);
    Require(result.value("ok").toBool() && result.value("transaction_active").toBool() && result.value("transaction_id") == "71"
        && result.value("transaction_revision").isObject(), "独立写结果保留原事务外壳");
    const auto bytes = copied.current->Member(MapDataKind::Units).sources.front().data; const auto rev = copied.session.Revision();
    Require(Code(InvokePlacementTool("entities_copy", copied.Copy(MapDataKind::Units, {1}), copied.host)) == "PLACEMENT_RECIPE_CONFLICT"
        && copied.session.Revision() == rev && copied.current->Member(MapDataKind::Units).sources.front().data == bytes, "第二copy在发布前拒绝");
    Require(Code(InvokePlacementTool("entities_rotate", copied.Rotate(MapDataKind::Units, 0, 2), copied.host)) == "PLACEMENT_RECIPE_CONFLICT", "copy与rotation不混用");
    Fixture rotated(true);
    Require(InvokePlacementTool("entities_rotate", rotated.Rotate(MapDataKind::Units, 0, 1.0), rotated.host).value("ok").toBool()
        && InvokePlacementTool("entities_rotate", rotated.Rotate(MapDataKind::Units, 0, 2.0), rotated.host).value("ok").toBool(), "同原行可重复绝对rotation");
    Require(std::get<MapDataPlacementRotation>(*rotated.pending).facingRadians == 2.0f, "保留最后请求float");
    Require(InvokePlacementTool("entities_rotate", rotated.Rotate(MapDataKind::Units, 0, 0.0), rotated.host).value("ok").toBool()
        && !rotated.pending && !rotated.session.IsDirty(), "恢复原值清净差而不制造保存成功");
    Fixture mixed(true); auto region = *mixed.original->Regions(); region.entries[0].left -= 1;
    Require(mixed.session.EditRegions(region).Succeeded(), "建立原其他领域编辑");
    Require(Code(InvokePlacementTool("entities_copy", mixed.Copy(MapDataKind::Units, {0}), mixed.host)) == "MAP_PLACEMENT_REPLAY_MISMATCH"
        && mixed.publishes == 0, "whole replay拒绝fields+copy");
}
void ReadOnlyAndGuiPriorAuthorChangesRemainSeparate() {
    Fixture denied; denied.readOnly = true;
    Require(Code(InvokePlacementTool("entities_copy", denied.Copy(MapDataKind::Doodads, {0}), denied.host)) == "HOST_READ_ONLY"
        && denied.publishes == 0 && !denied.session.CanUndo(), "只读宿主拒绝原命令");
    Fixture gui; auto regions = *gui.original->Regions(); regions.entries[0].left -= 2;
    Require(gui.session.EditRegions(regions).Succeeded(), "建立GUI作者原区域编辑");
    const auto rb = gui.session.Replacements().front().data;
    Require(InvokePlacementTool("entities_copy", gui.Copy(MapDataKind::Doodads, {0}), gui.host).value("ok").toBool()
        && gui.current->Member(MapDataKind::Regions).sources.front().data == rb, "GUI不套whole standalone freeze且保留prior作者编辑");
}
void CreateFourStandardAndCustomKindsUseRealSources() {
    Fixture f;
    auto unit = NewRecord("unit", "u001");
    unit.insert("owner", 7); unit.insert("hero_level", 12); unit.insert("life_percent", 80); unit.insert("mana", 240);
    unit.insert("height_offset", 24); unit.insert("facing_radians", 1.25); unit.insert("variation", 3);
    unit.insert("scale", QJsonObject{{"x", 1}, {"y", 2}, {"z", 0.5}});
    unit.insert("item_sets", QJsonArray{QJsonObject{{"items", QJsonArray{
        QJsonObject{{"item_id", "i001"}, {"chance_percent", 40}}, QJsonObject{{"item_id", "I001"}, {"chance_percent", 60}}}}}});
    const auto request = f.Create(MapDataKind::Units, {unit, NewRecord("unit", "U001"), NewRecord("item", "i001"), NewRecord("item", "I001")});
    const auto result = InvokePlacementTool("entities_create", request, f.host);
    Require(result.value("ok").toBool() && result.value("operation") == "create" && result.value("applied").toBool()
        && result.value("planned_placements") == "4" && f.publishes == 1 && f.session.Revision() == 1, "四条同成员创建只发布一个命令");
    Require(result.value("before").toObject().value("rows").toArray().isEmpty()
        && result.value("after").toObject().value("rows").toArray().size() == 4, "创建回执不伪造before行");
    const auto& records = f.current->Units()->units;
    const auto terrain = f.session.InspectTerrainPoint({64, -128}); Require(terrain.Succeeded(), "真实地形采样");
    Require(records.size() == 7 && records[3].rawcode == "u001" && records[4].rawcode == "U001"
        && records[5].rawcode == "i001" && records[6].rawcode == "I001" && records[6].owner == 15,
        "四条原版与自定义单位和物品记录保持请求顺序");
    Require(records[3].position == std::array<float, 3>{64, -128, terrain.value->groundHeight + 24}
        && records[3].rotation == 1.25f && records[3].scale == std::array<float, 3>{1, 2, 0.5f}
        && records[3].variation == 3 && records[3].owner == 7 && records[3].heroLevel == 12
        && records[3].hitPoints == 80 && records[3].mana == 240 && records[3].itemSets[0].items[1].rawcode == "I001",
        "新建字段经共用服务写入原生记录");
    Require(f.objectLookups[static_cast<std::size_t>(ObjectData::ObjectType::Unit)] == 1
        && f.objectLookups[static_cast<std::size_t>(ObjectData::ObjectType::Item)] == 1, "同批类型目录和掉落目录只取一次");
    const auto ids = result.value("new_ids").toArray();
    const auto changes = result.value("changes").toObject().value("items").toArray();
    for (qsizetype i = 0; i < ids.size(); ++i) {
        const auto id = ids[i].toObject();
        Require(id.value("kind") == "units" && id.value("source_index").toString() == QString::number(i + 3)
            && id.value("creation_id").toString() == QString::number(i + 13)
            && id.value("row_key") == f.Locator(MapDataKind::Units, static_cast<std::size_t>(i + 3)).value("row_key")
            && id.value("member_sha256") == result.value("after").toObject().value("member").toObject().value("member_sha256")
            && changes[i].toObject().value("before").isNull(), "新增ID绑定当前成员SHA与完整行，before明确为null");
    }
    Require(ids.size() == 4 && changes.size() == 4, "四个实际新增身份和变化");
    const auto unitBytes = f.current->Member(MapDataKind::Units).sources[0].data;
    const auto doodads = InvokePlacementTool("entities_create", f.Create(MapDataKind::Doodads,
        {NewRecord("doodad", "D001"), NewRecord("doodad", "D101"), NewRecord("destructable", "b001"), NewRecord("destructable", "b101")}), f.host);
    Require(doodads.value("ok").toBool() && f.current->Doodads()->doodads.size() == 6 && f.current->Doodads()->specialDoodads.size() == 1
        && f.current->Doodads()->doodads[5].rawcode == "b101" && f.current->Member(MapDataKind::Units).sources[0].data == unitBytes,
        "四种地物来源创建且保留特殊尾与另一成员");
    Require(f.session.Undo() && f.session.Undo() && !f.session.IsDirty() && f.session.Redo() && f.session.Redo(),
        "两批创建沿同会话精确撤销重做");
}
void PreviewAllOperationsNeverAppliesOrConsumesIdentity() {
    Fixture f;
    Require(InvokePlacementTool("entities_rotate", f.Rotate(MapDataKind::Units, 0, 1), f.host).value("ok").toBool(), "建立真实命令身份");
    const auto command = f.session.LastEdit().CommandIdentity();
    Require(f.session.Undo(), "建立真实重做尾"); ++f.epoch;
    const auto revision = f.session.Revision(); const auto attempts = f.attempts;
    std::array<std::pair<const char*, QJsonObject>, 4> requests{{
        {"entities_create", f.Create(MapDataKind::Units, {NewRecord("unit", "U001")})},
        {"entities_delete", f.Delete(MapDataKind::Units, {1, 0})},
        {"entities_copy", f.Copy(MapDataKind::Doodads, {1})},
        {"entities_rotate", f.Rotate(MapDataKind::Units, 0, 2)}}};
    for (auto& [name, args] : requests) {
        args.insert("dry_run", true);
        const auto preview = InvokePlacementTool(name, args, f.host);
        Require(preview.value("ok").toBool() && preview.value("dry_run").toBool() && !preview.value("changed").toBool()
            && !preview.value("applied").toBool() && preview.value("applied_placements") == "0"
            && !preview.contains("session_identity") && !preview.contains("command_identity")
            && preview.value("new_ids").toArray().isEmpty(), "预览不声称提交或新增，不伪造命令身份");
        const bool creates = QString::fromUtf8(name) == "entities_create" || QString::fromUtf8(name) == "entities_copy";
        Require(preview.value("candidate_ids").toArray().size() == (creates ? 1 : 0), "仅创建/复制预览提供未提交候选ID");
        Require(f.attempts == attempts && f.session.Revision() == revision && f.session.CanRedo() && !f.session.IsDirty(),
            "预览不调用apply、不移动修订、不改变redo");
    }
    auto acceptedArgs = requests[0].second; acceptedArgs["dry_run"] = false;
    const auto created = InvokePlacementTool("entities_create", acceptedArgs, f.host);
    Require(created.value("ok").toBool() && created.value("command_identity").toString().toULongLong() == command + 1,
        "所有预览都不消耗下一实际命令身份");
}
void DeleteUsesOriginalRowsAndRejectsStaleLateSources() {
    Fixture f; const auto original = f.original->Units()->SerializeOriginal();
    const auto result = InvokePlacementTool("entities_delete", f.Delete(MapDataKind::Units, {1, 0}), f.host);
    Require(result.value("ok").toBool() && result.value("applied_placements") == "2" && result.value("operation") == "delete"
        && result.value("after").toObject().value("rows").toArray().isEmpty() && result.value("new_ids").toArray().isEmpty()
        && f.current->Units()->units.size() == 1 && f.current->Units()->units[0].rawcode == "sloc", "删除两行保留其余顺序，不伪造after行");
    const auto changes = result.value("changes").toObject().value("items").toArray();
    Require(changes.size() == 2 && changes[0].toObject().value("source_index") == "1"
        && changes[1].toObject().value("source_index") == "0" && changes[0].toObject().value("after").isNull(),
        "删除回执保持请求原行顺序，after明确为null");
    Require(f.session.Undo(), "删除可撤销"); ++f.epoch;
    Require(f.Base(MapDataKind::Units).size() > 0 && f.current->Units()->SerializeOriginal() == original, "删除撤销恢复原字节");
    const auto revision = f.session.Revision(); const auto attempts = f.attempts;
    Require(!InvokePlacementTool("entities_delete", f.Delete(MapDataKind::Units, {0, 2}), f.host).value("ok").toBool(), "禁止普通删除联动开始位置");
    Require(!InvokePlacementTool("entities_delete", f.Delete(MapDataKind::Units, {0, 0}), f.host).value("ok").toBool(), "重复删除原子拒绝");
    Require(Code(InvokePlacementTool("entities_delete", f.Delete(MapDataKind::Doodads, {2}), f.host)) == "PLACEMENT_SPECIAL_SOURCE_UNSUPPORTED",
        "特殊尾不能当普通地物删除");
    auto stale = f.Delete(MapDataKind::Units, {0, 1}); auto sources = stale.value("sources").toArray();
    auto late = sources[1].toObject(); late["row_key"] = "stale"; sources[1] = late; stale["sources"] = sources;
    Require(Code(InvokePlacementTool("entities_delete", stale, f.host)) == "MAP_PLACEMENT_ROW_STALE", "陈旧末行拒绝整批删除");
    Require(f.attempts == attempts && f.session.Revision() == revision && f.session.CanRedo(), "全部非法删除保留redo和命令身份");
}
void CreationValidationRejectsWholeBatchBeforePublish() {
    Fixture f; const auto valid = NewRecord("unit", "u001");
    std::vector<QJsonObject> rejected;
    auto bad = valid; bad["object_id"] = "xxxx"; rejected.push_back(bad);
    bad = valid; bad["object_kind"] = "item"; rejected.push_back(bad);
    bad = valid; bad["object_kind"] = "doodad"; bad["object_id"] = "D001"; rejected.push_back(bad);
    bad = valid; bad["x"] = 1024.5; rejected.push_back(bad);
    bad = valid; bad["x"] = 1024.000001; rejected.push_back(bad);
    bad = valid; bad["y"] = -1024.000001; rejected.push_back(bad);
    bad = valid; bad["owner"] = 16; rejected.push_back(bad);
    bad = valid; bad["owner"] = "1"; rejected.push_back(bad);
    bad = valid; bad["hero_level"] = 1.5; rejected.push_back(bad);
    bad = valid; bad["variation"] = 2147483648.0; rejected.push_back(bad);
    bad = valid; bad["facing_radians"] = -1e-50; rejected.push_back(bad);
    bad = valid; bad["scale"] = QJsonObject{{"x", 1}, {"y", 0}, {"z", 1}}; rejected.push_back(bad);
    bad = valid; bad["mana_percent"] = 50; rejected.push_back(bad);
    bad = valid; bad["item_sets"] = QJsonArray{QJsonObject{{"items", QJsonArray{QJsonObject{{"item_id", "xxxx"}, {"chance_percent", 100}}}}}}; rejected.push_back(bad);
    bad = valid; bad["item_sets"] = QJsonArray{QJsonObject{{"items", QJsonArray{
        QJsonObject{{"item_id", "i001"}, {"chance_percent", 60}}, QJsonObject{{"item_id", "I001"}, {"chance_percent", 60}}}}}}; rejected.push_back(bad);
    for (const auto& row : rejected) Require(!InvokePlacementTool("entities_create", f.Create(MapDataKind::Units, {valid, row}), f.host)
        .value("ok").toBool(), "合法前项加非法后项不能部分创建");
    auto wrongSha = f.Create(MapDataKind::Units, {valid}); wrongSha["expected_member_sha256"] = QString(64, '0');
    Require(Code(InvokePlacementTool("entities_create", wrongSha, f.host)) == "MAP_PLACEMENT_MEMBER_STALE", "创建仍绑定实际成员SHA");
    Require(f.attempts == 0 && f.publishes == 0 && f.session.Revision() == 0 && !f.session.CanUndo()
        && f.current->Units()->SerializeOriginal() == f.original->Units()->SerializeOriginal(), "所有失败零提交且原字节不变");
    auto missing = f.host; missing.objectContext = {};
    Require(Code(InvokePlacementTool("entities_create", f.Create(MapDataKind::Units, {valid}), missing)) == "ENTITY_CREATE_HOST_UNAVAILABLE",
        "宿主没有实际物编源时不猜对象");
}
void NewCommandsKeepPublicationAndStandaloneGates() {
    Fixture denied; denied.rejectPublish = true;
    const auto request = denied.Create(MapDataKind::Units, {NewRecord("unit", "u001")});
    Require(Code(InvokePlacementTool("entities_create", request, denied.host)) == "HOST_PUBLICATION_DENIED"
        && denied.session.Revision() == 0 && !denied.session.CanUndo(), "新建发布门拒绝时保留原会话");
    denied.rejectPublish = false;
    denied.hostChanges = {{"title", QString(140000, QChar('"'))}};
    Require(Code(InvokePlacementTool("entities_delete", denied.Delete(MapDataKind::Units, {0}), denied.host)) == "ENTITY_RESPONSE_BUDGET"
        && denied.session.Revision() == 0 && !denied.session.CanUndo(), "删除仍检查转义后的整份回执预算");
    Fixture created(true);
    const auto result = InvokePlacementTool("entities_create", created.Create(MapDataKind::Units,
        {NewRecord("unit", "U001"), NewRecord("item", "I001")}), created.host);
    Require(result.value("ok").toBool() && result.value("transaction_active").toBool() && result.value("transaction_id") == "71", "独立创建保留原事务身份");
    Require(Code(InvokePlacementTool("entities_delete", created.Delete(MapDataKind::Units, {0}), created.host)) == "PLACEMENT_RECIPE_CONFLICT"
        && created.publishes == 1 && created.session.Revision() == 1, "不借新工具放宽独立单批配方");
    Fixture deleted(true);
    Require(InvokePlacementTool("entities_delete", deleted.Delete(MapDataKind::Units, {0, 1}), deleted.host).value("ok").toBool()
        && std::holds_alternative<MapDataPlacementDelete>(*deleted.pending), "独立删除保留可重放的密封意图");
}
} // namespace

int main() {
    try {
        SchemaAndCopyReceiptUseOriginalRowsAndOrder(); RotationUnitsAndSpecialTailRemainExact();
        InvalidLateSourceAndStaleRequestsNeverPublish(); HostDenialAndCompleteEscapedBudgetPreserveRedoAndIdentity();
        StandaloneRecipeUsesOriginalReplayAndTransactionEnvelope(); ReadOnlyAndGuiPriorAuthorChangesRemainSeparate();
        CreateFourStandardAndCustomKindsUseRealSources(); PreviewAllOperationsNeverAppliesOrConsumesIdentity();
        DeleteUsesOriginalRowsAndRejectsStaleLateSources(); CreationValidationRejectsWholeBatchBeforePublish();
        NewCommandsKeepPublicationAndStandaloneGates();
        TestRegisteredPlacementHost();
        std::cout << "11 placement facade scenarios and actual registered host roundtrips passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
