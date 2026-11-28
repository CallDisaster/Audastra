#include "ProjectPlacementImport.h"
#include "EditorProject.h"
#include "ObjectContextProvider.h"
#include "ObjectPlanCommit.h"
#include "Commands/MapDataCommands.h"

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

#include <QCoreApplication>
#include <QSet>
#include <QUndoStack>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace War3Map::QtProject {
namespace {
constexpr const char* kDomain = "Placement.Import";
using ObjectData::ObjectType;

const QUndoCommand* Top(const QUndoStack& stack) {
    return stack.index() > 0 ? stack.command(stack.index() - 1) : nullptr;
}

// 撤回并删除本次刚推入的命令，不留下可重做的残步。命令已不在栈顶（极少见）时保持原样。
void DropPushedCommand(QUndoStack& stack, const QUndoCommand* pushed) {
    if (!pushed || Top(stack) != pushed) return;
    stack.undo();
    // 作废的命令在 redo 时不再执行，直接移出栈。
    const_cast<QUndoCommand*>(pushed)->setObsolete(true);
    stack.redo();
}

bool Uses(const MapData::PlacementManifest& manifest, MapData::PlacementManifestKind kind) {
    return std::ranges::any_of(manifest.types, [kind](const auto& type) { return type.kind == kind; });
}
} // namespace

Status CheckPlacementImportPayloadBudget(const MapData::PlacementManifest& manifest) {
    using Policy = Services::ImportPayloadPolicy;
    std::uint64_t total = 0;
    for (const auto& file : manifest.files) {
        if (file.bytes.size() > Policy::ownedFileBytes || file.bytes.size() > Policy::ownedBatchBytes - total)
            return Status::Failure(kDomain, "PLACEMENT_IMPORT_UI_BUDGET",
                "编辑器内导入上限为单文件 64MiB、合计 256MiB（超出于 " + file.archivePath + "）；更大的清单请用命令行保存导入。");
        total += file.bytes.size();
    }
    return Status::Ok();
}

Result<Services::PlacementImportPlan> PlanProjectPlacementImport(EditorProject& project,
    const MapData::PlacementManifest& manifest, const Services::PlacementImportBoundsOptions& bounds) {
    using R = Result<Services::PlacementImportPlan>;
    if (!project.mapEdits_) return R::Failure(kDomain, "PLACEMENT_IMPORT_MAP_UNAVAILABLE", "先打开地图。");
    const auto terrain = project.mapEdits_->Terrain();
    if (!terrain) return R::Failure(kDomain, "PLACEMENT_IMPORT_TERRAIN", "地图地形尚未读取，不能确定摆放范围。");
    Services::PlacementImportInputs inputs;
    inputs.terrain = terrain->header;
    inputs.bounds = bounds;
    inputs.groundHeight = [terrain](double x, double y) {
        return MapData::SampleGroundHeight(*terrain, x, y, MapData::TerrainHeightSource::RawWithRamps);
    };
    // 上下文借用 provider 内的空表与名称快照；规划会复制文档，provider 只需活到规划结束。
    const auto provider = MakeObjectContextProvider(project);
    for (const auto [kind, type, target] : {
             std::tuple{MapData::PlacementManifestKind::Doodad, ObjectType::Doodad, &inputs.doodads},
             std::tuple{MapData::PlacementManifestKind::Unit, ObjectType::Unit, &inputs.units}}) {
        if (!Uses(manifest, kind)) continue;
        auto context = provider(type);
        if (!context.Succeeded() || !context.value) return {{}, std::move(context.diagnostics)};
        *target = *context.value;
    }
    QSet<QString> existing;
    if (!manifest.files.empty())
        for (const auto& entry : project.AssetEntries()) existing.insert(EditorProject::AssetKey(entry.path));
    inputs.memberExists = [&existing](std::string_view path) {
        return existing.contains(EditorProject::AssetKey(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()))));
    };
    return Services::PlanPlacementImport(manifest, inputs);
}

Result<Services::PlacementImportReport> CommitProjectPlacementImport(EditorProject& project,
    MapData::PlacementManifest manifest, const Services::PlacementImportBoundsOptions& bounds) {
    using R = Result<Services::PlacementImportReport>;
    if (project.savingMap_) return R::Failure(kDomain, "PLACEMENT_IMPORT_BUSY", "地图正在保存，保存完成后再导入。");
    if (const auto budget = CheckPlacementImportPayloadBudget(manifest); !budget.Succeeded()) return {{}, budget.diagnostics};
    auto planned = PlanProjectPlacementImport(project, manifest, bounds);
    if (!planned.Succeeded() || !planned.value) return {{}, std::move(planned.diagnostics)};
    auto& plan = *planned.value;
    auto* assets = project.AssetUndoStack();
    auto* world = project.mapDataUndoStack_;
    const auto objectStack = [&](const std::optional<Services::ObjectCommandPlan>& objects) -> QUndoStack* {
        return objects ? project.objectUndoStacks_[static_cast<std::size_t>(objects->subject.type)] : nullptr;
    };
    if ((!manifest.files.empty() && !assets) || !world || (plan.doodadObjects && !objectStack(plan.doodadObjects))
        || (plan.unitObjects && !objectStack(plan.unitObjects)))
        return R::Failure(kDomain, "PLACEMENT_IMPORT_UNDO_UNAVAILABLE", "导入涉及的撤销记录未就绪；地图未修改。");

    const auto label = QCoreApplication::translate("QtProject::PlacementImport", "导入摆放清单");
    std::vector<std::pair<QUndoStack*, const QUndoCommand*>> pushed;
    const auto rollback = [&] {
        for (auto step = pushed.rbegin(); step != pushed.rend(); ++step) DropPushedCommand(*step->first, step->second);
    };
    std::size_t replaced = 0;
    if (!manifest.files.empty()) {
        std::vector<EditorProject::ImportPayload> payloads;
        payloads.reserve(manifest.files.size());
        // 加宽过的模型改用规划里的副本字节，其余文件正文直接移入。
        for (auto& rewrite : plan.fileRewrites) manifest.files[rewrite.file].bytes = std::move(rewrite.bytes);
        plan.fileRewrites.clear();
        for (auto& file : manifest.files)
            payloads.push_back({QString::fromStdString(file.archivePath),
                std::make_shared<const std::vector<std::byte>>(std::move(file.bytes))});
        const auto* before = Top(*assets);
        const auto problem = project.AddImportPayloads(payloads, true, label);
        if (!problem.isEmpty())
            return R::Failure(kDomain, "PLACEMENT_IMPORT_FILES", problem.toStdString() + "；地图未修改。");
        if (Top(*assets) != before) pushed.emplace_back(assets, Top(*assets));
        replaced = static_cast<std::size_t>(std::ranges::count_if(payloads, [](const auto& payload) { return payload.replaced; }));
    }
    for (const auto* objects : {&plan.doodadObjects, &plan.unitObjects}) {
        if (!*objects) continue;
        auto* stack = objectStack(*objects);
        const auto* before = Top(*stack);
        const auto committed = CommitObjectCommandPlan(project, **objects);
        if (!committed.Succeeded()) { rollback(); return {{}, committed.diagnostics}; }
        if (Top(*stack) != before) pushed.emplace_back(stack, Top(*stack));
    }
    auto added = MapData::AddPlacementBatch(*project.mapEdits_, plan.units, plan.doodads);
    if (!added.Succeeded()) { rollback(); return {{}, std::move(added.diagnostics)}; }
    (void)project.ObserveMapDataEdit("导入摆放清单");
    PushMapDataEdit(project, label, "导入摆放清单");
    plan.report.replacedFiles = replaced;
    return R::Success(std::move(plan.report));
}

} // namespace War3Map::QtProject
