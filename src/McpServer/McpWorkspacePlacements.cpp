#include "McpWorkspace.h"
#include "War3MapCore/MapData/MapDataPlacementPatchCodec.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <type_traits>

namespace War3Map::Mcp {
namespace {
template<class T, class U> Result<T> Failed(const U& source) { return {{}, source.diagnostics}; }
MapDataKind IntentKind(const MapData::MapDataPlacementIntent& intent) {
    const auto file = std::visit([](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, MapData::MapDataPlacementRotation>) return operation.source.id.file;
        else return operation.sources.front().id.file;
    }, intent);
    return file == MapData::PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
}
bool SameRotationSource(const MapData::MapDataPlacementIntent& previous, const MapData::MapDataPlacementIntent& next) {
    const auto* oldRotation = std::get_if<MapData::MapDataPlacementRotation>(&previous);
    const auto* newRotation = std::get_if<MapData::MapDataPlacementRotation>(&next);
    return oldRotation && newRotation && oldRotation->source.id == newRotation->source.id
        && oldRotation->source.expectedRowKey == newRotation->source.expectedRowKey
        && oldRotation->source.expectedMemberSha256 == newRotation->source.expectedMemberSha256;
}
bool IsMemberPath(const std::filesystem::path& path, const std::string& memberName) {
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    };
    const auto normalized = lower(path.generic_string()), member = lower(memberName);
    return normalized == member || normalized == "map/" + member;
}
}

Status McpWorkspace::ValidateMapDataOverlay() const {
    if (!activeEdit_ || !activeEdit_->mapData) return Status::Ok();
    if (activeEdit_->placementIntent) {
        const auto frozen = MapData::FreezeOriginalMapDataPlacement(*initialMapData_, *activeEdit_->mapData,
            *activeEdit_->placementIntent);
        return {frozen.diagnostics};
    }
    const auto frozen = MapData::FreezeOriginalMapDataFields(*initialMapData_, *activeEdit_->mapData);
    return {frozen.diagnostics};
}

Result<MapData::MapDataEditSession::EditReceipt> McpWorkspace::ApplyPlacementPlan(
    const MapData::PreparedMapDataPlacement& plan, const MapData::MapDataPublishValidator& prepublish) {
    using Receipt = MapData::MapDataEditSession::EditReceipt;
    const auto checked = CheckActive(ActiveTransactionId(), Revision());
    if (!checked.Succeeded()) return Failed<Receipt>(checked);
    const auto captured = CaptureMapDataSources();
    if (!captured.Succeeded()) return Failed<Receipt>(captured);
    if (!plan.OriginalReplayIntent())
        return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_ORIGINAL_SOURCE_REQUIRED",
            "独立预放命令只接受原来源内的精确摆放身份。");
    const auto before = activeEdit_->mapData;
    const auto beforeIntent = activeEdit_->placementIntent;
    const auto generation = activeEdit_->generation;
    const auto transactionId = activeEdit_->id;
    const auto baseRevision = activeEdit_->baseRevision;
    std::vector<MapArchiveMember> priorMembers;
    if (beforeIntent) {
        if (!SameRotationSource(*beforeIntent, *plan.OriginalReplayIntent()))
            return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_COMBINATION_UNSUPPORTED",
                "本原来源只支持一次同类复制，或同一原行重复绝对朝向；暂不组合其它预放操作。");
        const auto prior = MapData::FreezeOriginalMapDataPlacement(*initialMapData_, *before, *beforeIntent);
        if (!prior.Succeeded()) return Failed<Receipt>(prior);
        priorMembers.assign(prior.value->Members().begin(), prior.value->Members().end());
    } else {
        const auto fields = MapData::FreezeOriginalMapDataFields(*initialMapData_, *before);
        if (!fields.Succeeded()) return Failed<Receipt>(fields);
        if (!fields.value->Operations().empty())
            return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED",
                "独立预放事务暂不混入原来源字段净变化。");
    }
    const auto frozen = MapData::FreezeOriginalMapDataPlacement(*initialMapData_, plan.After(), *plan.OriginalReplayIntent());
    if (!frozen.Succeeded()) return Failed<Receipt>(frozen);
    auto next = plan.OwnedAfter();
    auto intent = std::make_shared<const MapData::MapDataPlacementIntent>(*plan.OriginalReplayIntent());
    auto patch = MapData::MapDataPlacementPatchCodec::Seal(*initialMapData_, *next,
        mapDataSource_->Snapshot(), *intent);
    if (!patch.Succeeded()) return Failed<Receipt>(patch);
    MapData::MapDataEditSession candidate(*initialMapData_);
    for (const auto& member : priorMembers) {
        const auto entry = std::find_if(initialMapData_->Members().begin(), initialMapData_->Members().end(),
            [&](const auto& input) { return input.memberName == member.name; });
        if (entry == initialMapData_->Members().end())
            return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_PATCH_SCOPE", "预放覆盖不属于原来源。");
        const auto seeded = candidate.SetSerialized(entry->kind, member.data);
        if (!seeded.Succeeded()) return Failed<Receipt>(seeded);
    }
    const auto applied = candidate.ApplyPreparedPlacement(plan, prepublish);
    if (!applied.Succeeded()) return Failed<Receipt>(applied);
    const auto receipt = candidate.LastEdit();
    const auto finalGuard = CheckActive(ActiveTransactionId(), Revision());
    if (!finalGuard.Succeeded()) return Failed<Receipt>(finalGuard);
    if (activeEdit_->id != transactionId || activeEdit_->baseRevision != baseRevision
        || activeEdit_->generation != generation || activeEdit_->mapData != before
        || activeEdit_->placementIntent != beforeIntent)
        return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_PLAN_STALE", "预放发布前原事务已变化。");
    const auto published = session_.ApplyMapDataPatch(activeEdit_->id, std::move(*patch.value));
    if (!published.Succeeded()) return Failed<Receipt>(published);
    activeEdit_->mapData = std::move(next);
    projectSearchMapCache_.reset();
    // 恢复原始 native 后清净意图，仍由上面的密封 patch 将原 bytes 写回旧覆盖。
    activeEdit_->placementIntent = frozen.value->HasChanges() ? std::move(intent) : nullptr;
    ++activeEdit_->generation;
    ++toolEpoch_;
    activeEdit_->previewedGeneration.reset();
    activeEdit_->validatedGeneration.reset();
    mapObjectReferences_.reset();
    return Result<Receipt>::Success(receipt);
}

Result<Services::MapEditSaveReceipt> McpWorkspace::ExportPlacementArchiveCandidate(
    TransactionId transactionId, const ProjectRevision& expectedRevision, Services::MapEditSaveRequest request) {
    using Receipt = Services::MapEditSaveReceipt;
    const auto intent = activeEdit_->placementIntent;
    const auto document = activeEdit_->mapData;
    const auto generation = activeEdit_->generation;
    const auto frozen = MapData::FreezeOriginalMapDataPlacement(*initialMapData_, *document, *intent);
    if (!frozen.Succeeded()) return Failed<Receipt>(frozen);
    if (!frozen.value->HasChanges())
        return Result<Receipt>::Failure("MCP", "MAP_PLACEMENT_NO_CHANGE", "当前原来源预放意图没有净变化。");
    const auto triggers = codec_.Serialize(activeEdit_->document, catalog_);
    if (!triggers.Succeeded()) return Failed<Receipt>(triggers);
    const auto originalWtg = IO::ReadFileBytes(projectRoot_ / wtgRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    const auto originalWct = IO::ReadFileBytes(projectRoot_ / wctRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    if (!originalWtg.Succeeded()) return Failed<Receipt>(originalWtg);
    if (!originalWct.Succeeded()) return Failed<Receipt>(originalWct);
    const auto objects = FreezeObjectTransactions(request.sourceMap, request.expectedSourceSha256);
    if (!objects.Succeeded()) return Failed<Receipt>(objects);
    if (triggers.value->wtg != *originalWtg.value || triggers.value->wct != *originalWct.value || !objects.value->empty())
        return Result<Receipt>::Failure("MCP", "MAP_PLACEMENT_MIXED_SAVE_UNSUPPORTED",
            "独立预放候选暂不混入触发器或物编净变化。");
    const auto source = [](const MapData::MapDataPlacementSource& input) {
        return Services::MapEditPlacementSource{input.id.file, input.id.index,
            input.expectedRowKey, input.expectedMemberSha256};
    };
    std::visit([&](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, MapData::MapDataPlacementRotation>)
            request.placementEdit = Services::MapEditPlacementRotation{source(operation.source), operation.facingRadians};
        else {
            Services::MapEditPlacementCopy copy;
            copy.offset = operation.offset;
            for (const auto& input : operation.sources) copy.sources.push_back(source(input));
            request.placementEdit = std::move(copy);
        }
    }, frozen.value->Intent());
    const auto guard = [this, transactionId, expectedRevision, document, intent, generation]() -> Status {
        if (!publishing_ || !activeEdit_ || activeEdit_->id != transactionId || activeEdit_->baseRevision != expectedRevision
            || session_.Revision() != expectedRevision || activeEdit_->generation != generation
            || activeEdit_->mapData != document || activeEdit_->placementIntent != intent
            || activeEdit_->previewedGeneration != generation || activeEdit_->validatedGeneration != generation)
            return Status::Failure("MCP", "MCP_ARCHIVE_PUBLISH_STALE", "预放归档发布身份已变化。");
        const auto environment = CheckEnvironmentUnchanged();
        if (!environment.Succeeded()) return environment;
        const auto freeze = MapData::FreezeOriginalMapDataPlacement(*initialMapData_, *document, *intent);
        if (!freeze.Succeeded()) return {freeze.diagnostics};
        const auto preview = session_.PreviewEdit(transactionId);
        if (!preview.Succeeded()) return {preview.diagnostics};
        std::set<std::string> matched;
        for (const auto& change : preview.value->changes) {
            // 已恢复原 bytes 的密封成员仍会留在原 overlay；仅接受精确原 SHA 的净零项。
            if (change.kind == "map-data" && change.beforeSha256 == change.afterSha256) {
                const auto original = std::find_if(initialMapData_->Members().begin(), initialMapData_->Members().end(),
                    [&](const auto& input) { return IsMemberPath(change.path, input.memberName); });
                if (original != initialMapData_->Members().end() && original->state == MapDataState::Ready
                    && original->sources.size() == 1) {
                    const auto sha = Sha256(original->sources.front().data);
                    if (sha.Succeeded() && change.beforeSha256 == *sha.value) continue;
                }
            }
            const auto member = std::find_if(freeze.value->Members().begin(), freeze.value->Members().end(),
                [&](const auto& input) { return IsMemberPath(change.path, input.name); });
            if (change.kind != "map-data" || member == freeze.value->Members().end() || !matched.insert(member->name).second)
                return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "预放覆盖含未冻结成员或重复来源。");
            const auto& original = initialMapData_->Member(IntentKind(*intent));
            const auto before = Sha256(original.sources.front().data), after = Sha256(member->data);
            if (!before.Succeeded() || !after.Succeeded() || change.beforeSha256 != *before.value || change.afterSha256 != *after.value)
                return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "预放覆盖 SHA 与完整原意图重放不符。");
        }
        if (matched.size() != freeze.value->Members().size())
            return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "预放覆盖未包含完整冻结成员。");
        return Status::Ok();
    };
    const auto checked = guard();
    if (!checked.Succeeded()) return Failed<Receipt>(checked);
    request.beforePublish = guard;
    return Services::SaveMapEditCandidate(request);
}

} // namespace War3Map::Mcp
