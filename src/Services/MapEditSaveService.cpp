#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainShadowBake.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"
#include "MapEditSaveValidation.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <set>
#include <type_traits>
#include <utility>

namespace War3Map::Services {
namespace {

using SaveResult = Result<MapEditSaveReceipt>;
using namespace MapEditValidation;

constexpr std::array kObjectTypes{
    ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability, ObjectData::ObjectType::Item,
    ObjectData::ObjectType::Buff, ObjectData::ObjectType::Upgrade, ObjectData::ObjectType::Doodad,
    ObjectData::ObjectType::Destructable,
};

SaveResult Fail(std::string code, std::string message) {
    return SaveResult::Failure("MapEdit.Save", std::move(code), std::move(message));
}

SaveResult Forward(std::vector<Diagnostic> diagnostics) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    })) {
        diagnostics.push_back({DiagnosticSeverity::Error, "MapEdit.Save", "SAVE_UPSTREAM_FAILED",
            "依赖的解析或保存步骤未返回完整结果；候选未提交。"});
    }
    SaveResult result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

Status ForwardStatus(std::vector<Diagnostic> diagnostics) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    })) {
        diagnostics.push_back({DiagnosticSeverity::Error, "MapEdit.Save", "SAVE_UPSTREAM_FAILED",
            "依赖的脚本输入步骤未返回完整结果；候选未提交。"});
    }
    Status status;
    status.diagnostics = std::move(diagnostics);
    return status;
}

// 类别的原生成员必须唯一且完整解析；W3I 允许仅带已知尾部诊断的部分状态（尾部原样保留）。
bool FieldMemberEditable(const MapMemberState& member) {
    if (member.sources.size() != 1) return false;
    if (member.state == MapDataState::Ready) return true;
    return member.kind == MapDataKind::Settings && member.state == MapDataState::Partial
        && std::all_of(member.diagnostics.begin(), member.diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.severity != DiagnosticSeverity::Error
                && (diagnostic.code == "W3I_UNKNOWN_TAIL" || diagnostic.code == "W3I_CUSTOM_TABLES_OMITTED");
        });
}

Status CommitTable(MapData::MapDataEditSession& edits, const MapData::MapFieldTables& tables, MapDataKind kind) {
    switch (kind) {
    case MapDataKind::Settings: return edits.EditSettings(*tables.settings);
    case MapDataKind::Units: return edits.EditUnits(*tables.units);
    case MapDataKind::Regions: return edits.EditRegions(*tables.regions);
    case MapDataKind::Cameras: return edits.EditCameras(*tables.cameras);
    case MapDataKind::Sounds: return edits.EditSounds(*tables.sounds);
    case MapDataKind::Strings: return edits.EditStrings(*tables.strings);
    default: return Status::Failure("MapEdit.Save", "SAVE_FIELD_KIND_UNSUPPORTED", "该类别没有可写字段。");
    }
}

Status FieldFailure(std::string code, std::string message) {
    return Status::Failure("MapEdit.Save", std::move(code), std::move(message));
}

// 字段修改的成员门禁与回读核对对所有类别相同：源成员逐字节往返、编辑会话写出的字节
// 恰为目标表的规范序列化且再次解析后不变、目标字段回读为新值；全为定长字段时，
// 成员等长且差异字节不超过 4×修改数。不按格式手算偏移。
Status ApplyFieldEdits(const MapEditSaveRequest& request, const MapDocument& map,
    MapData::MapDataEditSession& edits, std::vector<MapArchiveMember>& replacements,
    std::vector<MapEditMemberReceipt>& receipts) {
    std::vector<MapDataKind> kinds;
    std::vector<MapData::MapFieldEdit> fieldEdits;
    for (const auto& operation : request.fieldEdits) {
        fieldEdits.push_back(operation.edit);
        const auto kind = operation.edit.kind;
        if (const auto prior = std::find(kinds.begin(), kinds.end(), kind); prior != kinds.end()) {
            if (operation.expectedMemberSha256 != receipts[static_cast<std::size_t>(prior - kinds.begin())].originalSha256)
                return FieldFailure("SAVE_FIELD_MEMBER_STALE", "同一成员的后续字段 SHA-256 与原来源不符，请重新查询。");
            continue;
        }
        kinds.push_back(kind);
        const auto kindId = std::string(MapData::MapDataKindId(kind));
        if (MapData::MapFieldCatalog(kind).empty())
            return FieldFailure("SAVE_FIELD_KIND_UNSUPPORTED", "类别 " + kindId + " 没有字段目录。");
        const auto& member = map.Member(kind);
        if (!FieldMemberEditable(member)) return FieldFailure("SAVE_FIELD_SOURCE_UNSAFE",
            "源成员 " + member.memberName + " 缺失、歧义或无法完整解析，不能修改 " + kindId + "。");
        auto memberSha = Sha256(member.sources.front().data);
        if (!memberSha.Succeeded() || !memberSha.value) return ForwardStatus(std::move(memberSha.diagnostics));
        if (*memberSha.value != operation.expectedMemberSha256) return FieldFailure("SAVE_FIELD_MEMBER_STALE",
            member.memberName + " 的 SHA-256 与请求不符，请重新查询。");
        auto roundtrip = MapData::SerializeMapFieldTable(map, kind);
        if (!roundtrip.Succeeded() || !roundtrip.value || *roundtrip.value != member.sources.front().data)
            return FieldFailure("SAVE_FIELD_ROUNDTRIP_UNSAFE", "源成员 " + member.memberName + " 未通过逐字节往返。");
        receipts.push_back({member.memberName, *memberSha.value, {}});
    }
    auto tables = MapData::ApplyMapFieldEdits(map, fieldEdits);
    if (!tables.Succeeded() || !tables.value) return ForwardStatus(std::move(tables.diagnostics));
    bool redirected = false;
    for (const auto& edit : fieldEdits) {
        if (edit.kind != MapDataKind::Settings) continue;
        const auto* field = MapData::FindMapField(edit.kind, edit.field);
        const auto after = MapData::ReadMapField(*tables.value, edit.kind, edit.row, *field);
        if (!after.Succeeded() || !after.value) return ForwardStatus(after.diagnostics);
        redirected |= MapData::FormatMapFieldRaw(*after.value) != MapData::FormatMapFieldRaw(edit.value);
    }
    if (redirected) {
        // 共用字段层保留了 W3I 引用；用同一冻结器转为原 WTS 行绑定的保存操作，
        // 回读与成员 SHA 门禁仍核对实际写入正文，不为 CLI 另造一套字符串写出逻辑。
        if (!tables.value->strings)
            return FieldFailure("SAVE_FIELD_EDIT_SCOPE_INVALID", "设置引用修改没有生成受验证的 WTS 目标表。");
        auto projectedKinds = kinds;
        if (std::find(projectedKinds.begin(), projectedKinds.end(), MapDataKind::Strings) == projectedKinds.end())
            projectedKinds.push_back(MapDataKind::Strings);
        std::vector<MapArchiveMember> proposed;
        for (const auto kind : projectedKinds) {
            auto serialized = MapData::SerializeMapFieldTable(*tables.value, kind);
            if (!serialized.Succeeded() || !serialized.value) return ForwardStatus(std::move(serialized.diagnostics));
            if (*serialized.value != map.Member(kind).sources.front().data)
                proposed.push_back({map.Member(kind).memberName, std::move(*serialized.value)});
        }
        const auto projected = MapData::ProjectMapData(map, proposed);
        if (!projected.Succeeded() || !projected.value) return ForwardStatus(projected.diagnostics);
        const auto frozen = MapData::FreezeOriginalMapDataFields(map, *projected.value);
        if (!frozen.Succeeded() || !frozen.value) return ForwardStatus(frozen.diagnostics);
        fieldEdits.clear(); kinds.clear(); receipts.clear();
        for (const auto& operation : frozen.value->Operations()) {
            fieldEdits.push_back(operation.edit);
            const auto kind = operation.edit.kind;
            if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) continue;
            kinds.push_back(kind);
            receipts.push_back({map.Member(kind).memberName, operation.expectedMemberSha256, {}});
        }
    }
    for (const auto kind : kinds) {
        const auto committed = CommitTable(edits, *tables.value, kind);
        if (!committed.Succeeded()) return ForwardStatus(committed.diagnostics);
    }
    if (!edits.IsDirty()) return FieldFailure("SAVE_NO_FIELD_CHANGE", "字段修改未产生成员变化。");
    replacements = edits.Replacements();
    if (replacements.size() != kinds.size() || !edits.Removals().empty())
        return FieldFailure("SAVE_EDIT_SCOPE_INVALID", "字段修改只能替换所涉成员，且不能删除成员。");
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        const auto kind = kinds[index];
        auto& receipt = receipts[index];
        const auto replacement = std::find_if(replacements.begin(), replacements.end(),
            [&receipt](const auto& member) { return member.name == receipt.name; });
        if (replacement == replacements.end())
            return FieldFailure("SAVE_EDIT_SCOPE_INVALID", "字段修改没有生成成员 " + receipt.name + "。");
        const auto expected = MapData::SerializeMapFieldTable(*tables.value, kind);
        const auto reparsed = MapData::ParseMapFieldTable(kind, replacement->data);
        const auto stable = reparsed.Succeeded() && reparsed.value
            ? MapData::SerializeMapFieldTable(*reparsed.value, kind) : Result<std::vector<std::byte>>{};
        if (!expected.Succeeded() || !expected.value || *expected.value != replacement->data
            || !stable.Succeeded() || !stable.value || *stable.value != replacement->data) {
            return FieldFailure("SAVE_FIELD_EDIT_SCOPE_INVALID", "成员 " + receipt.name + " 的写出结果与目标字段不一致。");
        }
        std::size_t numericEdits = 0;
        bool allNumeric = true;
        for (const auto& edit : fieldEdits) {
            if (edit.kind != kind) continue;
            const auto* field = MapData::FindMapField(kind, edit.field);
            const auto after = MapData::ReadMapField(*reparsed.value, kind, edit.row, *field);
            if (!after.Succeeded() || !after.value || after.value->type != edit.value.type
                || MapData::FormatMapFieldRaw(*after.value) != MapData::FormatMapFieldRaw(edit.value)) {
                return FieldFailure("SAVE_FIELD_EDIT_SCOPE_INVALID",
                    "回读后字段 " + edit.field + " 不是请求的新值。");
            }
            if (field->type == MapData::MapFieldType::Bytes) allNumeric = false;
            else ++numericEdits;
        }
        const auto& original = map.Member(kind).sources.front().data;
        if (allNumeric) {
            std::size_t changed = 0;
            if (replacement->data.size() == original.size()) {
                for (std::size_t byte = 0; byte < original.size(); ++byte) changed += replacement->data[byte] != original[byte];
            }
            if (replacement->data.size() != original.size() || changed > numericEdits * 4) {
                return FieldFailure("SAVE_FIELD_EDIT_SCOPE_INVALID",
                    "定长字段修改改变了目标字段之外的 " + receipt.name + " 字节。");
            }
        }
        auto savedSha = Sha256(replacement->data);
        if (!savedSha.Succeeded() || !savedSha.value) return ForwardStatus(std::move(savedSha.diagnostics));
        receipt.savedSha256 = *savedSha.value;
    }
    return {std::move(tables.diagnostics)};
}

Status ApplyPlacementEdit(const MapEditPlacementOperation& operation, const MapDocument& map,
    MapData::MapDataEditSession& edits, std::vector<MapArchiveMember>& replacements,
    std::vector<MapEditMemberReceipt>& receipts) {
    std::vector<MapEditPlacementSource> sources;
    if (const auto* copy = std::get_if<MapEditPlacementCopy>(&operation)) sources = copy->sources;
    else if (const auto* rotation = std::get_if<MapEditPlacementRotation>(&operation)) sources.push_back(rotation->source);
    else if (const auto* removed = std::get_if<MapEditPlacementDelete>(&operation)) sources = removed->sources;

    std::vector<MapData::PlacementId> ids;
    ids.reserve(sources.size());
    const auto bindMember = [&](MapData::PlacementFile file, const std::string& expectedSha) -> Status {
        const auto kind = file == MapData::PlacementFile::Units
            ? MapDataKind::Units : MapDataKind::Doodads;
        const auto& member = map.Member(kind);
        if (!FieldMemberEditable(member) || member.state != MapDataState::Ready)
            return FieldFailure("SAVE_PLACEMENT_SOURCE_UNSAFE", "摆放成员缺失、歧义或无法完整解析：" + member.memberName);
        const bool first = std::none_of(receipts.begin(), receipts.end(), [&](const auto& receipt) {
            return receipt.name == member.memberName;
        });
        if (first) {
            auto sha = Sha256(member.sources.front().data);
            if (!sha.Succeeded() || !sha.value) return ForwardStatus(std::move(sha.diagnostics));
            auto serialized = kind == MapDataKind::Units ? map.Units()->Serialize() : map.Doodads()->Serialize();
            if (!serialized.Succeeded() || !serialized.value || *serialized.value != member.sources.front().data)
                return FieldFailure("SAVE_PLACEMENT_ROUNDTRIP_UNSAFE", "源摆放成员未通过逐字节往返：" + member.memberName);
            receipts.push_back({member.memberName, *sha.value, {}});
        }
        const auto& receipt = *std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
            return item.name == member.memberName;
        });
        if (expectedSha != receipt.originalSha256)
            return FieldFailure("SAVE_PLACEMENT_MEMBER_STALE", "摆放成员 SHA-256 已变化：" + member.memberName);
        return Status::Ok();
    };
    for (const auto& source : sources) {
        const auto bound = bindMember(source.file, source.expectedMemberSha256);
        if (!bound.Succeeded()) return bound;
        const auto kind = source.file == MapData::PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
        auto key = MapData::MapFieldRowKey(map, kind, source.row);
        if (!key.Succeeded() || !key.value || *key.value != source.expectedRowKey)
            return FieldFailure("SAVE_PLACEMENT_ROW_STALE", "摆放行身份已变化，请重新查询。");
        if (source.file == MapData::PlacementFile::Units) {
            ids.push_back(MapData::MakePlacementId(source.file, source.row, map.Units()->units[source.row]));
        } else {
            if (source.row >= map.Doodads()->doodads.size())
                return FieldFailure("SAVE_PLACEMENT_SPECIAL_UNSUPPORTED", "特殊装饰物不能作为摆放编辑源。");
            ids.push_back(MapData::MakePlacementId(source.file, source.row, map.Doodads()->doodads[source.row]));
        }
    }

    if (const auto* created = std::get_if<MapEditPlacementCreate>(&operation)) {
        const auto file = std::holds_alternative<std::vector<MapData::UnitPlacement>>(created->records)
            ? MapData::PlacementFile::Units : MapData::PlacementFile::Doodads;
        const auto bound = bindMember(file, created->expectedMemberSha256);
        if (!bound.Succeeded()) return bound;
    }

    if (const auto* copy = std::get_if<MapEditPlacementCopy>(&operation)) {
        auto result = MapData::CopyPlacements(edits, ids, copy->offset);
        if (!result.Succeeded() || !result.value) return ForwardStatus(std::move(result.diagnostics));
    } else if (const auto* rotation = std::get_if<MapEditPlacementRotation>(&operation)) {
        auto current = MapData::FindPlacement(map, ids.front());
        if (!current.Succeeded() || !current.value) return ForwardStatus(std::move(current.diagnostics));
        const MapData::PlacementMove move{ids.front(), current.value->position, rotation->facingRadians};
        const auto status = MapData::MovePlacements(edits, std::span(&move, 1));
        if (!status.Succeeded()) return ForwardStatus(status.diagnostics);
    } else if (const auto* created = std::get_if<MapEditPlacementCreate>(&operation)) {
        const auto added = MapData::AddPlacements(edits, created->records);
        if (!added.Succeeded()) return ForwardStatus(added.diagnostics);
    } else {
        const auto removed = MapData::DeletePlacements(edits, ids);
        if (!removed.Succeeded()) return ForwardStatus(removed.diagnostics);
    }
    if (!edits.IsDirty()) return FieldFailure("SAVE_NO_PLACEMENT_CHANGE", "摆放操作没有产生成员变化。");
    replacements = edits.Replacements();
    if (replacements.size() != receipts.size() || !edits.Removals().empty())
        return FieldFailure("SAVE_EDIT_SCOPE_INVALID", "摆放事务只能替换所涉成员，且不能删除成员。");
    const auto after = edits.Snapshot();
    for (auto& receipt : receipts) {
        const auto replacement = std::find_if(replacements.begin(), replacements.end(), [&](const auto& member) {
            return member.name == receipt.name;
        });
        if (replacement == replacements.end())
            return FieldFailure("SAVE_EDIT_SCOPE_INVALID", "摆放事务未产生目标成员：" + receipt.name);
        const bool unit = receipt.name == map.Member(MapDataKind::Units).memberName;
        auto expected = unit ? after.Units()->Serialize() : after.Doodads()->Serialize();
        if (!expected.Succeeded() || !expected.value || *expected.value != replacement->data)
            return FieldFailure("SAVE_PLACEMENT_EDIT_SCOPE_INVALID", "目标摆放成员与事务快照不一致。");
        if (unit) {
            auto parsed = MapData::ParseUnitPlacements(replacement->data);
            if (!parsed.Succeeded() || !parsed.value || parsed.value->Serialize().value != expected.value)
                return FieldFailure("SAVE_PLACEMENT_EDIT_SCOPE_INVALID", "单位摆放成员回读不稳定。");
        } else {
            auto parsed = MapData::ParseDoodadPlacements(replacement->data);
            if (!parsed.Succeeded() || !parsed.value || parsed.value->Serialize().value != expected.value)
                return FieldFailure("SAVE_PLACEMENT_EDIT_SCOPE_INVALID", "装饰物摆放成员回读不稳定。");
        }
        auto sha = Sha256(replacement->data);
        if (!sha.Succeeded() || !sha.value) return ForwardStatus(std::move(sha.diagnostics));
        receipt.savedSha256 = *sha.value;
    }
    return Status::Ok();
}

Result<Trigger::UiCatalog> ResolveEditCatalog(const std::filesystem::path& uiRoot,
    const std::optional<std::vector<std::string>>& requestedLayers) {
    const auto forward = [](std::vector<Diagnostic> diagnostics) {
        Result<Trigger::UiCatalog> failed;
        failed.diagnostics = ForwardStatus(std::move(diagnostics)).diagnostics;
        return failed;
    };
    // CLI/MCP 没有界面内存态；按与界面相同的默认启用规则解析 config。
    // 如果调用方提供已冻结层序，用它构造目录并在下游逐项核对。
    auto discovered = UiLibrary::DiscoverPackages(uiRoot);
    if (!discovered.Succeeded() || !discovered.value) return forward(std::move(discovered.diagnostics));
    const auto defaults = UiLibrary::ReconcileGlobal({}, *discovered.value);
    const auto inferredLayers = UiLibrary::EffectiveLayers(defaults, {}, *discovered.value);
    const auto layers = requestedLayers ? *requestedLayers : inferredLayers;
    auto configLayers = Trigger::UiCatalog::ReadConfigLayers(uiRoot);
    if (!configLayers.Succeeded() || !configLayers.value) return forward(std::move(configLayers.diagnostics));
    if (layers.empty() || (!requestedLayers && layers != *configLayers.value)) {
        return Result<Trigger::UiCatalog>::Failure("MapEdit.Save", "SAVE_UI_LAYERS_UNRESOLVED",
            "无法从 config 证明默认函数库层序；请显式提供本次保存的有效层序。");
    }
    std::set<std::string> uniqueLayers;
    if (layers.size() > 128) return Result<Trigger::UiCatalog>::Failure("MapEdit.Save", "SAVE_UI_LAYERS_UNRESOLVED",
        "函数库层序超过 128 项的保存预算。");
    for (const auto& layer : layers) {
        const bool discoveredLayer = std::any_of(discovered.value->begin(), discovered.value->end(),
            [&layer](const auto& package) { return package.id == layer; });
        if (layer.empty() || layer.size() > 256 || !uniqueLayers.insert(layer).second || !discoveredLayer) {
            return Result<Trigger::UiCatalog>::Failure("MapEdit.Save", "SAVE_UI_LAYERS_UNRESOLVED",
                "函数库层序包含重复、缺失或超预算的包；不能用未发现的目录构建脚本。");
        }
    }
    // config 的原字节也是公开 Catalog 身份的一部分。相同有效层序不能改写
    // 换行后再计算另一份 SHA，否则公开查询得到的身份无法用于同一次保存。
    auto catalog = layers == *configLayers.value
        ? Trigger::UiCatalog::Load(uiRoot) : Trigger::UiCatalog::LoadLayers(uiRoot, layers);
    if (!catalog.Succeeded() || !catalog.value) return forward(std::move(catalog.diagnostics));
    if (catalog.value->Layers() != layers) return Result<Trigger::UiCatalog>::Failure("MapEdit.Save", "SAVE_UI_LAYERS_STALE",
        "加载期间 config 的有效函数库层序已变化；请重新查询来源后再保存。");
    catalog.diagnostics.insert(catalog.diagnostics.end(), discovered.diagnostics.begin(), discovered.diagnostics.end());
    return catalog;
}

Status ApplyObjectTransactions(const MapEditSaveRequest& request, const MapArchiveSession& source,
    std::vector<MapArchiveMember>& replacements, std::vector<MapEditMemberReceipt>& receipts) {
    if (request.objectTransactions.empty() || request.objectTransactions.size() > kObjectTypes.size())
        return FieldFailure("SAVE_OBJECT_TRANSACTION_COUNT", "物编保存需要 1 到 7 个不同类别的冻结事务。");
    std::vector<std::string> names;
    for (const auto& snapshot : request.objectTransactions) {
        if (!snapshot || std::find(kObjectTypes.begin(), kObjectTypes.end(), snapshot->Type()) == kObjectTypes.end())
            return FieldFailure("SAVE_OBJECT_TRANSACTION_INVALID", "物编候选为空或不属于支持的七类格式。");
        const std::string name(ObjectData::NativeFileName(snapshot->Type()));
        if (std::find(names.begin(), names.end(), name) != names.end())
            return FieldFailure("SAVE_OBJECT_TRANSACTION_DUPLICATE", "同一物编类别只能提交一个最终冻结候选。");
        names.push_back(name);
    }
    // 精确捕获可以证明未列在 listfile 中的成员存在性，不能将匿名成员误当成空表。
    auto original = source.CaptureExactMembers(names);
    if (!original.Succeeded() || !original.value) return ForwardStatus(std::move(original.diagnostics));
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& member = (*original.value)[index];
        const auto& snapshot = *request.objectTransactions[index];
        const auto verified = snapshot.VerifyAgainst(source.SourceSha256(), member.present
            ? std::optional<std::span<const std::byte>>(member.bytes) : std::nullopt);
        if (!verified.Succeeded()) return verified;
        if (snapshot.OriginalNative() && std::ranges::equal(*snapshot.OriginalNative(), snapshot.EncodedNative()))
            return FieldFailure("SAVE_NO_OBJECT_CHANGE", "物编冻结候选与原生成员完全相同，未提交空改动。");
        replacements.push_back({names[index], {snapshot.EncodedNative().begin(), snapshot.EncodedNative().end()}});
        receipts.push_back({names[index], snapshot.OriginalSha256(), snapshot.CandidateSha256()});
    }
    return Status::Ok();
}

Status PrepareScript(MapSaveTask& task, const MapArchiveSession& source,
    const MapEditSaveRequest& request, std::vector<MapEditMemberReceipt>& editedMembers,
    std::optional<Trigger::TriggerEditBatchReceipt>& triggerReceipt,
    std::optional<Trigger::TriggerTransactionReceipt>& transactionReceipt) {
    const auto& uiRoot = request.uiRoot.empty() ? request.toolRoot : request.uiRoot;
    auto catalog = ResolveEditCatalog(uiRoot, request.effectiveLayers);
    if (!catalog.Succeeded() || !catalog.value) return ForwardStatus(std::move(catalog.diagnostics));
    const auto layers = catalog.value->Layers();
    if (!source.HasMember("war3map.wtg") || !source.HasMember("war3map.wct")) {
        return Status::Failure("MapEdit.Save", "SAVE_TRIGGER_PAIR_MISSING",
            "源地图缺少完整 WTG/WCT；不能用旧脚本保存地形修改。");
    }
    auto wtg = source.ReadMember("war3map.wtg"), wct = source.ReadMember("war3map.wct");
    if (!wtg.Succeeded() || !wtg.value) return ForwardStatus(std::move(wtg.diagnostics));
    if (!wct.Succeeded() || !wct.value) return ForwardStatus(std::move(wct.diagnostics));
    auto triggers = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *catalog.value);
    if (!triggers.Succeeded() || !triggers.value) return ForwardStatus(std::move(triggers.diagnostics));
    if (request.auxiliaryTransaction) {
        const auto& snapshot = *request.auxiliaryTransaction;
        if (!snapshot.HasChanges()) return FieldFailure("AUX_TRANSACTION_NO_CHANGE", "区域/镜头封印与原来源相同，未生成空改动候选。");
        // 精确查已知名字，不能把未列入 listfile 的成员误判为缺席。
        std::vector<std::string> names;
        for (const auto& member : snapshot.Original().Members()) names.push_back(member.memberName);
        const auto captured = source.CaptureExactMembers(names);
        if (!captured.Succeeded()) return ForwardStatus(captured.diagnostics);
        std::vector<MapArchiveMember> members;
        for (const auto& member : *captured.value) if (member.present) members.push_back({member.name, member.bytes});
        const auto original = MapDocument::FromMembers(members, source.SourcePath());
        AuxiliaryReferenceContext refs{&*triggers.value, true, {}};
        for (const auto& member : source.Members()) ObserveAuxiliaryScriptSource(refs, member.name);
        if (source.HasOpaqueMembers()) refs.opaqueScriptSources.push_back("未命名归档成员");
        const auto verified = snapshot.VerifyAgainst(original, refs);
        if (!verified.Succeeded()) return verified;
        for (const auto& member : snapshot.Members()) {
            const auto kind = member.name == "war3map.w3r" ? MapDataKind::Regions : MapDataKind::Cameras;
            const auto before = AuxiliaryEditing::MemberIdentity(original, kind), after = Sha256(member.data);
            if (!before.Succeeded()) return ForwardStatus(before.diagnostics);
            if (!after.Succeeded()) return ForwardStatus(after.diagnostics);
            task.replacements.push_back(member);
            editedMembers.push_back({member.name, *before.value, *after.value});
        }
    }
    if (request.triggerEdits) {
        const auto edited = Trigger::ApplyTriggerEditBatch(*triggers.value, *catalog.value,
            *wtg.value, *request.triggerEdits);
        if (!edited.Succeeded() || !edited.value) return ForwardStatus(edited.diagnostics);
        auto encoded = Trigger::WtgWctCodec{}.Serialize(*triggers.value, *catalog.value);
        if (!encoded.Succeeded() || !encoded.value) return ForwardStatus(encoded.diagnostics);
        const auto wtgSha = Sha256(encoded.value->wtg), wctSha = Sha256(encoded.value->wct);
        if (!wtgSha.Succeeded() || !wtgSha.value) return ForwardStatus(wtgSha.diagnostics);
        if (!wctSha.Succeeded() || !wctSha.value) return ForwardStatus(wctSha.diagnostics);
        editedMembers.push_back({"war3map.wtg", request.triggerEdits->expectedWtgSha256, *wtgSha.value});
        editedMembers.push_back({"war3map.wct", request.triggerEdits->expectedWctSha256, *wctSha.value});
        task.replacements.push_back({"war3map.wtg", std::move(encoded.value->wtg)});
        task.replacements.push_back({"war3map.wct", std::move(encoded.value->wct)});
        task.triggerChanges = true;
        triggerReceipt = *edited.value;
    } else if (request.triggerTransaction) {
        const auto& snapshot = *request.triggerTransaction;
        const auto verified = snapshot.VerifyAgainst(source.SourceSha256(), *wtg.value, *wct.value, *catalog.value);
        if (!verified.Succeeded()) return verified;
        // 完整类型图与成对产物来自同一封印；编译消费类型图，保存消费配对原编码字节。
        // 此处不消费 workspace lease，也不把保存成功描述为工作区已重新绑定到候选归档。
        *triggers.value = snapshot.Document();
        const auto& receipt = snapshot.Receipt();
        editedMembers.push_back({"war3map.wtg", receipt.originalWtgSha256, receipt.savedWtgSha256});
        editedMembers.push_back({"war3map.wct", receipt.originalWctSha256, receipt.savedWctSha256});
        task.replacements.push_back({"war3map.wtg", snapshot.Encoded().wtg});
        task.replacements.push_back({"war3map.wct", snapshot.Encoded().wct});
        task.triggerChanges = true;
        transactionReceipt = receipt;
    }
    ScriptSaveInputSources sources;
    sources.catalog = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    sources.triggers = std::make_unique<Trigger::TriggerDocument>(std::move(*triggers.value));
    sources.layers = layers;
    sources.toolRoot = request.toolRoot;
    sources.uiRoot = uiRoot;
    sources.gameRoot = request.gameRoot;
    sources.sourceEncodings = request.sourceEncodings;
    sources.includeAccess = request.includeAccess;
    sources.luaTarget = request.luaTarget;
    sources.scriptMember = Script::MapScriptMemberName(source);

    // 模板可能整类生成 native 物编。v2 类别需完整可替换基线；已证版式的经典 v1
    // 虽支持已有值修改，此处仍只提供查询记录，不能将该能力扩大为模板替换许可。
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        const auto type = kObjectTypes[index];
        const auto member = std::string(ObjectData::NativeFileName(type));
        const auto transaction = std::find_if(request.objectTransactions.begin(), request.objectTransactions.end(),
            [type](const auto& snapshot) { return snapshot && snapshot->Type() == type; });
        if (transaction != request.objectTransactions.end()) {
            const auto& snapshot = **transaction;
            const auto records = snapshot.CandidateNative().Objects();
            if (type == ObjectData::ObjectType::Unit) sources.units.assign(records.begin(), records.end());
            else if (type == ObjectData::ObjectType::Ability) sources.abilities.assign(records.begin(), records.end());
            else if (type == ObjectData::ObjectType::Item) sources.items.assign(records.begin(), records.end());
            sources.editableInputs[index] = std::make_unique<ObjectData::LniObjectDocument>(snapshot.EditedDocument());
            task.nextBaselines[index] = std::make_unique<ObjectData::NativeObjectDocument>(snapshot.CandidateNative());
            continue;
        }
        if (!source.HasMember(member)) continue;
        auto bytes = source.ReadMember(member);
        if (!bytes.Succeeded() || !bytes.value) return ForwardStatus(std::move(bytes.diagnostics));
        const ObjectData::NativeObjectCodec codec(type);
        auto parsed = codec.Parse(*bytes.value);
        auto* native = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
        if (native && native->Version() == 1) {
            const std::string expectedCode = "native-v1-" + std::string(ObjectData::ToString(type)) + "-read-only";
            const bool recognizedReadOnly = parsed.Succeeded() && native->IsStructurallySafe()
                && native->Supports(ObjectData::NativeObjectOperation::EditExistingValue)
                && !native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)
                && parsed.diagnostics.size() == 1
                && parsed.diagnostics.front().severity == DiagnosticSeverity::Warning
                && parsed.diagnostics.front().code == expectedCode;
            if (!recognizedReadOnly) {
                return Status::Failure("MapEdit.Save", "SAVE_OBJECT_BASELINE_UNSAFE",
                    "源地图经典 v1 物编成员无法建立完整只读查询记录：" + member);
            }
            const auto encoded = codec.Serialize(*native);
            if (!encoded.Succeeded() || !encoded.value || *encoded.value != *bytes.value)
                return Status::Failure("MapEdit.Save", "SAVE_OBJECT_BASELINE_UNSAFE",
                    "源地图经典v1物编未通过保版本原字节往返：" + member);
            const auto records = native->Objects();
            if (type == ObjectData::ObjectType::Unit) sources.units.assign(records.begin(), records.end());
            else if (type == ObjectData::ObjectType::Item) sources.items.assign(records.begin(), records.end());
            else if (type == ObjectData::ObjectType::Ability) sources.abilities.assign(records.begin(), records.end());
            task.diagnostics.insert(task.diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
            continue;
        }
        // 无法建立无损可编辑基线时不立即失败：本次保存不改该类别，原成员逐字节保留。
        // 若模板随后要整类改写它，MapSaveService 会因缺少可编辑来源而拒绝（SAVE_GENERATED_NATIVE_BASELINE_UNAVAILABLE）。
        const auto deferBaseline = [&](std::string reason) {
            task.diagnostics.push_back({DiagnosticSeverity::Warning, "MapEdit.Save", "SAVE_OBJECT_BASELINE_DEFERRED",
                reason + member + "。本次保存保持该成员原字节不变；脚本模板若要改写该类别将拒绝保存。"});
        };
        if (!parsed.Succeeded() || !native
            || !native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate) || !parsed.diagnostics.empty()) {
            // 解析问题作为原因说明附上；该成员不参与本次改写，所以不作为保存错误。
            for (auto diagnostic : parsed.diagnostics) {
                diagnostic.severity = DiagnosticSeverity::Warning;
                task.diagnostics.push_back(std::move(diagnostic));
            }
            deferBaseline("源地图物编类别无法建立无损可编辑基线：");
            continue;
        }
        auto encoded = codec.Serialize(*native);
        if (!encoded.Succeeded() || !encoded.value || *encoded.value != *bytes.value) {
            deferBaseline("源地图物编类别未通过原字节往返：");
            continue;
        }
        auto lniBytes = ObjectData::ConvertNativeObjectDataToLni(type, *bytes.value);
        if (!lniBytes.Succeeded() || !lniBytes.value || !lniBytes.diagnostics.empty()) {
            deferBaseline("源地图物编类别无法无损投影到编辑基线：");
            continue;
        }
        auto lni = ObjectData::LniObjectCodec(type).Parse(*lniBytes.value);
        auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(lni.document.get());
        if (!lni.Succeeded() || !editable || !editable->IsStructurallySafe() || !lni.diagnostics.empty()) {
            deferBaseline("源地图物编编辑基线无法解析：");
            continue;
        }
        const auto records = native->Objects();
        const auto projected = editable->Objects();
        if (records.size() != projected.size() || !std::equal(records.begin(), records.end(),
            projected.begin(), [](const auto& original, const auto& converted) {
                return original.key == converted.key && original.parentRawcode == converted.parentRawcode
                    && original.modifications.size() == converted.modifications.size()
                    && std::equal(original.modifications.begin(), original.modifications.end(),
                        converted.modifications.begin(), [](const auto& oldField, const auto& newField) {
                            return oldField.key == newField.key && oldField.value == newField.value
                                && oldField.valueWasOpaque == newField.valueWasOpaque;
                        });
            })) {
            deferBaseline("源地图物编投影后对象或字段数量发生变化：");
            continue;
        }
        if (index == 0) sources.units.assign(records.begin(), records.end());
        else if (index == 1) sources.abilities.assign(records.begin(), records.end());
        else if (index == 2) sources.items.assign(records.begin(), records.end());
        sources.editableInputs[index].reset(static_cast<ObjectData::LniObjectDocument*>(lni.document.release()));
        task.nextBaselines[index].reset(static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
    }
    task.preserveUnaffectedScript = true;
    auto prepared = PrepareScriptSaveInputs(task, std::move(sources));
    if (!prepared.Succeeded() || !prepared.value || *prepared.value != ScriptSavePreparationState::Ready) {
        return ForwardStatus(std::move(prepared.diagnostics));
    }
    task.diagnostics.insert(task.diagnostics.end(), catalog.diagnostics.begin(), catalog.diagnostics.end());
    task.diagnostics.insert(task.diagnostics.end(), triggers.diagnostics.begin(), triggers.diagnostics.end());
    task.diagnostics.insert(task.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
    return Status::Ok();
}

} // namespace

Result<MapEditSaveReceipt> SaveMapEditCandidate(
    const MapEditSaveRequest& request, const MapSaveService::Progress& progress) {
    bool written = false;
    try {
        const bool fieldRequest = !request.fieldEdits.empty();
        const bool placementRequest = !std::holds_alternative<std::monostate>(request.placementEdit);
        const bool terrainRequest = !request.terrainStrokes.empty();
        const bool triggerRequest = request.triggerEdits.has_value() || request.triggerTransaction != nullptr;
        const bool objectRequest = !request.objectTransactions.empty();
        const bool auxiliaryRequest = request.auxiliaryTransaction != nullptr;
        const bool terrainCompanion = terrainRequest && (fieldRequest || objectRequest)
            && !placementRequest && !triggerRequest && !auxiliaryRequest;
        if (terrainRequest && fieldRequest && std::any_of(request.fieldEdits.begin(), request.fieldEdits.end(),
            [](const auto& operation) {
                return operation.edit.kind != MapDataKind::Settings
                    || (operation.edit.field != "map_name" && operation.edit.field != "author"
                        && operation.edit.field != "description" && operation.edit.field != "recommended_players");
            }))
            return Fail("SAVE_TERRAIN_FIELD_SCOPE_INVALID", "地形笔划只能与地图名称、作者、说明和建议人数的字段操作合并。");
        const bool shadowOnly = request.rebuildTerrainShadow && !fieldRequest && !placementRequest
            && !terrainRequest && !triggerRequest && !objectRequest && !auxiliaryRequest;
        if (request.minimapChoice != TerrainMinimapSaveChoice::Automatic
            && request.minimapChoice != TerrainMinimapSaveChoice::Preserve
            && request.minimapChoice != TerrainMinimapSaveChoice::GenerateMissingGroundOnly
            && request.minimapChoice != TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly)
            return Fail("SAVE_MINIMAP_CHOICE_INVALID", "小地图保存选项未获识别。");
        if (request.minimapChoice != TerrainMinimapSaveChoice::Preserve
            && request.minimapChoice != TerrainMinimapSaveChoice::Automatic && !terrainRequest)
            return Fail("SAVE_MINIMAP_SCOPE_INVALID", "补建缺失小地图只能伴随非空干地高度另存事务。");
        if (request.rebuildTerrainShadow && request.minimapChoice != TerrainMinimapSaveChoice::Automatic
            && request.minimapChoice != TerrainMinimapSaveChoice::Preserve)
            return Fail("TERRAIN_SHADOW_MINIMAP_CONFLICT", "显式计算阴影不能与旧的单独补建小地图选项混用。");
        if (request.triggerEdits && request.triggerTransaction)
            return Fail("SAVE_TRIGGER_INPUT_CONFLICT", "触发器批次与完整冻结事务不能在同一次保存中混用。");
        if (request.triggerTransaction && request.triggerTransaction->IsSourceCompanion() && !objectRequest)
            return Fail("SAVE_SOURCE_COMPANION_WITHOUT_OBJECTS", "未变化的触发器配对封印只能伴随非空物编保存事务。");
        if (request.sourceMap.empty() || request.targetCandidate.empty() || request.toolRoot.empty()
            || request.gameRoot.empty() || !ValidSha(request.expectedSourceSha256)
            || ((terrainRequest || (!fieldRequest && !placementRequest && !triggerRequest && !objectRequest && !auxiliaryRequest))
                ? !ValidSha(request.expectedTerrainSha256)
                : !request.expectedTerrainSha256.empty())) {
            return Fail("SAVE_REQUEST_INVALID", "地图路径、数据根、源 SHA 和当前操作所需的成员 SHA 必须有效。");
        }
        if (static_cast<int>(fieldRequest) + static_cast<int>(placementRequest)
            + static_cast<int>(terrainRequest) + static_cast<int>(triggerRequest) + static_cast<int>(objectRequest) + static_cast<int>(auxiliaryRequest)
            - static_cast<int>(objectRequest && request.triggerTransaction != nullptr)
            - static_cast<int>(terrainCompanion) != (shadowOnly ? 0 : 1))
            return Fail("SAVE_OPERATION_SCOPE_INVALID", "一次保存只能提交一个领域操作、同源触发器与物编事务，或地形及一种受支持的说明/物编修改。");
        const auto bounded = shadowOnly ? Status::Ok() : fieldRequest ? ValidateFieldOperations(request.fieldEdits)
            : placementRequest ? ValidatePlacementOperation(request.placementEdit)
            : request.triggerEdits ? Trigger::ValidateTriggerEditBatch(*request.triggerEdits)
            : (request.triggerTransaction || objectRequest || auxiliaryRequest) ? Status::Ok()
                                         : ValidateStrokes(request.terrainStrokes);
        if (!bounded.Succeeded()) return Forward(bounded.diagnostics);
        if (terrainCompanion) {
            const auto strokes = ValidateStrokes(request.terrainStrokes);
            if (!strokes.Succeeded()) return Forward(strokes.diagnostics);
        }
        auto opened = MapArchiveSession::Open(request.sourceMap);
        if (!opened.Succeeded() || !opened.value) return Forward(std::move(opened.diagnostics));
        auto& source = *opened.value;
        if (source.SourceSha256() != request.expectedSourceSha256) {
            return Fail("SAVE_SOURCE_STALE", "源归档 SHA-256 与编辑请求不符，请重新获取数据快照。");
        }
        // 旧的显式 missing-only 入口必须在编辑/脚本准备前拒绝已有成员，
        // 包括 present-empty；最终派生阶段仍会再次核验，不能先写再发现冲突。
        if (request.minimapChoice == TerrainMinimapSaveChoice::GenerateMissingGroundOnly
            || request.minimapChoice == TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly) {
            const std::array<std::string, 1> names{"war3mapMap.blp"};
            const auto mini = source.DescribeExactMemberIdentities(source.SourceSha256(), names);
            if (!mini.Succeeded() || !mini.value) return Forward(mini.diagnostics);
            if (mini.value->members[0].present)
                return Fail("MINIMAP_SAVE_NOT_MISSING", "源图已有BLP小地图，不能使用只补建缺失小地图的选项。");
        }
        // 本轮默认小地图会消费W3I和两份DOO；匿名MPQ里“没列名”不代表
        // 不存在。所有已知地图数据在同一真实来源上按明确名字捕获，再交
        // 原MapDocument解析器，不能让对象捕获把未列名的现有摆放当作删除。
        const std::array<std::string, 8> nativeNames{"war3map.w3i", "table/w3i.ini", "war3mapUnits.doo",
            "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"};
        auto capturedNative = source.CaptureExactMembers(nativeNames);
        std::vector<Diagnostic> minimapInputFailure;
        const bool nativeCaptureFailed = !capturedNative.Succeeded() || !capturedNative.value;
        if (nativeCaptureFailed) {
            // 触发器/物编有自己的来源封印和解析链。这里的其它 native 成员
            // 只用于小地图，不能因它们超出生成预算而阻止本领域合法保存。
            // 失败仍交共用分类；来源/I/O等错误不会被当作空地图接受。
            if (terrainRequest || request.minimapChoice != TerrainMinimapSaveChoice::Automatic || (!triggerRequest && !objectRequest))
                return Forward(capturedNative.diagnostics);
            minimapInputFailure = capturedNative.diagnostics;
        }
        std::vector<MapArchiveMember> nativeMembers;
        if (!nativeCaptureFailed) for (auto& member : *capturedNative.value)
            if (member.present) nativeMembers.push_back({member.name, std::move(member.bytes)});
        const auto map = MapDocument::FromMembers(nativeMembers, source.SourcePath());
        MapData::MapDataEditSession edits(map);
        std::string originalTerrainShaValue, savedTerrainShaValue, savedPathingShaValue;
        std::vector<MapEditMemberReceipt> editedMembers;
        std::vector<MapArchiveMember> replacements;
        std::vector<Diagnostic> fieldDiagnostics;
        std::optional<Trigger::TriggerEditBatchReceipt> triggerReceipt;
        std::optional<Trigger::TriggerTransactionReceipt> transactionReceipt;
        std::shared_ptr<const MapData::TerrainSavePlan> terrainPlan;
        std::vector<MapData::TerrainPathingResources> waterRuleResources;
        std::shared_ptr<MapData::MapDataEditSession> terrainReplay;
        std::shared_ptr<const TerrainMinimapSave> minimapSave;
        std::shared_ptr<const TerrainMinimapPreserveGuard> minimapPreserve;

        // 地形笔划和Automatic小地图需要一个稳定的源地形会话。必须在任何
        // settings/物编/摆放编辑前附加 W3E/WPM，否则最终派生阶段再附加
        // 会撞上 MapDataEditSession 的 AttachLate 门禁。笔划必须有完整来源；
        // 仅Automatic的局部归档可以保持未附加，后续按真实成员决定是否跳过生成。
        if (terrainRequest || request.minimapChoice == TerrainMinimapSaveChoice::Automatic) {
            const std::array<std::string, 2> names{"war3map.w3e", "war3map.wpm"};
            const auto members = source.CaptureExactMembers(names);
            if (!members.Succeeded() || !members.value) {
                if (terrainRequest) return Forward(members.diagnostics);
                minimapInputFailure.insert(minimapInputFailure.end(), members.diagnostics.begin(), members.diagnostics.end());
            } else if ((*members.value)[0].present && (*members.value)[1].present) {
                auto attached = edits.AttachTerrain((*members.value)[0].bytes);
                if (attached.Succeeded()) attached = edits.AttachPathing((*members.value)[1].bytes);
                if (!attached.Succeeded()) {
                    if (terrainRequest) return Forward(attached.diagnostics);
                    minimapInputFailure.insert(minimapInputFailure.end(), attached.diagnostics.begin(), attached.diagnostics.end());
                }
            } else if (terrainRequest) {
                return Fail("SAVE_TERRAIN_PATHING_MISSING", "源地图必须同时包含唯一可读取的 W3E 和 WPM。");
            } else if (shadowOnly && (*members.value)[0].present && !(*members.value)[1].present) {
                // 独立阴影计算不消费WPM。这里只接受准确捕获证明的缺失，不能
                // 将损坏、读取失败或未知来源当成缺失，也不为生成图像伪造WPM。
                // 无关MapBLP暂不生成，现有保留guard仍核对其真实存在/缺失状态。
                minimapInputFailure.push_back({DiagnosticSeverity::Error, "TerrainMinimapSave",
                    "CLASSIC_MINIMAP_GENERATION_UNSUPPORTED",
                    "源图准确缺少WPM；仅计算阴影时不补写通行文件，自动小地图暂不生成。"});
            }
        }
        if (fieldRequest) {
            const auto applied = ApplyFieldEdits(request, map, edits, replacements, editedMembers);
            if (!applied.Succeeded()) return Forward(applied.diagnostics);
            fieldDiagnostics = applied.diagnostics;
        } else if (placementRequest) {
            const auto applied = ApplyPlacementEdit(request.placementEdit, map, edits, replacements, editedMembers);
            if (!applied.Succeeded()) return Forward(applied.diagnostics);
        } else if (objectRequest) {
            const auto applied = ApplyObjectTransactions(request, source, replacements, editedMembers);
            if (!applied.Succeeded()) return Forward(applied.diagnostics);
        } else if (shadowOnly) {
            // 计算阴影本身不重放空笔划，也不要求原图有WPM；W3E仍须逐字绑定。
            const auto terrainBytes = source.ReadMember("war3map.w3e");
            if (!terrainBytes.Succeeded() || !terrainBytes.value) return Forward(terrainBytes.diagnostics);
            const auto terrainSha = Sha256(*terrainBytes.value);
            if (!terrainSha.Succeeded() || !terrainSha.value) return Forward(terrainSha.diagnostics);
            if (*terrainSha.value != request.expectedTerrainSha256)
                return Fail("SAVE_TERRAIN_STALE", "W3E SHA-256 与阴影计算请求不符，请重新获取地形快照。");
            originalTerrainShaValue = savedTerrainShaValue = *terrainSha.value;
        }
        if (terrainRequest) {
            if (!source.HasMember("war3map.w3e") || !source.HasMember("war3map.wpm")) {
                return Fail("SAVE_TERRAIN_PATHING_MISSING", "源地图必须同时包含唯一可读取的 W3E 和 WPM。");
            }
            auto terrainBytes = source.ReadMember("war3map.w3e");
            auto pathingBytes = source.ReadMember("war3map.wpm");
            if (!terrainBytes.Succeeded() || !terrainBytes.value) return Forward(std::move(terrainBytes.diagnostics));
            if (!pathingBytes.Succeeded() || !pathingBytes.value) return Forward(std::move(pathingBytes.diagnostics));
            auto terrainSha = Sha256(*terrainBytes.value);
            if (!terrainSha.Succeeded() || !terrainSha.value) return Forward(std::move(terrainSha.diagnostics));
            if (*terrainSha.value != request.expectedTerrainSha256) {
                return Fail("SAVE_TERRAIN_STALE", "W3E SHA-256 与编辑请求不符，请重新获取地形快照。");
            }

            if (!edits.HasTerrainPathing()) {
                const auto attachedTerrain = edits.AttachTerrain(*terrainBytes.value);
                if (!attachedTerrain.Succeeded()) return Forward(attachedTerrain.diagnostics);
                const auto attachedPathing = edits.AttachPathing(*pathingBytes.value);
                if (!attachedPathing.Succeeded()) return Forward(attachedPathing.diagnostics);
            }
            const auto grid = edits.Terrain();
            if (!grid) return Fail("SAVE_TERRAIN_UNAVAILABLE", "源 W3E 无法建立地形编辑快照。");
            std::vector<MapData::TerrainStrokeIntent> intents;
            intents.reserve(request.terrainStrokes.size());
            for (const auto& stroke : request.terrainStrokes) {
                const auto points = WorldPoints(stroke, *grid);
                if (!points.Succeeded() || !points.value) return Forward(points.diagnostics);
                intents.push_back({stroke.brush, *points.value, stroke.followDoodads, stroke.waterBrush});
            }
            auto replayed = TerrainPathingSave::ReplayStrokes(source, map, *terrainBytes.value, *pathingBytes.value, intents, request.gameRoot);
            if (!replayed.Succeeded() || !replayed.value) return Forward(replayed.diagnostics);
            waterRuleResources = std::move(replayed.value->waterOwners);
            terrainReplay = std::move(replayed.value->session);
            if (!terrainReplay->IsDirty()) {
                return Fail("SAVE_NO_TERRAIN_CHANGE", "全部笔划结束后地形与源图相同；未生成空改动候选。");
            }
            // 领域候选已各自核对原成员SHA、前值和完整写出范围。先独立检查地形
            // 输出，再将有类型的说明表装入同一会话；物编仍交最终通行来源门核对。
            auto companionMembers = std::move(replacements);
            replacements = terrainReplay->Replacements();
            if (replacements.empty() || replacements.size() > 4 || !terrainReplay->Removals().empty())
                return Fail("SAVE_EDIT_SCOPE_INVALID", "地形事务生成了空替换、过多成员或删除成员。");
            std::array<bool, 4> seen{};
            for (const auto& member : replacements) {
                const std::size_t index = member.name == "war3map.w3e" ? 0
                    : member.name == "war3map.wpm" ? 1
                    : member.name == "war3map.doo" ? 2
                    : member.name == "war3mapUnits.doo" ? 3 : 4;
                if (index >= seen.size() || seen[index]) {
                    return Fail("SAVE_EDIT_SCOPE_INVALID", "地形事务生成了未获允许或重复的地图成员：" + member.name);
                }
                seen[index] = true;
            }
            const bool restrictionOnly = seen[1] && !seen[2] && !seen[3]
                && std::all_of(request.terrainStrokes.begin(), request.terrainStrokes.end(),
                    [](const auto& stroke) { return !stroke.waterBrush && stroke.brush.kind == MapData::TerrainBrushKind::PathingRestriction; });
            if (!seen[0] && !restrictionOnly)
                return Fail("SAVE_EDIT_SCOPE_INVALID", "地形事务缺少 W3E 替换，且不属于仅修改 WPM 的通行限制笔划。");
            if ((seen[3] || (seen[2] && std::none_of(request.terrainStrokes.begin(), request.terrainStrokes.end(),
                    [](const auto& stroke) { return stroke.waterBrush.has_value(); })))
                && std::none_of(request.terrainStrokes.begin(), request.terrainStrokes.end(),
                    [](const auto& stroke) { return stroke.followDoodads; })) {
                return Fail("SAVE_EDIT_SCOPE_INVALID", "未请求摆放对象随地面移动，却生成了摆放替换成员。");
            }
            const auto w3e = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) {
                return member.name == "war3map.w3e";
            });
            const auto wpm = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) {
                return member.name == "war3map.wpm";
            });
            auto savedTerrainSha = Sha256(w3e == replacements.end() ? *terrainBytes.value : w3e->data);
            auto savedPathingSha = Sha256(wpm == replacements.end() ? *pathingBytes.value : wpm->data);
            if (!savedTerrainSha.Succeeded() || !savedTerrainSha.value) return Forward(std::move(savedTerrainSha.diagnostics));
            if (!savedPathingSha.Succeeded() || !savedPathingSha.value) return Forward(std::move(savedPathingSha.diagnostics));
            originalTerrainShaValue = *terrainSha.value;
            savedTerrainShaValue = *savedTerrainSha.value;
            savedPathingShaValue = *savedPathingSha.value;
            if (fieldRequest) {
                for (const auto& member : companionMembers) {
                    const auto kind = member.name == "war3map.w3i" ? MapDataKind::Settings
                        : member.name == "war3map.wts" ? MapDataKind::Strings : MapDataKind::Count;
                    if (kind == MapDataKind::Count)
                        return Fail("SAVE_TERRAIN_FIELD_SCOPE_INVALID", "地图说明生成了未获允许的伴随成员。");
                    const auto parsed = MapData::ParseMapFieldTable(kind, member.data);
                    if (!parsed.Succeeded() || !parsed.value) return Forward(parsed.diagnostics);
                    const auto committed = CommitTable(*terrainReplay, *parsed.value, kind);
                    if (!committed.Succeeded()) return Forward(committed.diagnostics);
                }
                replacements = terrainReplay->Replacements();
            } else if (objectRequest) {
                replacements.insert(replacements.end(), std::make_move_iterator(companionMembers.begin()),
                    std::make_move_iterator(companionMembers.end()));
            }
        }
        // 原版通行生成在最终脚本完成后执行；此时还未取得最终替换列表，
        // 不把“须重建通行”提前当成不能保存。其它领域仍走原保存门。
        const auto& finalEdits = terrainReplay ? *terrainReplay : edits;
        const auto pathingGate = terrainRequest ? Status::Ok() : finalEdits.VerifyTerrainPathingForSave();
        if (!pathingGate.Succeeded()) return Forward(pathingGate.diagnostics);

        MapSaveTask task;
        task.beforePublish = request.beforePublish;
        if (!waterRuleResources.empty()) {
            // 每个owner都绑定原始W3E与本笔当前草稿；只向原始归档复核来源，
            // 不把编辑后的输出当成水刷来源。最终WPM另由下方Native保存计划证明。
            task.beforePublish = [&source, resources = std::move(waterRuleResources), previous = std::move(task.beforePublish)]() {
                if (previous) { const auto checked = previous(); if (!checked.Succeeded()) return checked; }
                for (const auto& owner : resources) {
                    const auto checked = owner.VerifyReadback(source, MapData::TerrainSavePlanLimits{}.maximumBytes);
                    if (!checked.Succeeded()) return checked;
                }
                return Status::Ok();
            };
        }
        task.mapObjectReferences = request.mapObjectReferences;
        task.mapObjectReferenceGameRoot = request.gameRoot;
        task.target = request.targetCandidate;
        task.requireGeneratedNativeByteEquality = true;
        task.objectTransactions = request.objectTransactions;
        const auto frozen = task.FreezeArchive(source);
        if (!frozen.Succeeded()) return Forward(frozen.diagnostics);
        task.diagnostics.insert(task.diagnostics.end(), fieldDiagnostics.begin(), fieldDiagnostics.end());
        task.replacements = std::move(replacements);
        const auto script = PrepareScript(task, source, request, editedMembers, triggerReceipt, transactionReceipt);
        if (!script.Succeeded()) return Forward(script.diagnostics);

        // 小地图/地形派生必须在最终脚本和模板替换完成后准备，避免其原字
        // 门禁看到过期的 replacements。回调只在共用保存服务的一次最终阶段执行。
        {
            task.prepareFinalDerived = [&task, &finalEdits, &request, &terrainPlan, &minimapSave,
                &minimapPreserve, &minimapInputFailure, nativeCaptureFailed, terrainRequest](MapSaveTask&) mutable -> Status {
                if (terrainRequest) {
                    auto prepared = TerrainPathingSave::PrepareForSave(*task.archive, finalEdits, request.gameRoot,
                        {}, task.replacements, task.removals);
                    if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
                    terrainPlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*prepared.value));
                    const auto installed = terrainPlan->InstallPathingOutput(finalEdits, task.replacements);
                    if (!installed.Succeeded()) return installed;
                    const auto checked = terrainPlan->VerifyForSave(finalEdits, task.sourceSha256, task.replacements, task.removals);
                    if (!checked.Succeeded()) return checked;
                }
                bool automaticRequired = true;
                if (request.minimapChoice == TerrainMinimapSaveChoice::Automatic) {
                    const auto required = TerrainMinimapSave::RequiresAutomaticGeneration(task);
                    if (!required.Succeeded() || !required.value)
                        minimapInputFailure.insert(minimapInputFailure.end(), required.diagnostics.begin(), required.diagnostics.end());
                    else automaticRequired = *required.value;
                }
                // MMP先进入同一个计划，之后MapBLP工厂复制计划时保留该完整输出。
                if (!terrainPlan && automaticRequired && minimapInputFailure.empty()
                    && request.minimapChoice == TerrainMinimapSaveChoice::Automatic) {
                    auto prepared = TerrainPathingSave::PrepareForSave(*task.archive, finalEdits, request.gameRoot,
                        {}, task.replacements, task.removals);
                    if (prepared.Succeeded() && prepared.value && prepared.value->SaveGate().Succeeded())
                        terrainPlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*prepared.value));
                    else {
                        const auto& notes = prepared.value ? prepared.value->SaveGate().diagnostics : prepared.diagnostics;
                        minimapInputFailure.insert(minimapInputFailure.end(), notes.begin(), notes.end());
                    }
                }
                if (request.minimapChoice == TerrainMinimapSaveChoice::Automatic
                    || request.minimapChoice == TerrainMinimapSaveChoice::Preserve) {
                    const auto markers = PrepareTerrainMinimapMarkers(task, request.gameRoot, &finalEdits, terrainPlan);
                    if (!markers.Succeeded()) return markers;
                }
                if (request.rebuildTerrainShadow) {
                    const auto shadow = PrepareTerrainShadowBake(task, request.gameRoot, &finalEdits, terrainPlan);
                    if (!shadow.Succeeded()) return shadow;
                }
                if (request.minimapChoice == TerrainMinimapSaveChoice::Automatic) {
                    auto prepared = (minimapInputFailure.empty() || (!automaticRequired && !nativeCaptureFailed))
                        ? TerrainMinimapSave::PrepareAutomaticOrPreserve(task, finalEdits, request.gameRoot, {}, terrainPlan)
                        : TerrainMinimapSave::PreserveUnsupportedAutomatic(task, finalEdits, minimapInputFailure);
                    if (!prepared.Succeeded() || !prepared.value) return Status{std::move(prepared.diagnostics)};
                    minimapSave = std::move(prepared.value->generated);
                    minimapPreserve = std::move(prepared.value->preserved);
                    task.diagnostics.insert(task.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
                    if (minimapPreserve) {
                        const auto checked = minimapPreserve->VerifyCurrent(task, finalEdits);
                        if (!checked.Succeeded()) return checked;
                    }
                } else if (request.minimapChoice != TerrainMinimapSaveChoice::Preserve) {
                    const auto prepared = TerrainMinimapSave::Prepare(task, finalEdits, request.gameRoot, request.minimapChoice);
                    if (!prepared.Succeeded() || !prepared.value) return Status{prepared.diagnostics};
                    minimapSave = *prepared.value;
                }
                if (!minimapSave && request.minimapChoice != TerrainMinimapSaveChoice::Automatic
                    && request.minimapChoice != TerrainMinimapSaveChoice::Preserve)
                    return Status::Failure("MapEditSaveService", "SAVE_MINIMAP_PREPARATION_MISSING",
                        "补建小地图未返回本次实际保存任务的完整计划。");
                if (minimapSave) {
                    task.replacements.push_back(minimapSave->MinimapReplacement());
                    terrainPlan = minimapSave->Plan();
                    const auto current = minimapSave->VerifyCurrent(task, finalEdits, request.gameRoot);
                    if (!current.Succeeded()) return current;
                }
                auto previous = std::move(task.beforePublish);
                if (terrainPlan || minimapPreserve) {
                    if (terrainPlan) {
                        const auto& notes = terrainPlan->Diagnostics();
                        task.diagnostics.insert(task.diagnostics.end(), notes.begin(), notes.end());
                    }
                    task.beforePublish = [taskPtr = &task, terrainPlan, minimapSave, minimapPreserve,
                        previous = std::move(previous)]() {
                        if (previous) { const auto checked = previous(); if (!checked.Succeeded()) return checked; }
                        const auto checked = minimapSave ? minimapSave->VerifyFrozenOutput(*taskPtr)
                            : terrainPlan ? terrainPlan->VerifyFrozenOutput(taskPtr->sourceSha256, taskPtr->replacements, taskPtr->removals)
                            : Status::Ok();
                        if (!checked.Succeeded()) return checked;
                        return minimapPreserve ? minimapPreserve->VerifyFrozenOutput(*taskPtr) : Status::Ok();
                    };
                } else if (previous) {
                    task.beforePublish = std::move(previous);
                }
                return Status::Ok();
            };
        }
        if (auxiliaryRequest) {
            task.beforePublish = [&task, snapshot = request.auxiliaryTransaction, previous = std::move(task.beforePublish)]() -> Status {
                if (previous) { const auto status = previous(); if (!status.Succeeded()) return status; }
                return VerifyAuxiliarySaveOutput(snapshot->Members(), task.replacements, task.removals);
            };
        }
        MapSaveService::Run(task, progress);
        if (!task.succeeded || !task.nextSession || !task.scriptRebuilt) {
            return Forward(std::move(task.diagnostics));
        }
        written = true;
        if (terrainPlan) {
            const auto readback = minimapSave ? minimapSave->VerifyReadback(task, *task.nextSession)
                : terrainPlan->VerifyReadback(*task.nextSession);
            if (!readback.Succeeded()) return Forward(readback.diagnostics);
            // 回执绑定最终重建的WPM，不返回生成前草稿的摘要。
            if (terrainPlan->Members()[1].outputSha256)
                savedPathingShaValue = *terrainPlan->Members()[1].outputSha256;
        }
        if (minimapPreserve) {
            const auto readback = minimapPreserve->VerifyReadback(task, *task.nextSession);
            if (!readback.Succeeded()) return Forward(readback.diagnostics);
        }
        // 保存服务已回读归档；再核对配对成员确实对应本次语义候选，不能只凭编译器 exit 0。
        if (triggerRequest || objectRequest || auxiliaryRequest) {
            for (const auto& member : editedMembers) {
                const auto bytes = task.nextSession->ReadMember(member.name);
                if (!bytes.Succeeded() || !bytes.value) return Forward(bytes.diagnostics);
                const auto sha = Sha256(*bytes.value);
                if (!sha.Succeeded() || !sha.value) return Forward(sha.diagnostics);
                if (*sha.value != member.savedSha256)
                    return Fail(auxiliaryRequest ? "SAVE_AUXILIARY_READBACK_MISMATCH" : objectRequest ? "SAVE_OBJECT_READBACK_MISMATCH" : "SAVE_TRIGGER_PAIR_READBACK_MISMATCH",
                        "候选已写出，但领域成员回读与本次冻结候选不一致：" + member.name);
            }
        }
        SaveResult result = SaveResult::Success({task.written, source.SourceSha256(),
            task.nextSession->SourceSha256(), std::move(originalTerrainShaValue), std::move(savedTerrainShaValue),
            std::move(savedPathingShaValue), task.memberCount, task.scriptSeconds, std::move(task.scriptLayers),
            request.terrainStrokes.size(), request.fieldEdits.size(),
            std::visit([](const auto& operation) -> std::size_t {
                using Operation = std::decay_t<decltype(operation)>;
                if constexpr (std::is_same_v<Operation, std::monostate>) return 0;
                else if constexpr (std::is_same_v<Operation, MapEditPlacementRotation>) return 1;
                else if constexpr (std::is_same_v<Operation, MapEditPlacementCreate>)
                    return std::visit([](const auto& records) { return records.size(); }, operation.records);
                else return operation.sources.size();
            }, request.placementEdit),
            std::move(editedMembers), std::move(triggerReceipt), std::move(transactionReceipt)});
        FinalizeScriptWorkspace(task);
        result.value->appliedObjectTransactionCount = request.objectTransactions.size();
        result.value->appliedAuxiliaryCommandCount = auxiliaryRequest ? request.auxiliaryTransaction->Commands().size() : 0;
        result.value->scriptPreserved = task.scriptPreserved;
        if (minimapSave) {
            const auto& manifest = minimapSave->Manifest();
            const auto& mip0 = minimapSave->DecodedMip0();
            result.value->generatedMissingMinimap = MapEditMinimapReceipt{
                manifest.outputBlpSha256, mip0.decodedRgbaSha256, manifest.manifestSha256,
                manifest.encodedBlpBytes, mip0.width, mip0.height, manifest.encodedMipCount, manifest.rangePolicy};
        }
        result.diagnostics = std::move(task.diagnostics);
        return result;
    } catch (const std::exception&) {
        return Fail("SAVE_PREPARATION_FAILED", written
            ? "候选已写出，但无法建立完整回执；请核对目标文件并重新打开。"
            : "地图编辑或保存准备发生异常；当前基线未切换，请核对目标路径。");
    }
}

Result<MapTriggerTextQueryReceipt> QueryMapTriggerText(const MapTriggerTextQueryRequest& request) {
    using QueryResult = Result<MapTriggerTextQueryReceipt>;
    const auto forward = [](std::vector<Diagnostic> diagnostics) {
        QueryResult result;
        result.diagnostics = ForwardStatus(std::move(diagnostics)).diagnostics;
        return result;
    };
    try {
        if (request.sourceMap.empty() || (request.uiRoot.empty() && request.toolRoot.empty())
            || !ValidSha(request.expectedSourceSha256)
            || (request.expectedCatalogSha256 && !ValidSha(*request.expectedCatalogSha256)))
            return QueryResult::Failure("MapEdit.Query", "TRIGGER_TEXT_QUERY_INVALID", "地图、UI 来源与查询绑定 SHA 无效。");
        auto opened = MapArchiveSession::Open(request.sourceMap);
        if (!opened.Succeeded() || !opened.value) return forward(std::move(opened.diagnostics));
        auto& source = *opened.value;
        if (source.SourceSha256() != request.expectedSourceSha256)
            return QueryResult::Failure("MapEdit.Query", "SAVE_SOURCE_STALE", "查询源归档 SHA 与请求不符。");
        auto catalog = ResolveEditCatalog(request.uiRoot.empty() ? request.toolRoot : request.uiRoot, request.effectiveLayers);
        if (!catalog.Succeeded() || !catalog.value) return forward(std::move(catalog.diagnostics));
        if (request.expectedCatalogSha256 && catalog.value->Sha256() != *request.expectedCatalogSha256)
            return QueryResult::Failure("MapEdit.Query", "TRIGGER_EDIT_SOURCE_STALE", "查询 Catalog 已变化；expected="
                + *request.expectedCatalogSha256 + "; actual=" + catalog.value->Sha256());
        const std::array<std::string, 2> names{"war3map.wtg", "war3map.wct"};
        auto pair = source.CaptureExactMembers(names);
        if (!pair.Succeeded() || !pair.value) return forward(std::move(pair.diagnostics));
        if (!(*pair.value)[0].present || !(*pair.value)[1].present)
            return QueryResult::Failure("MapEdit.Query", "SAVE_TRIGGER_PAIR_MISSING", "源地图缺少完整 WTG/WCT 配对成员。");
        const auto& wtg = (*pair.value)[0].bytes;
        auto document = Trigger::WtgWctCodec{}.Parse(wtg, (*pair.value)[1].bytes, *catalog.value);
        if (!document.Succeeded() || !document.value) return forward(std::move(document.diagnostics));
        auto page = Trigger::QueryTriggerText(*document.value, *catalog.value, request.query);
        if (!page.Succeeded() || !page.value) return forward(std::move(page.diagnostics));
        MapTriggerTextQueryReceipt receipt{source.SourceSha256(), std::move(*page.value), catalog.value->Layers(), {}};
        if (request.query.slot == Trigger::TriggerTextSlot::TriggerCode) {
            const auto trigger = std::find_if(document.value->triggers.begin(), document.value->triggers.end(),
                [&](const auto& value) { return value.id == request.query.triggerId; });
            if (trigger != document.value->triggers.end() && trigger->type == 0 && trigger->customTextFlag == 0
                && trigger->ecas.size() <= 4096) {
                if (trigger->origin.offset > wtg.size() || trigger->origin.length > wtg.size() - trigger->origin.offset)
                    return QueryResult::Failure("MapEdit.Query", "TRIGGER_EDIT_SOURCE_SPAN_INVALID", "触发器原树来源范围越界。");
                const auto sha = Sha256(std::span<const std::byte>(wtg).subspan(trigger->origin.offset, trigger->origin.length));
                if (!sha.Succeeded() || !sha.value) return forward(sha.diagnostics);
                Trigger::TriggerEditTriggerTarget target{trigger->id, {}, *sha.value};
                for (const auto index : trigger->ecas) target.expectedRootIds.push_back(document.value->ecaNodes[index].id);
                receipt.guiTarget = std::move(target);
            }
        }
        const auto unchanged = source.VerifySourceUnchanged();
        if (!unchanged.Succeeded()) return forward(unchanged.diagnostics);
        auto result = QueryResult::Success(std::move(receipt));
        result.diagnostics = std::move(catalog.diagnostics);
        result.diagnostics.insert(result.diagnostics.end(), document.diagnostics.begin(), document.diagnostics.end());
        return result;
    } catch (const std::exception&) {
        return QueryResult::Failure("MapEdit.Query", "TRIGGER_TEXT_QUERY_FAILED", "配对触发器文本查询失败；没有写入地图。");
    }
}

} // namespace War3Map::Services
