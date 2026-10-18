#include "EditorProject.h"
#include "MapSaveJob.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"

#include <QDateTime>
#include <QUndoStack>

#include <algorithm>
#include <cctype>
#include "War3MapCore/ObjectData/LniObjectCodec.h"

// 地图保存的执行与提交阶段。执行阶段只读任务里的输入（可在工作线程运行，期间工程不得修改）；
// 提交阶段在界面线程写入诊断并切换到新基线。准备阶段见 MapCopySave.cpp。
namespace War3Map::QtProject {
namespace {

QString StageText(std::string_view stage) {
    if (stage == "generate") return EditorProject::tr("正在生成游戏脚本…");
    if (stage == "inject") return EditorProject::tr("正在注入函数库代码…");
    if (stage == "preprocess") return EditorProject::tr("正在预处理脚本…");
    if (stage == "wave") return EditorProject::tr("正在预处理脚本（Wave）…");
    if (stage == "template") return EditorProject::tr("正在展开脚本模板…");
    if (stage == "vjass") return EditorProject::tr("正在翻译 vJass…");
    if (stage == "jasshelper") return EditorProject::tr("正在编译并检查语法（jasshelper / pjass）…");
    if (stage == "check") return EditorProject::tr("正在检查语法…");
    if (stage == "pjass") return EditorProject::tr("正在检查语法（pjass）…");
    if (stage == "archive-in-place") return EditorProject::tr("正在打包并写回地图（原图移入备份）…");
    if (stage == "archive-candidate") return EditorProject::tr("正在打包地图…");
    if (stage == "archive-reopen") return EditorProject::tr("正在切换到保存结果…");
    return EditorProject::tr("正在构建游戏脚本…");
}

std::string MinimapMemberName(std::string_view name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    return key;
}

Status InstallPreservedMinimap(MapSaveJob& job, const MapData::MapDataEditSession& edits,
    Services::TerrainMinimapAutomaticPreparation prepared) {
    if (!prepared.preserved) return Status::Ok();
    const auto current = prepared.preserved->VerifyCurrent(job, edits);
    if (!current.Succeeded()) return current;
    job.minimapPreserve = std::move(prepared.preserved);
    job.beforePublish = [&job, previous = std::move(job.beforePublish)] {
        if (previous) { const auto checked = previous(); if (!checked.Succeeded()) return checked; }
        if (job.terrainSavePlan) {
            const auto checked = job.terrainSavePlan->VerifyFrozenOutput(job.sourceSha256, job.replacements, job.removals);
            if (!checked.Succeeded()) return checked;
        }
        return job.minimapPreserve->VerifyFrozenOutput(job);
    };
    return Status::Ok();
}

Status PrepareWorkerMinimapInputs(MapSaveJob& job, std::unique_ptr<MapData::MapDataEditSession>& edits) {
    const auto reject = [](const char* code, const char* message) {
        return Status::Failure("TerrainMinimapSave.Qt", code, message);
    };
    const bool automatic = job.minimapChoice == Services::TerrainMinimapSaveChoice::Automatic;
    if (!automatic && job.minimapChoice != Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly
        && job.minimapChoice != Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly)
        return reject("MINIMAP_SAVE_CHOICE", "未知的单次地形小地图保存选项。");
    if (automatic) {
        const auto required = Services::TerrainMinimapSave::RequiresAutomaticGeneration(job);
        if (!required.Succeeded() || !required.value) return {required.diagnostics};
        if (!*required.value) {
            // TGA或完全未变化的已有BLP不重新编码。地形本身的计划仍然必须
            // 在最终列表和回读阶段核验，不因跳过小地图而跳过正式保存门。
            if (job.terrainSavePlan) {
                const auto* task = &job;
                job.beforePublish = [plan = job.terrainSavePlan, task, previous = std::move(job.beforePublish)] {
                    if (previous) { const auto status = previous(); if (!status.Succeeded()) return status; }
                    return plan->VerifyFrozenOutput(task->sourceSha256, task->replacements, task->removals);
                };
            }
            job.minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
            return Status::Ok();
        }
    }
    if (!job.archive || (!automatic && (job.inPlace || job.testCopyOnly || job.triggerChanges || !job.objectTransactions.empty()
        || !job.removals.empty() || std::count_if(job.replacements.begin(), job.replacements.end(), [](const auto& member) {
            return MinimapMemberName(member.name) == "war3map.w3e";
        }) != 1)))
        return reject("MINIMAP_SAVE_DOMAIN", "worker补建只接受真实独立另存任务与唯一高度W3E，拒绝测试副本或混合领域。");

    // FromArchive仅复制七类MapData原生成员，不复制整幅地图。保留原有
    // Invalid/Ambiguous/Partial基线，不能以空MapDocument伪造DOO等成员不存在。
    // raw intake沿既有64MiB帽；解析/私有session/撤销历史沿各codec原上限，
    // 属于Qt重建工作，在helper的128MiB容器账之外，不宣称全保存/RSS上限。
    constexpr std::array<std::string_view, 8> nativeNames{
        "war3map.w3i", "table/w3i.ini", "war3mapUnits.doo", "war3map.doo",
        "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"};
    constexpr auto maximumRaw = MapData::TerrainSavePlanLimits{}.maximumBytes;
    constexpr auto maximumW3e = Render::TerrainMinimapAssetCaptureLimits{}.maxCurrentW3eBytes;
    std::vector<std::string> exactNativeNames;
    for (const auto name : nativeNames) exactNativeNames.emplace_back(name);
    ArchiveExactMemberLimits nativeLimits;
    nativeLimits.memberCount = nativeNames.size(); nativeLimits.memberBytes = maximumRaw; nativeLimits.totalBytes = maximumRaw;
    auto native = job.archive->CaptureExactMembers(exactNativeNames, nativeLimits);
    if (!native.Succeeded() || !native.value) return {native.diagnostics};
    std::vector<MapArchiveMember> nativeMembers;
    for (auto& member : *native.value) if (member.present) nativeMembers.push_back({member.name, std::move(member.bytes)});
    // 依赖明确名字捕获，DOO未列在(listfile)中也不能被误认成缺失。
    edits = std::make_unique<MapData::MapDataEditSession>(MapDocument::FromMembers(nativeMembers));
    {
        const std::array<std::string, 2> names{"war3map.w3e", "war3map.wpm"};
        ArchiveExactMemberLimits limits;
        // 同一4MiB单成员帽也覆盖WPM：允许域最多65536 tiles，其WPM至多
        // 65536×16 cells+16字节头，小于此上限；没有放宽原Capture累计额度。
        limits.memberCount = names.size(); limits.memberBytes = maximumW3e; limits.totalBytes = maximumRaw;
        const auto captured = job.archive->CaptureExactMembers(names, limits);
        if (!captured.Succeeded() || !captured.value) return {captured.diagnostics};
        if (!(*captured.value)[0].present || !(*captured.value)[1].present)
            return reject("MINIMAP_SAVE_SOURCE", "worker重建必须准确读到原W3E和WPM，不能把未知或缺失当作空来源。");
        const auto terrain = edits->AttachTerrain((*captured.value)[0].bytes);
        if (!terrain.Succeeded()) return terrain;
        const auto pathing = edits->AttachPathing((*captured.value)[1].bytes);
        if (!pathing.Succeeded()) return pathing;
    } // 在SetTerrain/Capture/Build之前释放本轮源raw临时缓冲。
    const MemberReplacement* terrainOutput = nullptr;
    const MemberReplacement* pathingOutput = nullptr;
    const std::array<std::pair<std::string_view, MapDataKind>, 7> editableMembers{{
        {"war3map.w3i", MapDataKind::Settings}, {"war3mapunits.doo", MapDataKind::Units},
        {"war3map.doo", MapDataKind::Doodads}, {"war3map.w3r", MapDataKind::Regions},
        {"war3map.w3c", MapDataKind::Cameras}, {"war3map.w3s", MapDataKind::Sounds}, {"war3map.wts", MapDataKind::Strings}}};
    for (const auto& member : job.replacements) {
        const auto key = MinimapMemberName(member.name);
        if (key == "war3map.w3e") terrainOutput = &member;
        else if (key == "war3map.wpm") pathingOutput = &member;
        else for (const auto& [name, kind] : editableMembers) if (key == name) {
            const auto restored = edits->SetSerialized(kind, member.data);
            if (!restored.Succeeded()) return restored;
        }
    }
    if (terrainOutput && terrainOutput->data.size() > maximumW3e)
        return reject("MINIMAP_SAVE_BUDGET", "worker地形输入超过既有4MiB地形额度。");
    if (terrainOutput) {
        const auto restored = pathingOutput ? edits->SetTerrainAndPathing(terrainOutput->data, pathingOutput->data)
                                            : edits->SetTerrain(terrainOutput->data);
        if (!restored.Succeeded()) return restored;
    } else if (pathingOutput) {
        const auto restored = edits->SetPathing(pathingOutput->data);
        if (!restored.Succeeded()) return restored;
    }
    for (const auto& removed : job.removals) for (const auto& [name, kind] : editableMembers)
        if (MinimapMemberName(removed) == name) {
            const auto restored = edits->Remove(kind);
            if (!restored.Succeeded()) return restored;
        }
    std::shared_ptr<const Services::TerrainMinimapSave> minimap;
    if (automatic) {
        auto prepared = Services::TerrainMinimapSave::PrepareAutomaticOrPreserve(job, *edits, job.minimapGameRoot);
        if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
        job.diagnostics.insert(job.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
        minimap = prepared.value->generated;
        if (!minimap) return InstallPreservedMinimap(job, *edits, std::move(*prepared.value));
    } else {
        auto prepared = Services::TerrainMinimapSave::Prepare(job, *edits, job.minimapGameRoot, job.minimapChoice);
        if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
        minimap = std::move(*prepared.value);
    }
    if (!minimap || !minimap->Plan())
        return reject("MINIMAP_SAVE_SOURCE", "worker未取得本次实际生成的小地图保存资格。");
    job.replacements.push_back(minimap->MinimapReplacement());
    const auto current = minimap->VerifyCurrent(job, *edits, job.minimapGameRoot);
    if (!current.Succeeded()) return current;
    job.minimapSave = minimap;
    job.terrainSavePlan = minimap->Plan();
    const auto& diagnostics = job.terrainSavePlan->Diagnostics();
    job.diagnostics.insert(job.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    const auto* task = &job;
    job.beforePublish = [minimap = std::move(minimap), task, previous = std::move(job.beforePublish)] {
        if (previous) {
            const auto checked = previous();
            if (!checked.Succeeded()) return checked;
        }
        return minimap->VerifyFrozenOutput(*task);
    };
    // 私有edits随本函数销毁。脚本已经完成；发布与回读仅消费owned task/封闭helper，
    // 不持EditorProject、mapEdits_或前台会话指针；UI代际由原Commit门独立核对。
    return Status::Ok();
}

Status PrepareWorkerMinimap(MapSaveJob& job) {
    // 若输入捕获本身超出生成器预算，仍由同一个能力分类决定能否保留。
    // 该临时会话只绑定本次 worker；编辑器代际仍由 Commit 的原门禁核对。
    auto edits = std::make_unique<MapData::MapDataEditSession>(MapDocument::FromMembers({}));
    auto status = PrepareWorkerMinimapInputs(job, edits);
    if (status.Succeeded() || job.minimapChoice != Services::TerrainMinimapSaveChoice::Automatic) return status;
    auto preserved = Services::TerrainMinimapSave::PreserveUnsupportedAutomatic(job, *edits, std::move(status.diagnostics));
    if (!preserved.Succeeded() || !preserved.value) return {std::move(preserved.diagnostics)};
    const auto installed = InstallPreservedMinimap(job, *edits, std::move(*preserved.value));
    if (!installed.Succeeded()) return installed;
    return {std::move(preserved.diagnostics)};
}

} // namespace

void EditorProject::RunMapSave(MapSaveJob& job) {
    // Run拒绝重复执行时，也不能留下上次的Passed结果供本次提交复用。
    job.terrainReadbackStatus.reset();
    job.terrainReadbackArchiveSha256.clear();
    if (job.executed) {
        job.minimapSave.reset();
        job.minimapPreserve.reset();
        Services::MapSaveService::Run(job); // 原服务统一报告SAVE_TASK_ALREADY_EXECUTED，不能再生成。
        return;
    }
    if (job.minimapChoice != Services::TerrainMinimapSaveChoice::Preserve) {
        job.prepareFinalDerived = [&job, previous = std::move(job.prepareFinalDerived)](Services::MapSaveTask& task) -> Status {
            if (previous) { const auto checked = previous(task); if (!checked.Succeeded()) return checked; }
            try { job.Report(tr("正在生成缺失的地形小地图…")); } catch (...) {}
            try {
                return PrepareWorkerMinimap(job);
            } catch (...) {
                return Status::Failure("TerrainMinimapSave.Qt", "MINIMAP_SAVE_WORKER_EXCEPTION",
                    "worker准备小地图时发生异常，本次任务已终止；未接受保存结果，当前编辑和原图保留。");
            }
        };
    }
    Services::MapSaveService::Run(job, [&job](std::string_view stage) { job.Report(StageText(stage)); });
    if (!job.succeeded || !job.nextSession || (!job.terrainSavePlan && !job.minimapPreserve)) return;
    // 展示回调沿用通用保存服务的合同，异常不能改变实际写出或回读资格。
    try { job.Report(tr("正在核对保存后的地形…")); } catch (...) {}
    try {
        // UI由既有worker调用；公开同步保存helper仍同步执行，不在Commit重复散列。
        job.terrainReadbackArchiveSha256 = job.nextSession->SourceSha256();
        job.terrainReadbackStatus = job.minimapSave ? job.minimapSave->VerifyReadback(job, *job.nextSession)
            : job.terrainSavePlan ? job.terrainSavePlan->VerifyReadback(*job.nextSession) : Status::Ok();
        if (job.terrainReadbackStatus->Succeeded() && job.minimapPreserve)
            job.terrainReadbackStatus = job.minimapPreserve->VerifyReadback(job, *job.nextSession);
    } catch (...) {
        // 领域回读位于通用Run的异常边界之外，失败须保留已写出路径及备份。
        job.terrainReadbackStatus = Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_READBACK_EXCEPTION",
            "保存后的地形核对发生异常，未接受此次保存结果。");
    }
    if (!job.terrainReadbackStatus->Succeeded()) {
        job.succeeded = false;
        const auto& diagnostics = job.terrainReadbackStatus->diagnostics;
        job.diagnostics.insert(job.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    }
}

std::optional<MapCopySaveResult> EditorProject::CommitMapSave(MapSaveJob& job) {
    AddDiagnostics(job.diagnostics);

    const auto rejectStaleCommit = [this, &job](const QString& code, const QString& reason)
        -> std::optional<MapCopySaveResult> {
        if (job.inPlace) {
            // 原路径已由保存服务写回；旧会话的文件身份失效，必须显式提示重新打开。
            sourceChangeSuspected_ = true;
            sourceChangeConfirmed_ = true;
            RefreshProjectWatch();
            AddDiagnostic(tr("错误"), tr("地图"), code,
                tr("地图已写回 %1（备份：%2），但%3；当前编辑未标记为已保存，必须重新打开地图。")
                    .arg(QString::fromStdWString(job.written.wstring()))
                    .arg(QString::fromStdWString(job.backup.wstring())).arg(reason));
        } else {
            AddDiagnostic(tr("错误"), tr("地图"), code,
                tr("候选地图已写出至 %1，但%2；当前编辑未标记为已保存，保存基线保持不变。")
                    .arg(QString::fromStdWString(job.written.wstring())).arg(reason));
        }
        return std::nullopt;
    };
    if (job.nextSession && (job.minimapChoice != Services::TerrainMinimapSaveChoice::Preserve || job.minimapSave)) {
        const bool automatic = job.minimapChoice == Services::TerrainMinimapSaveChoice::Automatic;
        const bool full = job.minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly;
        const bool declared = job.minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
        if (!automatic && !full && !declared)
            return rejectStaleCommit(tr("MINIMAP_SAVE_POLICY_MISMATCH"), tr("小地图保存选项与本次实际生成范围不一致"));
        // 保留分支由同一任务的 FrozenOutput/Readback 保护，不要求生成资格。
        if (!(automatic && job.minimapPreserve && !job.minimapSave)) {
            if (!job.minimapSave || job.terrainSavePlan != job.minimapSave->Plan())
                return rejectStaleCommit(tr("MINIMAP_SAVE_READBACK_MISSING"), tr("小地图保存缺少同一任务的生成资格和地形计划"));
            const auto actualRange = job.minimapSave->Manifest().rangePolicy;
            if ((automatic && (!job.minimapSave->ClassicManifest() || actualRange != Render::TerrainMinimapRangePolicy::ClassicPlayable))
                || (full && actualRange != Render::TerrainMinimapRangePolicy::FullW3e)
                || (declared && actualRange != Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge))
                return rejectStaleCommit(tr("MINIMAP_SAVE_POLICY_MISMATCH"), tr("小地图保存选项与本次实际生成范围不一致"));
        }
    }
    if ((job.terrainSavePlan || job.minimapPreserve) && job.nextSession) {
        if (!job.terrainReadbackStatus) {
            return rejectStaleCommit(tr("TERRAIN_SAVE_PLAN_READBACK_MISSING"), tr("尚未核对保存后的地形结果"));
        }
        if (!job.terrainReadbackStatus->Succeeded()) {
            return rejectStaleCommit(tr("TERRAIN_SAVE_PLAN_READBACK_REJECTED"), tr("保存后的地形核对未通过"));
        }
        if (job.terrainReadbackArchiveSha256 != job.nextSession->SourceSha256()) {
            return rejectStaleCommit(tr("TERRAIN_SAVE_PLAN_READBACK_STALE"), tr("地形核对结果不属于本次保存输出"));
        }
    }
    if (!job.succeeded || !job.nextSession) return std::nullopt;
    if (!job.preparedRevision) {
        return rejectStaleCommit(tr("SAVE_REVISION_BINDING_MISSING"), tr("保存任务缺少冻结的编辑修订身份"));
    }
    if (!ObserveEditState("地图保存提交前复核")) {
        return rejectStaleCommit(tr("SAVE_REVISION_UNAVAILABLE"), tr("无法复核当前编辑修订"));
    }
    const auto& frozen = *job.preparedRevision;
    if (editRevision_.Generation() != frozen.generation || editRevision_.Revision() != frozen.revision
        || editRevision_.Current() != frozen.scopes || luaMapTargetGeneration_ != frozen.luaTargetGeneration) {
        return rejectStaleCommit(tr("SAVE_REVISION_CHANGED"), tr("保存期间工程编辑状态发生变化"));
    }

    // 唯一提交点。后续保存以新文件为来源，包含此前已保存的物编及导入资源。
    MapCopySaveResult result;
    result.target = job.written;
    result.memberCount = job.memberCount;
    result.savedTriggerChanges = job.triggerChanges && !job.scriptRebuilt;
    result.scriptRebuilt = job.scriptRebuilt;
    result.scriptPreserved = job.scriptPreserved;
    result.inPlace = job.inPlace;
    result.backup = job.backup;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
    if (job.inPlace) {
        // 写回替换了被监视的源文件；这是自己的操作，不提示“来源已变化”。
        sourceWatchQuietUntil_ = QDateTime::currentMSecsSinceEpoch() + 3000;
        sourceChangeSuspected_ = false;
        sourceChangeConfirmed_ = false;
    }
    archiveSession_ = std::move(job.nextSession);
    readOnlyArchive_.reset();
    sourceArchivePath_ = job.written;
    if (luaMapTarget_) luaMapTargetSource_ = sourceArchivePath_;
    const bool generatedObjects = std::any_of(job.generatedObjectBaselines.begin(), job.generatedObjectBaselines.end(),
        [](const auto& document) { return document != nullptr; });
    if (generatedObjects) emit ObjectDocumentsAboutToReplace();
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        if (!job.generatedObjectBaselines[i]) continue;
        // 旧撤销命令保存旧文档地址；只清理被生成结果替换的类别，不能留悬空命令。
        if (objectUndoStacks_[i]) objectUndoStacks_[i]->clear();
        objectDocuments_[i] = std::move(job.generatedObjectBaselines[i]);
    }
    nativeObjectDocuments_ = std::move(job.nextBaselines);
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        if (!objectDocuments_[i] || objectDocuments_[i]->CloneSources().empty()) continue;
        // 新 native 基线已经包含这些克隆；旧撤销快照仍绑定旧基线，不能再恢复临时来源。
        if (objectUndoStacks_[i]) objectUndoStacks_[i]->clear();
        objectDocuments_[i]->ClearCloneSources();
    }
    // MapDocumentChanged 的名称/实例观察者也必须看到本次最终物编，而不是旧对象表。
    LoadMapDocument(true);
    restoredTriggerOverlay_ = false;
    triggerDirty_ = false;
    objectDirty_.fill(false);
    pendingImports_.clear();
    assetEdits_.clear();
    NotifyImportsChanged();
    if (triggerUndoStack_) triggerUndoStack_->setClean();
    for (auto* stack : objectUndoStacks_) {
        if (stack) stack->setClean();
    }
    CommitMapEditBaseline();
    if (generatedObjects) emit ObjectDocumentsReplaced();
    RefreshProjectWatch();
    const auto diagnosticCount = job.diagnostics.size();
    Services::FinalizeScriptWorkspace(job);
    if (job.diagnostics.size() > diagnosticCount) {
        AddDiagnostics(std::vector<Diagnostic>(job.diagnostics.begin() + diagnosticCount, job.diagnostics.end()));
    }
    if (iconProvider_ && !iconProvider_->SetMapArchive(sourceArchivePath_)) {
        AddDiagnostic(tr("警告"), tr("资源"), tr("SAVED_ICON_LAYER_UNAVAILABLE"), tr("地图已保存，但地图图标层未能重新挂载。"));
    }
    const QString target = QString::fromStdWString(job.written.wstring());
    if (job.inPlace) {
        AddDiagnostic(tr("信息"), tr("地图"), tr("MAP_SAVED"),
            tr("地图已保存：%1（%2 秒）；原图备份为 %3%4").arg(target).arg(result.seconds, 0, 'f', 1)
                .arg(QString::fromStdWString(job.backup.wstring()))
                .arg(job.prunedBackups > 0 ? tr("；已清理 %1 个旧备份").arg(job.prunedBackups) : QString{}));
    } else {
        AddDiagnostic(tr("信息"), tr("地图"), tr("REPACK_SAVED"),
            tr("地图已另存为：%1（%2 秒）").arg(target).arg(result.seconds, 0, 'f', 1));
    }
    if (result.savedTriggerChanges) {
        AddDiagnostic(tr("警告"), tr("构建"), tr("EDIT_COPY_SCRIPT_NOT_REBUILT"),
            tr("触发器编辑数据已保存；游戏脚本仍为源地图版本，须配置 YDWE 编译器后重新保存"));
    }
    return result;
}

std::optional<MapCopySaveResult> EditorProject::SaveMapCopy(const std::filesystem::path& outputPath,
    Services::TerrainMinimapSaveChoice minimapChoice) {
    auto job = PrepareMapSave(outputPath, false, minimapChoice);
    if (!job) return std::nullopt;
    savingMap_ = true;
    RunMapSave(*job);
    savingMap_ = false;
    return CommitMapSave(*job);
}

std::optional<MapCopySaveResult> EditorProject::SaveMapInPlace() {
    auto job = PrepareMapSave({}, true);
    if (!job) return std::nullopt;
    savingMap_ = true;
    RunMapSave(*job);
    savingMap_ = false;
    return CommitMapSave(*job);
}

} // namespace War3Map::QtProject
