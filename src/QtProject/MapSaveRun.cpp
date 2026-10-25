#include "EditorProject.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"
#include "War3MapCore/Services/TerrainShadowBake.h"
#include "War3MapCore/Services/TerrainHistorySave.h"
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
#include <exception>
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
        return job.minimapPreserve->VerifyFrozenOutput(job);
    };
    return Status::Ok();
}

Status RestoreWorkerTerrainSession(MapSaveJob& job,
    std::shared_ptr<const MapData::MapDataEditSession>& edits) {
    const auto reject = [](const char* code, const char* message) {
        return Status::Failure("TerrainMinimapSave.Qt", code, message);
    };
    if (job.terrainSessionSnapshot) {
        edits = job.terrainSessionSnapshot;
        if (job.pathingRestrictionDraft) {
            const auto declaration = edits->SerializePathingRestrictionDraft();
            if (!declaration.Succeeded() || !declaration.value) return {declaration.diagnostics};
            if (*declaration.value != *job.pathingRestrictionDraft)
                return reject("PATHING_DRAFT_IDENTITY", "冻结的通行限制声明与保存会话不一致，本次未写出地图。");
        }
    } else {
        // FromArchive仅复制七类MapData原生成员，不复制整幅地图。保留原有
        // Invalid/Ambiguous/Partial基线，不能以空MapDocument伪造DOO等成员不存在。
        // raw intake沿既有64MiB帽；解析/私有session/撤销历史沿各codec原上限，
        // 属于Qt重建工作，在helper的128MiB容器账之外，不宣称全保存/RSS上限。
        constexpr std::array<std::string_view, 8> nativeNames{
            "war3map.w3i", "table/w3i.ini", "war3mapUnits.doo", "war3map.doo",
            "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"};
        constexpr auto maximumRaw = MapData::TerrainSavePlanLimits{}.maximumBytes;
        const auto maximumW3e = job.terrainDerivationRequired ? maximumRaw
            : Render::TerrainMinimapAssetCaptureLimits{}.maxCurrentW3eBytes;
        std::vector<std::string> exactNativeNames;
        for (const auto name : nativeNames) exactNativeNames.emplace_back(name);
        ArchiveExactMemberLimits nativeLimits;
        nativeLimits.memberCount = nativeNames.size(); nativeLimits.memberBytes = maximumRaw; nativeLimits.totalBytes = maximumRaw;
        auto native = job.archive->CaptureExactMembers(exactNativeNames, nativeLimits);
        if (!native.Succeeded() || !native.value) return {native.diagnostics};
        std::vector<MapArchiveMember> nativeMembers;
        for (auto& member : *native.value) if (member.present) nativeMembers.push_back({member.name, std::move(member.bytes)});
        // 依赖明确名字捕获，DOO未列在(listfile)中也不能被误认成缺失。
        auto restoredSession = std::make_shared<MapData::MapDataEditSession>(MapDocument::FromMembers(nativeMembers));
        std::vector<std::byte> sourceTerrain, sourcePathing;
        {
            const std::array<std::string, 2> names{"war3map.w3e", "war3map.wpm"};
            ArchiveExactMemberLimits limits;
            // 地形保存沿Core的64MiB原始输入帽；仅小地图请求仍沿原4MiB帽。
            // 后续小地图单元独立执行其预算，不因地形保存而扩大生成器容量。
            limits.memberCount = names.size(); limits.memberBytes = maximumW3e; limits.totalBytes = maximumRaw;
            auto captured = job.archive->CaptureExactMembers(names, limits);
            if (!captured.Succeeded() || !captured.value) return {captured.diagnostics};
            if (!(*captured.value)[0].present || !(*captured.value)[1].present)
                return reject("MINIMAP_SAVE_SOURCE", "worker重建必须准确读到原W3E和WPM，不能把未知或缺失当作空来源。");
            const auto terrain = restoredSession->AttachTerrain((*captured.value)[0].bytes);
            if (!terrain.Succeeded()) return terrain;
            const auto pathing = restoredSession->AttachPathing((*captured.value)[1].bytes);
            if (!pathing.Succeeded()) return pathing;
            if (job.pathingRestrictionDraft) {
                sourceTerrain = std::move((*captured.value)[0].bytes);
                sourcePathing = std::move((*captured.value)[1].bytes);
            }
        } // 在SetTerrain/Capture/Build之前释放本轮源raw临时缓冲。
        const MemberReplacement* terrainOutput = nullptr;
        const MemberReplacement* pathingOutput = nullptr;
        const std::array<std::pair<std::string_view, MapDataKind>, 7> editableMembers{{
            {"war3map.w3i", MapDataKind::Settings}, {"war3mapunits.doo", MapDataKind::Units},
            {"war3map.doo", MapDataKind::Doodads}, {"war3map.w3r", MapDataKind::Regions},
            {"war3map.w3c", MapDataKind::Cameras}, {"war3map.w3s", MapDataKind::Sounds}, {"war3map.wts", MapDataKind::Strings}}};
        for (const auto& member : job.replacements) {
            const auto key = MinimapMemberName(member.name);
            if (key == "war3map.w3e") {
                if (terrainOutput) return reject("TERRAIN_SAVE_OUTPUT_CONFLICT", "保存列表重复替换地形成员。");
                terrainOutput = &member;
            } else if (key == "war3map.wpm") {
                if (pathingOutput) return reject("TERRAIN_SAVE_OUTPUT_CONFLICT", "保存列表重复替换通行成员。");
                pathingOutput = &member;
            } else for (const auto& [name, kind] : editableMembers) if (key == name) {
                const auto restored = restoredSession->SetSerialized(kind, member.data);
                if (!restored.Succeeded()) return restored;
            }
        }
        if (terrainOutput && terrainOutput->data.size() > maximumW3e)
            return reject("MINIMAP_SAVE_BUDGET", "worker地形输入超过本次原始输入额度。");
        if (job.pathingRestrictionDraft) {
            const auto restored = restoredSession->SetTerrainAndPathingDraft(
                terrainOutput ? terrainOutput->data : sourceTerrain,
                pathingOutput ? pathingOutput->data : sourcePathing, *job.pathingRestrictionDraft);
            if (!restored.Succeeded()) return restored;
        } else if (terrainOutput) {
            const auto restored = pathingOutput ? restoredSession->SetTerrainAndPathing(terrainOutput->data, pathingOutput->data)
                                                : restoredSession->SetTerrain(terrainOutput->data);
            if (!restored.Succeeded()) return restored;
        } else if (pathingOutput) {
            const auto restored = restoredSession->SetPathing(pathingOutput->data);
            if (!restored.Succeeded()) return restored;
        }
        for (const auto& removed : job.removals) for (const auto& [name, kind] : editableMembers)
            if (MinimapMemberName(removed) == name) {
                const auto restored = restoredSession->Remove(kind);
                if (!restored.Succeeded()) return restored;
            }
        edits = std::move(restoredSession);
    }
    return Status::Ok();
}

Status PrepareWorkerFinalPathing(MapSaveJob& job,
    const std::shared_ptr<const MapData::MapDataEditSession>& frozenEdits) {
    if (!job.archive)
        return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_SOURCE_MISSING",
            "准备最终通行输入时缺少本次冻结地图来源，当前编辑保留。");
    auto prepared = Services::TerrainPathingSave::PrepareFinalForSave(*job.archive, frozenEdits.get(),
        job.minimapGameRoot, {}, job.replacements, job.removals);
    if (!prepared.Succeeded()) return {std::move(prepared.diagnostics)};
    if (!prepared.value)
        return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_RESULT_MISSING",
            "最终通行输入检查没有返回完整结果，当前编辑保留。");
    if (*prepared.value) {
        if (!frozenEdits)
            return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_BASELINE_MISSING",
                "通行重建缺少本次真实编辑会话快照，当前编辑保留。");
        auto& plan = **prepared.value;
        const auto installed = plan.InstallPathingOutput(*frozenEdits, job.replacements);
        if (!installed.Succeeded()) return installed;
        const auto verified = plan.VerifyForSave(*frozenEdits, job.sourceSha256, job.replacements, job.removals);
        if (!verified.Succeeded()) return verified;
        job.terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(plan));
        const auto& diagnostics = job.terrainSavePlan->Diagnostics();
        job.diagnostics.insert(job.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    } else {
        if (job.terrainDerivationRequired)
            return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_BASELINE_MISSING",
                "地形修改没有取得对应的最终通行计划，当前编辑保留。");
        // 无关物编/导入没有重建资格；写前仍须按真实来源和最终列表重查，不能裸放行。
        job.beforePublish = [&job, frozenEdits, previous = std::move(job.beforePublish)] {
            if (previous) { const auto checked = previous(); if (!checked.Succeeded()) return checked; }
            if (!job.archive)
                return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_SOURCE_MISSING",
                    "写入前缺少本次冻结地图来源，当前编辑保留。");
            return Services::TerrainPathingSave::VerifyUnchangedFinalInputs(*job.archive, frozenEdits.get(),
                job.minimapGameRoot, {}, job.replacements, job.removals);
        };
    }
    job.diagnostics.insert(job.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
    return Status::Ok();
}

Status PrepareWorkerDerivedInputs(MapSaveJob& job, std::shared_ptr<const MapData::MapDataEditSession>& edits,
    bool& pathingPrepared) {
    const auto reject = [](const char* code, const char* message) {
        return Status::Failure("TerrainMinimapSave.Qt", code, message);
    };
    const bool automatic = job.minimapChoice == Services::TerrainMinimapSaveChoice::Automatic;
    const bool preserve = job.minimapChoice == Services::TerrainMinimapSaveChoice::Preserve;
    const bool explicitGeneration = job.minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly
        || job.minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    if (!automatic && !preserve && !explicitGeneration)
        return reject("MINIMAP_SAVE_CHOICE", "未知的单次地形小地图保存选项。");
    if (job.rebuildTerrainShadow && explicitGeneration)
        return reject("TERRAIN_SHADOW_MINIMAP_CONFLICT", "显式计算阴影不能与旧的单独补建小地图选项混用。");
    if (!job.archive)
        return reject("TERRAIN_SAVE_PLAN_SOURCE_MISSING", "worker缺少本次冻结地图来源，当前编辑保留。");
    // 先核最终静态依赖，不能让作者小地图的保留策略决定是否更新WPM。
    const bool haveSession = job.terrainSessionSnapshot || job.terrainDerivationRequired
        || job.terrainSavePlan || explicitGeneration;
    if (haveSession) {
        const auto restored = RestoreWorkerTerrainSession(job, edits);
        if (!restored.Succeeded()) return restored;
    }
    const auto finalPathing = PrepareWorkerFinalPathing(job, haveSession ? edits : nullptr);
    if (!finalPathing.Succeeded()) return finalPathing;
    pathingPrepared = true;
    const auto prepareMarkersAndShadow = [&](const MapData::MapDataEditSession* currentEdits) {
        const auto markers = Services::PrepareTerrainMinimapMarkers(job, job.minimapGameRoot, currentEdits, job.terrainSavePlan);
        if (!markers.Succeeded() || !job.rebuildTerrainShadow) return markers;
        return Services::PrepareTerrainShadowBake(job, job.minimapGameRoot, currentEdits, job.terrainSavePlan);
    };
    if (automatic) {
        const auto required = Services::TerrainMinimapSave::RequiresAutomaticGeneration(job);
        if (!required.Succeeded() || !required.value) return {required.diagnostics};
        if (!*required.value && !job.terrainDerivationRequired) {
            // 静态通行已独立核对；无关物编/导入不额外取得地形能力门。
            auto prepared = Services::TerrainMinimapSave::PrepareAutomaticOrPreserve(job, *edits, job.minimapGameRoot);
            if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
            job.diagnostics.insert(job.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
            const auto installed = InstallPreservedMinimap(job, *edits, std::move(*prepared.value));
            if (!installed.Succeeded()) return installed;
            if (!job.minimapPreserve) job.minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
            return prepareMarkersAndShadow(job.terrainSavePlan ? edits.get() : nullptr);
        }
    }
    if (preserve && !job.terrainDerivationRequired)
        return prepareMarkersAndShadow(job.terrainSavePlan ? edits.get() : nullptr);
    if (!job.archive || (explicitGeneration && (job.inPlace || job.testCopyOnly || job.triggerChanges || !job.objectTransactions.empty()
        || !job.removals.empty() || std::count_if(job.replacements.begin(), job.replacements.end(), [](const auto& member) {
            return MinimapMemberName(member.name) == "war3map.w3e";
        }) != 1)))
        return reject("MINIMAP_SAVE_DOMAIN", "worker补建只接受真实独立另存任务与唯一高度W3E，拒绝测试副本或混合领域。");

    if (!haveSession) {
        const auto restored = RestoreWorkerTerrainSession(job, edits);
        if (!restored.Succeeded()) return restored;
    }
    if (!job.terrainSavePlan && (job.terrainDerivationRequired || automatic)) {
        auto prepared = Services::TerrainPathingSave::PrepareForSave(*job.archive, *edits, job.minimapGameRoot,
            {}, job.replacements, job.removals);
        if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
        const auto installed = prepared.value->InstallPathingOutput(*edits, job.replacements);
        if (!installed.Succeeded()) return installed;
        const auto verified = prepared.value->VerifyForSave(*edits, job.sourceSha256, job.replacements, job.removals);
        if (!verified.Succeeded()) return verified;
        job.terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*prepared.value));
        const auto& diagnostics = job.terrainSavePlan->Diagnostics();
        job.diagnostics.insert(job.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    }
    if (automatic || preserve) {
        const auto markers = prepareMarkersAndShadow(edits.get());
        if (!markers.Succeeded()) return markers;
    }
    if (preserve) return Status::Ok();
    std::shared_ptr<const Services::TerrainMinimapSave> minimap;
    if (automatic) {
        auto prepared = Services::TerrainMinimapSave::PrepareAutomaticOrPreserve(job, *edits, job.minimapGameRoot, {},
            job.terrainSavePlan);
        if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
        job.diagnostics.insert(job.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
        minimap = prepared.value->generated;
        if (!minimap) {
            const auto installed = InstallPreservedMinimap(job, *edits, std::move(*prepared.value));
            if (!installed.Succeeded()) return installed;
            if (!job.minimapPreserve) job.minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
            return Status::Ok();
        }
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

Status PrepareWorkerDerived(MapSaveJob& job) {
    // 若输入捕获本身超出生成器预算，仍由同一个能力分类决定能否保留。
    // 该临时会话只绑定本次 worker；编辑器代际仍由 Commit 的原门禁核对。
    std::shared_ptr<const MapData::MapDataEditSession> edits =
        std::make_shared<MapData::MapDataEditSession>(MapDocument::FromMembers({}));
    bool pathingPrepared = false;
    auto status = PrepareWorkerDerivedInputs(job, edits, pathingPrepared);
    if (!status.Succeeded()) {
        if (job.rebuildTerrainShadow && !job.terrainShadow) return status;
        // 小地图能力不足可以保留；静态依赖/通行计划没有通过时绝不能借此降级。
        if (job.minimapChoice != Services::TerrainMinimapSaveChoice::Automatic
            || !pathingPrepared) return status;
        auto preserved = Services::TerrainMinimapSave::PreserveUnsupportedAutomatic(job, *edits, std::move(status.diagnostics));
        if (!preserved.Succeeded() || !preserved.value) return {std::move(preserved.diagnostics)};
        const auto installed = InstallPreservedMinimap(job, *edits, std::move(*preserved.value));
        if (!installed.Succeeded()) return installed;
        status = {std::move(preserved.diagnostics)};
    }
    if (job.terrainSavePlan) {
        job.beforePublish = [&job, previous = std::move(job.beforePublish)] {
            if (previous) { const auto checked = previous(); if (!checked.Succeeded()) return checked; }
            if (!job.archive)
                return Status::Failure("Terrain.SavePlan", "TERRAIN_SAVE_PLAN_SOURCE_MISSING",
                    "写入前缺少本次地形计划的原始地图来源，当前编辑保留。");
            // 旧通行只能由真实旧来源解释；最终覆盖及回读另由新资源核对。
            // 此处位于原位替换之前，不能移到写回后的提交阶段。
            const auto source = job.terrainSavePlan->VerifySourceInputs(*job.archive);
            if (!source.Succeeded()) return source;
            return job.terrainSavePlan->VerifyFrozenOutput(job.sourceSha256, job.replacements, job.removals);
        };
    }
    return status;
}

void BindWorkerTerrainHistory(MapSaveJob& job) {
    if (!job.terrainHistorySaveLease) return;
    const auto warn = [&](std::string_view code) {
        job.terrainHistorySaveLease.reset();
        job.terrainHistoryWarning = "当前编辑仍保留；地图保存继续，但旧地形阴影未能绑定到历史保存站，跨保存点撤销的冷恢复不可用。";
        if (!code.empty()) job.terrainHistoryWarning += "（" + std::string(code) + "）";
    };
    try {
        if (!job.archive) { warn("TERRAIN_HISTORY_SOURCE_MISSING"); return; }
        // 此刻仍是写入前的真实旧归档；不得从最终覆盖列表或保存后新来源
        // 补造旧SHD。helper保留已绑定的最早epoch，不重复读取最新SHD。
        auto bound = Services::TerrainHistorySave::BindSourceShadow(*job.archive, *job.terrainHistorySaveLease);
        if (!bound.Succeeded() || !bound.value) {
            warn(bound.diagnostics.empty() ? std::string_view{} : bound.diagnostics.front().code);
            return;
        }
        job.terrainHistorySaveLease = std::move(*bound.value);
    } catch (...) {
        warn("TERRAIN_HISTORY_SOURCE_BIND_EXCEPTION");
    }
}

} // namespace

void InstallMapSaveDerivedPreparation(MapSaveJob& job) {
    if (job.derivedPreparationInstalled) return;
    job.derivedPreparationInstalled = true;
    job.prepareFinalDerived = [&job, previous = std::move(job.prepareFinalDerived)](Services::MapSaveTask& task) -> Status {
        if (previous) { const auto checked = previous(task); if (!checked.Succeeded()) return checked; }
        BindWorkerTerrainHistory(job);
        try {
            job.Report(job.rebuildTerrainShadow ? EditorProject::tr("正在更新地形通行、小地图与阴影…")
                : EditorProject::tr("正在更新地形通行与小地图…"));
        } catch (...) {}
        try {
            auto derived = PrepareWorkerDerived(job);
            if (derived.Succeeded() && job.terrainHistorySaveLease && job.terrainShadow
                && (!job.terrainSessionSnapshot || !job.terrainSessionSnapshot->HasTerrainTypedResizeState())) {
                // 普通地形历史没有可重放的Bake命令，不能把旧SHD绑定冒充
                // 本次已重算的保存站。只消费实际成功owner，不凭请求开关降级。
                job.terrainHistorySaveLease.reset();
                job.terrainHistoryWarning = "地图已按请求重算阴影；这次普通保存包含尚不能重放的阴影计算，未保留跨保存点冷撤销历史，当前编辑和地图保存不受影响。";
            }
            return derived;
        } catch (...) {
            return Status::Failure("TerrainMinimapSave.Qt", "MINIMAP_SAVE_WORKER_EXCEPTION",
                "worker准备地形派生时发生异常，本次任务已终止；未接受保存结果，当前编辑和原图保留。");
        }
    };
}

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
    InstallMapSaveDerivedPreparation(job);
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
    if (job.terrainDerivationRequired && !job.terrainSavePlan)
        return rejectStaleCommit(tr("TERRAIN_SAVE_PLAN_READBACK_MISSING"), tr("本次地形保存缺少实际派生计划"));
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
    // 先准备会分配内存的新文档和来源路径，地形基线提交后只交换现成对象。
    // 会话本身不可复制或换址，Core在原会话内原子提交并保留撤销回执身份。
    std::unique_ptr<MapDocument> preparedDocument;
    std::filesystem::path nextSourcePath, nextLuaSource;
    try {
        nextSourcePath = job.written;
        if (luaMapTarget_) nextLuaSource = job.written;
        if (job.terrainSavePlan) {
            if (!mapEdits_)
                return rejectStaleCommit(tr("TERRAIN_PATHING_COMMIT_STALE"), tr("前台地形会话已失效"));
            preparedDocument = std::make_unique<MapDocument>(MapDocument::FromArchive(*job.nextSession));
        }
    } catch (...) {
        return rejectStaleCommit(tr("TERRAIN_PATHING_COMMIT_PREPARATION"), tr("无法准备保存后的地图文档，前台编辑基线未改变"));
    }

    MapCopySaveResult result;
    result.target = job.written;
    result.memberCount = job.memberCount;
    result.savedTriggerChanges = job.triggerChanges && !job.scriptRebuilt;
    result.scriptRebuilt = job.scriptRebuilt;
    result.scriptPreserved = job.scriptPreserved;
    result.inPlace = job.inPlace;
    result.backup = job.backup;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
    if (preparedDocument) {
        const auto committed = job.terrainSavePlan->CommitSaved(*mapEdits_, *preparedDocument);
        if (!committed.Succeeded()) {
            AddDiagnostics(committed.diagnostics);
            return rejectStaleCommit(tr("TERRAIN_PATHING_COMMIT_STALE"), tr("无法将实际通行输出绑定到前台编辑基线"));
        }
    }
    // 唯一提交点。后续保存以新文件为来源，包含此前已保存的物编及导入资源。
    if (job.inPlace) {
        // 写回替换了被监视的源文件；这是自己的操作，不提示“来源已变化”。
        sourceWatchQuietUntil_ = QDateTime::currentMSecsSinceEpoch() + 3000;
        sourceChangeSuspected_ = false;
        sourceChangeConfirmed_ = false;
    }
    archiveSession_ = std::move(job.nextSession);
    readOnlyArchive_.reset();
    sourceArchivePath_.swap(nextSourcePath);
    if (luaMapTarget_) luaMapTargetSource_.swap(nextLuaSource);
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
    if (preparedDocument) {
        mapDocument_ = std::move(preparedDocument);
        for (const auto& diagnostic : mapDocument_->Diagnostics())
            AddDiagnostic(diagnostic.severity == DiagnosticSeverity::Error ? tr("错误") : tr("警告"), tr("地图数据"),
                QString::fromUtf8(diagnostic.code), QString::fromUtf8(diagnostic.message));
        emit MapDocumentChanged();
    } else LoadMapDocument(true);
    if (job.terrainHistorySaveLease && mapEdits_) {
        try {
            const auto history = mapEdits_->CommitTerrainHistorySave(*job.terrainHistorySaveLease);
            if (!history.Succeeded()) {
                job.terrainHistoryWarning = "草稿已完整保留；地图已保存，但地形历史保存站安装失败，跨保存点撤销的冷恢复不可用。";
                if (!history.diagnostics.empty()) job.terrainHistoryWarning += "（" + history.diagnostics.front().code + "）";
            }
        } catch (const std::exception&) {
            job.terrainHistoryWarning = "草稿已完整保留；地图已保存，但地形历史保存站安装超出可用预算，跨保存点撤销的冷恢复不可用。";
        }
    }
    if (!job.terrainHistoryWarning.empty()) {
        if (terrainJournalWarning_.empty()) terrainJournalWarning_ = job.terrainHistoryWarning;
        auto warning = Status::Failure("Terrain.History", "TERRAIN_HISTORY_DRAFT_ONLY", job.terrainHistoryWarning);
        for (auto& diagnostic : warning.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
        AddDiagnostics(warning.diagnostics);
    }
    restoredTriggerOverlay_ = false;
    triggerDirty_ = false;
    objectDirty_.fill(false);
    pendingImports_.clear();
    assetEdits_.clear();
    NotifyImportsChanged();
    if (triggerUndoStack_) triggerUndoStack_->setClean();
    if (mapDataUndoStack_) mapDataUndoStack_->setClean();
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
