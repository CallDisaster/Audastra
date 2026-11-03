#include "EditorProject.h"
#include "MapSaveJob.h"
#include "MapSavePreferences.h"
#include "PendingImportPayload.h"
#include "ProjectObjectCatalog.h"

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <cctype>
#include <new>
#include <set>
#include <stdexcept>

// 地图保存的准备阶段（界面线程）：以源归档快照为基础，序列化本次修改的成员、编辑导入清单，
// 决定是否重建游戏脚本，生成保存任务。执行与提交见 MapSaveRun.cpp。
namespace War3Map::QtProject {
namespace {

constexpr std::array kObjectTypes{
    ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability, ObjectData::ObjectType::Item,
    ObjectData::ObjectType::Buff, ObjectData::ObjectType::Upgrade, ObjectData::ObjectType::Doodad,
    ObjectData::ObjectType::Destructable,
};

std::string CanonicalMember(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
        return character == '\\' ? '/' : static_cast<char>(std::tolower(character));
    });
    return name;
}

Result<ObjectData::LniObjectDocument> ObjectWriteDocument(ObjectData::ObjectType type,
    const std::optional<std::vector<std::byte>>& native) {
    std::vector<std::byte> text;
    if (native) {
        auto converted = ObjectData::ConvertNativeObjectDataToLni(type, *native);
        if (!converted.Succeeded()) return {{}, converted.diagnostics};
        text = std::move(*converted.value);
    }
    auto parsed = ObjectData::LniObjectCodec(type).Parse(text);
    const auto* document = parsed.Succeeded() ? dynamic_cast<const ObjectData::LniObjectDocument*>(parsed.document.get()) : nullptr;
    if (!document || !document->IsStructurallySafe()) {
        if (!parsed.diagnostics.empty()) return {{}, parsed.diagnostics};
        return Result<ObjectData::LniObjectDocument>::Failure("Object.Save", "OBJECT_WRITE_BASELINE_INVALID",
            "保存写入资格没有完整的原生/LNI前后文档。");
    }
    return Result<ObjectData::LniObjectDocument>::Success(*document);
}

Result<std::optional<std::vector<std::byte>>> ObjectWriteNative(const MapArchiveSession& archive,
    ObjectData::ObjectType type) {
    using Output = Result<std::optional<std::vector<std::byte>>>;
    const auto name = ObjectData::NativeFileName(type);
    if (archive.HasMember(name)) {
        auto read = archive.ReadMember(name);
        if (!read.Succeeded()) return {{}, read.diagnostics};
        return Output::Success(std::move(*read.value));
    }
    // 命名快照缺项不证明匿名块缺失；最终模板可涉及此前未命名的native成员。
    const std::vector<std::string> names{std::string(name)};
    auto exact = archive.CaptureExactMembers(names);
    if (!exact.Succeeded()) return {{}, exact.diagnostics};
    if (!exact.value->front().present) return Output::Success(std::nullopt);
    return Output::Success(std::move(exact.value->front().bytes));
}

// 后台只读取任务自己拥有的归档、最终成员与冻结来源，不借用Project或CampaignSession。
// 模板也可能改变native/SLK；发布前按最终实际差异再核全部类别。
Status VerifyFrozenObjectWriteSources(const Services::MapSaveTask& task,
    const ObjectData::EffectiveObjectCatalog& captured, const std::filesystem::path& gameRoot) {
    if (!task.archive) return Status::Failure("Object.Save", "OBJECT_WRITE_BASELINE_INVALID", "保存写入资格缺少冻结归档。");
    if (const auto status = captured.VerifyCapturedSources(); !status.Succeeded()) return status;
    Services::ObjectCatalogSources sources;
    sources.archive = task.archive.get(); sources.gameRoot = gameRoot;
    sources.replacements = task.replacements; sources.removals = task.removals;
    sources.overlayIdentity = task.sourceSha256;
    auto current = Services::LoadEffectiveObjectCatalog(sources);
    if (!current.Succeeded()) return {current.diagnostics};
    for (const auto type : kObjectTypes) {
        const auto member = CanonicalMember(std::string(ObjectData::NativeFileName(type)));
        const MemberReplacement* replacement = nullptr;
        for (const auto& input : task.replacements) {
            if (CanonicalMember(input.name) != member) continue;
            if (replacement) return Status::Failure("Object.Save", "OBJECT_WRITE_OUTPUT_CONFLICT", "最终物编成员有重复覆盖。");
            replacement = &input;
        }
        const bool removed = std::any_of(task.removals.begin(), task.removals.end(), [&](const auto& name) {
            return CanonicalMember(name) == member;
        });
        if (!replacement && !removed) continue;
        if (replacement && removed) return Status::Failure("Object.Save", "OBJECT_WRITE_OUTPUT_CONFLICT", "最终物编成员同时删除和覆盖。");
        auto original = ObjectWriteNative(*task.archive, type);
        if (!original.Succeeded()) return {original.diagnostics};
        const auto finalBytes = replacement ? std::make_optional(replacement->data) : std::nullopt;
        if (*original.value == finalBytes) continue; // 未改变的未知字节仍按原保存链透传。
        auto before = ObjectWriteDocument(type, *original.value);
        auto after = ObjectWriteDocument(type, finalBytes);
        if (!before.Succeeded()) return {before.diagnostics};
        if (!after.Succeeded()) return {after.diagnostics};
        Services::ObjectCommandContext context;
        context.type = type; context.standard = &current.value->Catalog();
        const auto status = Services::VerifyObjectDocumentWriteSources(context, *before.value, *after.value);
        if (!status.Succeeded()) return status;
    }
    return current.value->VerifyCapturedSources();
}

// 编辑器自己管理的成员不能被导入资源覆盖。
bool IsManagedMember(const std::string& key) {
    static const std::array<std::string_view, 17> kManaged{
        "war3map.wtg", "war3map.wct", "war3map.imp", "war3map.j", "war3map.lua",
        "scripts/war3map.j", "scripts/war3map.lua", "(listfile)",
        "war3map.w3i", "war3mapunits.doo", "war3map.doo", "war3map.w3r",
        "war3map.w3c", "war3map.w3s", "war3map.wts", "war3map.w3e", "war3map.wpm"};
    if (std::find(kManaged.begin(), kManaged.end(), key) != kManaged.end()) {
        return true;
    }
    return std::any_of(kObjectTypes.begin(), kObjectTypes.end(),
        [&](const auto type) { return key == ObjectData::NativeFileName(type); });
}

// 这里只确定保存依赖的候选范围，资格仍由公共resolver给出。
// 禁用T/禁用子树不生成脚本，不把它们保留的旧对象名当成本轮运行依赖。
Result<std::vector<std::string>> ActivePreplacedNames(const Trigger::TriggerDocument& document) {
    using Output = Result<std::vector<std::string>>;
    std::vector<std::pair<bool, std::uint32_t>> pending;
    std::vector<bool> ecas(document.ecaNodes.size()), arguments(document.argumentNodes.size());
    std::set<std::string> names;
    for (const auto& trigger : document.triggers)
        if (trigger.type == 0 && trigger.enabled != 0)
            for (const auto index : trigger.ecas) pending.emplace_back(false, index);
    while (!pending.empty()) {
        const auto [argumentNode, index] = pending.back();
        pending.pop_back();
        if (argumentNode) {
            if (index >= document.argumentNodes.size())
                return Output::Failure("Trigger.MapObjects", "MAP_OBJECT_REFERENCE_GRAPH_INVALID", "运行参数索引越界。");
            if (arguments[index]) continue;
            arguments[index] = true;
            const auto& argument = document.argumentNodes[index];
            if (argument.type == 1 && (argument.value.text.starts_with("gg_unit_") || argument.value.text.starts_with("gg_item_")))
                names.insert(argument.value.text);
            if (argument.callEca) pending.emplace_back(false, *argument.callEca);
            if (argument.indexArgument) pending.emplace_back(true, *argument.indexArgument);
        } else {
            if (index >= document.ecaNodes.size())
                return Output::Failure("Trigger.MapObjects", "MAP_OBJECT_REFERENCE_GRAPH_INVALID", "运行ECA索引越界。");
            if (ecas[index]) continue;
            ecas[index] = true;
            const auto& eca = document.ecaNodes[index];
            if (!eca.enabled) continue;
            for (const auto child : eca.children) pending.emplace_back(false, child);
            for (const auto child : eca.arguments) pending.emplace_back(true, child);
        }
    }
    return Output::Success({names.begin(), names.end()});
}

} // namespace

bool EditorProject::MapSourceIsRecoveryCopy() const {
    if (!restoredFiles_ || sourceArchivePath_.empty()) return false;
    const auto root = std::filesystem::path(restoredFiles_->path().toStdWString()).lexically_normal();
    const auto relative = sourceArchivePath_.lexically_normal().lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

std::filesystem::path EditorProject::AuthorMapPath() const {
    return MapSourceIsRecoveryCopy() ? recordedSourceMapPath_ : sourceArchivePath_;
}

bool EditorProject::HasUnsavedMapChanges() const {
    // 待提交操作也不能让测试入口误用旧地图；它仍不冒充已提交的修订。
    if (HasPendingTerrainCommit()) return true;
    // 标题/退出使用同一可信Map修订；相同raw字节仍可能有待保存的可撤销前缀。
    // 不能在Ctrl+S入口将其误判为空而使脏状态永远无法提交。
    if (editTrackingReady_ && !editTrackingFailed_ && editRevision_.IsDirty(EditSaveTarget::Map)) return true;
    return triggerDirty_ || HasPendingAssetEdits() || (mapEdits_ && mapEdits_->IsDirty())
        || std::any_of(objectDirty_.begin(), objectDirty_.end(), [](bool dirty) { return dirty; });
}

std::unique_ptr<MapSaveJob> EditorProject::PrepareMapSave(const std::filesystem::path& outputPath, bool inPlace,
    Services::TerrainMinimapSaveChoice minimapChoice) {
    return PrepareMapSaveImpl(outputPath, inPlace, false, minimapChoice);
}

std::unique_ptr<MapSaveJob> EditorProject::PrepareMapTestCopy(const std::filesystem::path& target,
    Services::MapRuntimeSavePurpose purpose) {
    if (purpose == Services::MapRuntimeSavePurpose::Normal) {
        AddDiagnostic(tr("错误"), tr("地图"), tr("RUNTIME_SAVE_PURPOSE_INVALID"), tr("测试副本必须明确选择测试运行或内部验收用途。"));
        return nullptr;
    }
    return PrepareMapSaveImpl(target, false, false, Services::TerrainMinimapSaveChoice::Automatic, purpose);
}

std::unique_ptr<MapSaveJob> EditorProject::PrepareMapPreflight(Services::MapRuntimeSavePurpose purpose) {
    return PrepareMapSaveImpl({}, false, true, Services::TerrainMinimapSaveChoice::Automatic, purpose);
}

std::unique_ptr<MapSaveJob> EditorProject::PrepareMapSaveImpl(
    const std::filesystem::path& outputPath, bool inPlace, bool contentOnly,
    Services::TerrainMinimapSaveChoice minimapChoice, Services::MapRuntimeSavePurpose purpose) {
    const auto fail = [this](const QString& code, const QString& message) {
        AddDiagnostic(tr("错误"), tr("地图"), code, message);
        return std::unique_ptr<MapSaveJob>{};
    };
    if (HasPendingTerrainCommit())
        return fail(tr("TERRAIN_COMMIT_PENDING"), tr("地形操作正在核对并提交，请完成或取消后再保存地图；尚未冻结保存内容。"));
    if (purpose != Services::MapRuntimeSavePurpose::Normal && purpose != Services::MapRuntimeSavePurpose::TestRun
        && purpose != Services::MapRuntimeSavePurpose::InternalValidation)
        return fail(tr("RUNTIME_SAVE_PURPOSE_INVALID"), tr("未知保存用途不能授予内部包使用资格。"));
    const bool explicitMinimap = minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly
        || minimapChoice == Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    const bool automaticMinimap = minimapChoice == Services::TerrainMinimapSaveChoice::Automatic;
    // 正常保存只把策略交给worker；缺失、TGA、实际依赖变化均在真实冻结
    // 任务上准确判断，界面线程不读贴图、不生成小地图。
    const bool generateMinimap = explicitMinimap || (automaticMinimap && !contentOnly);
    if (minimapChoice != Services::TerrainMinimapSaveChoice::Preserve && !generateMinimap && !automaticMinimap)
        return fail(tr("MINIMAP_SAVE_CHOICE"), tr("未知的地形小地图保存选项。"));
    if (explicitMinimap) {
        if (inPlace || contentOnly || campaignScope_)
            return fail(tr("MINIMAP_SAVE_TARGET"), tr("补建缺失的地形小地图仅用于本次独立另存；原地保存、预检和战役不支持。"));
        if (!mapEdits_ || !MapData::TerrainSavePlan::HasTerrainChanges(*mapEdits_)
            || triggerDirty_ || luaMapTarget_ || !pendingImports_.empty() || !assetEdits_.empty()
            || std::any_of(objectDirty_.begin(), objectDirty_.end(), [](bool dirty) { return dirty; }))
            return fail(tr("MINIMAP_SAVE_DOMAIN"), tr("本次补建只允许已有地图的纯高度净变化；请先单独处理触发器、物编、资源和脚本档案变化。"));
    }
    // 作者手写的 Lua 文件按普通导入资源写入地图（YDWE 约定：由 Cheat("exec-lua: ...") 加载）；
    // 写入前用内置 Lua 5.3（含 YDWE |xxxx| 四字码）检查语法，有错误时不保存并指出文件与行。
    for (const auto& pending : pendingImports_) {
        const auto path = CanonicalMember(pending.archivePath.toStdString());
        if (!(pending.runtimeLuaDraft || path.ends_with(".lua"))) continue;
        std::string text;
        if (pending.ownedPayload) {
            text.assign(reinterpret_cast<const char*>(pending.ownedPayload->data()), pending.ownedPayload->size());
        } else {
            QFile disk(pending.diskPath);
            if (!disk.open(QIODevice::ReadOnly) || disk.size() > 16 * 1024 * 1024)
                return fail(tr("MAP_LUA_READ"), tr("无法读取待导入的 Lua 文件 %1（或超过 16 MiB），地图未保存。").arg(pending.diskPath));
            const auto bytes = disk.readAll();
            text.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
        const auto checked = Lua::CheckSyntax(text, "@" + pending.archivePath.toStdString());
        if (checked.Succeeded()) continue;
        const auto& first = checked.diagnostics.empty() ? Lua::SyntaxDiagnostic{"LUA_SYNTAX", "语法检查未完成。", 0}
            : checked.diagnostics.front();
        return fail(tr("MAP_LUA_SYNTAX"), tr("Lua 文件 %1 第 %2 行有语法错误，地图未保存：%3")
            .arg(pending.archivePath).arg(first.line).arg(QString::fromStdString(first.message)));
    }
    if (sourceArchivePath_.empty()) {
        return fail(tr("REPACK_NO_SOURCE"), tr("当前工程没有源地图归档。"));
    }
    if (!contentOnly && !inPlace && outputPath.empty()) {
        return nullptr;
    }
    if (inPlace && MapSourceIsRecoveryCopy()) {
        return fail(tr("MAP_SAVE_TARGET_RECOVERY_COPY"), tr("当前地图来自恢复工程的临时副本（原地图已变化或找不到），"
            "“保存地图”不会写回这里。请用“另存为”保存到地图文件夹；当前编辑保持不变。"));
    }
    std::error_code pathError;
    const auto target = contentOnly ? std::filesystem::path{} : inPlace ? sourceArchivePath_
        : std::filesystem::absolute(outputPath, pathError).lexically_normal();
    if (pathError) return fail(tr("REPACK_TARGET_INVALID"), tr("无法解析输出路径。"));
    if (purpose != Services::MapRuntimeSavePurpose::Normal && !contentOnly) {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(target, existsError);
        if (inPlace || target == sourceArchivePath_.lexically_normal() || exists || existsError)
            return fail(tr("RUNTIME_INTERNAL_COPY_REQUIRED"), tr("测试运行或内部验收必须使用独立且不存在的新副本路径，不能写回或覆盖地图。"));
    }
    if (!archiveSession_) {
        return fail(tr("REPACK_SOURCE_SNAPSHOT_MISSING"), tr("源归档未能完整建立快照，不能保存不完整副本。"));
    }
    std::error_code sourceError;
    if (!std::filesystem::equivalent(sourceArchivePath_, archiveSession_->SourcePath(), sourceError) || sourceError) {
        return fail(tr("REPACK_SOURCE_MISMATCH"), tr("地图来源不可达或与当前归档快照不一致。"));
    }
    // 源图未被外部修改的完整核对（散列整个归档）放在执行阶段：写出候选前与替换前各核对一次。
    if (!ObserveEditState("另存地图前核对")) {
        return fail(tr("SAVE_REVISION_UNAVAILABLE"), tr("无法确认编辑修订和导入文件快照；地图保存已中止"));
    }
    const auto frozenGeneration = editRevision_.Generation();
    const auto frozenRevision = editRevision_.Revision();
    const auto frozenScopes = editRevision_.Current();
    if (editRevision_.Generation() != frozenGeneration || editRevision_.Revision() != frozenRevision) {
        return fail(tr("SAVE_REVISION_CHANGED"), tr("准备保存时编辑修订发生变化，地图保存已中止。"));
    }
    auto job = std::make_unique<MapSaveJob>();
    job->runtimePurpose = purpose;
    job->testCopyOnly = purpose != Services::MapRuntimeSavePurpose::Normal;
    job->runtimePackageSources = runtimePackageSources_;
    job->runtimeCatalogDiagnostics = runtimeCatalogDiagnostics_;
    job->runtimeApprovedPackageSha256 = runtimeApprovedPackageSha256_;
    job->target = target;
    job->inPlace = inPlace;
    // 正常保存将自动小地图策略冻结给worker；内容预检只重建通行，
    // 是否需要生成小地图由最终来源和成员列表判定。
    job->minimapChoice = generateMinimap ? minimapChoice : Services::TerrainMinimapSaveChoice::Preserve;
    job->minimapGameRoot = war3GameRoot_;
    if (inPlace) {
        const auto options = ReadMapInPlaceSaveOptions();
        if (!options.Succeeded()) {
            AddDiagnostics(options.diagnostics);
            return nullptr;
        }
        job->inPlaceOptions = *options.value;
    }
    // 后台保存不借用可能被工程切换释放的源快照。冻结失败只转存返回诊断一次。
    const auto frozenArchive = job->FreezeArchive(*archiveSession_);
    if (!frozenArchive.Succeeded()) {
        AddDiagnostics(frozenArchive.diagnostics);
        return nullptr;
    }
    if (mapEdits_) {
        // 打开时已附入真实W3E/WPM。净地形不变的物编/导入也需要同一会话事实，
        // 是否重建由最终worker的实际依赖差异决定；冻结本身不检查占地能力。
        // 沿用现有单次快照（不复制Undo历史），不为无关修改新增64MiB能力门。
        try { job->terrainSessionSnapshot = mapEdits_->FreezeForSave(); }
        catch (const std::bad_alloc&) {
            return fail(tr("MAP_SAVE_SNAPSHOT_ALLOCATION"), tr("无法冻结本次地图编辑快照，当前编辑保留。"));
        } catch (const std::length_error&) {
            return fail(tr("MAP_SAVE_SNAPSHOT_ALLOCATION"), tr("地图编辑快照超出可分配容量，当前编辑保留。"));
        }
        try {
            auto history = mapEdits_->PrepareTerrainHistoryForSave();
            if (history.Succeeded() && history.value) {
                job->terrainHistorySaveLease = std::move(*history.value);
            } else if (history.diagnostics.empty() || !std::all_of(history.diagnostics.begin(), history.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "TERRAIN_HISTORY_NOT_AVAILABLE";
            })) {
                job->terrainHistoryWarning = "草稿已完整保留；当前地形历史不能绑定到一座保存站，跨保存点撤销的冷恢复不可用。";
                if (!history.diagnostics.empty()) job->terrainHistoryWarning += "（" + history.diagnostics.front().code + "）";
            }
        } catch (const std::exception&) {
            job->terrainHistoryWarning = "草稿已完整保留；地形历史冻结超出可用预算，跨保存点撤销的冷恢复不可用。";
        }
    }
    job->triggerChanges = triggerDirty_;
    auto& replacements = job->replacements;
    const auto replaceMember = [&replacements](const std::string& name, std::vector<std::byte> bytes) {
        replacements.push_back({name, std::move(bytes)});
    };

    // 任务内的副本承接全部回灌，失败时原基线（包括修改标志与源字节）保持不动。
    auto& nextBaselines = job->nextBaselines;
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        if (nativeObjectDocuments_[index]) {
            nextBaselines[index] = std::make_unique<ObjectData::NativeObjectDocument>(*nativeObjectDocuments_[index]);
        }
    }
    std::vector<MapArchiveMember> auxiliarySaveMembers;
    if (mapEdits_ && mapEdits_->IsDirty()) {
        job->terrainDerivationRequired = !explicitMinimap && MapData::TerrainSavePlan::HasTerrainChanges(*mapEdits_);
        // 原始成员与APR完整意图随任务冻结；worker在脚本/模板最终列表形成后
        // 才建立地形计划。这里只准备输入，不把尚未执行的通行门标记为通过。
        auto restrictions = mapEdits_->SerializePathingRestrictionDraft();
        AddDiagnostics(restrictions.diagnostics);
        if (!restrictions.Succeeded() || !restrictions.value) return nullptr;
        if (!restrictions.value->empty())
            job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(std::move(*restrictions.value));
        for (auto& member : mapEdits_->Replacements()) {
            const auto key = CanonicalMember(member.name);
            if (key == "war3map.w3r" || key == "war3map.w3c") auxiliarySaveMembers.push_back(member);
            replaceMember(member.name, std::move(member.data));
        }
        const auto removed = mapEdits_->Removals();
        job->removals.insert(job->removals.end(), removed.begin(), removed.end());
    }
    if (explicitMinimap && (!job->removals.empty() || job->replacements.size() != 1
        || CanonicalMember(job->replacements.front().name) != "war3map.w3e"))
        return fail(tr("MINIMAP_SAVE_DOMAIN"), tr("补建准备只能包含唯一的高度W3E输出，不能混入其它地图数据。"));
    if (triggerDirty_) {
        if (!triggerDocument_ || !uiCatalog_) {
            return fail(tr("REPACK_TRIGGER_STATE_INVALID"), tr("触发器存在修改，但文档或函数目录不可用。"));
        }
        auto encoded = Trigger::WtgWctCodec{}.Serialize(*triggerDocument_, *uiCatalog_);
        AddDiagnostics(encoded.diagnostics);
        if (!encoded.Succeeded() || !encoded.value) {
            return fail(tr("REPACK_TRIGGER_REJECTED"), tr("触发器序列化未通过，已中止另存。"));
        }
        replaceMember("war3map.wtg", std::move(encoded.value->wtg));
        replaceMember("war3map.wct", std::move(encoded.value->wct));
    }
    std::shared_ptr<const ObjectData::EffectiveObjectCatalog> objectWriteCatalog;
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        if (!objectDirty_[index]) continue;
        const auto type = kObjectTypes[index];
        if (!objectDocuments_[index] || !nextBaselines[index]) {
            return fail(tr("REPACK_NATIVE_BASELINE_MISSING"), tr("类型 %1 有修改，但缺少物编文档或 native 基线。")
                .arg(QString::fromUtf8(std::string(ObjectData::ToString(type)))));
        }
        // 尚未保存的创建全部撤销后，不向源图原本缺少的类别写入空成员。
        if (objectDocuments_[index]->Objects().empty() && !archiveSession_->HasMember(ObjectData::NativeFileName(type))) {
            continue;
        }
        auto original = ObjectWriteNative(*job->archive, type);
        if (!original.Succeeded()) { AddDiagnostics(original.diagnostics); return nullptr; }
        auto before = ObjectWriteDocument(type, *original.value);
        if (!before.Succeeded()) { AddDiagnostics(before.diagnostics); return nullptr; }
        if (!ObjectData::DiffObjects(before.value->Objects(), objectDocuments_[index]->Objects()).Empty()) {
            if (!objectWriteCatalog) {
                const auto ready = EnsureProjectObjectCatalog(*this);
                AddDiagnostics(ready.diagnostics);
                if (!ready.Succeeded()) return nullptr;
                const auto verified = VerifyProjectObjectCatalog(*this);
                AddDiagnostics(verified.diagnostics);
                if (!verified.Succeeded()) return nullptr;
                objectWriteCatalog = ProjectEffectiveObjectCatalog(*this);
                if (!objectWriteCatalog) return fail(tr("OBJECT_CATALOG_UNAVAILABLE"), tr("保存缺少可冻结的有效物编来源。"));
            }
            Services::ObjectCommandContext context;
            context.type = type; context.standard = &objectWriteCatalog->Catalog();
            const auto writable = Services::VerifyObjectDocumentWriteSources(context, *before.value, *objectDocuments_[index]);
            AddDiagnostics(writable.diagnostics);
            if (!writable.Succeeded()) return nullptr;
        }
        const auto applied = ObjectData::ApplyLniChangesToNative(
            *nextBaselines[index], *objectDocuments_[index], objectFieldSchema_.get(), standardCatalog_.get());
        AddDiagnostics(applied.diagnostics);
        if (!applied.Succeeded()) {
            return fail(tr("REPACK_OBJECT_REJECTED"), tr("物体数据回灌未通过，已中止另存。"));
        }
        auto encoded = ObjectData::NativeObjectCodec(type).Serialize(*nextBaselines[index]);
        AddDiagnostics(encoded.diagnostics);
        if (!encoded.Succeeded() || !encoded.value) {
            return fail(tr("REPACK_OBJECT_ENCODE_FAILED"), tr("物体数据序列化未通过，已中止另存。"));
        }
        replaceMember(std::string(ObjectData::NativeFileName(type)), std::move(*encoded.value));
    }

    auto importTable = Resources::ImportTable::CreateEmpty();
    QStringList missingDisabledIcons = MissingDisabledIcons();
    if (explicitMinimap && !missingDisabledIcons.isEmpty())
        return fail(tr("MINIMAP_SAVE_DOMAIN"), tr("本次保存还需要补建暂停图标，不能与地形小地图补建混用；请先普通另存，再单独修改高度。"));
    if (!missingDisabledIcons.isEmpty() && archiveSession_->HasOpaqueMembers()) {
        // 匿名成员的归档不能安全新增文件；普通保存也不能因此附带未请求的资源修改。
        // 在计算导入修改前跳过自动补图标，保留显式导入和新增成员的原有保存门禁。
        AddDiagnostic(tr("警告"), tr("资源"), tr("DISABLED_ICON_NOT_GENERATED"),
            tr("源地图含匿名成员，未自动新增 %1 个缺失的暂停图标；已有资源和导入清单保持原状。")
                .arg(missingDisabledIcons.size()));
        missingDisabledIcons.clear();
    }
    const bool editsImports = !pendingImports_.empty() || !assetEdits_.empty() || !missingDisabledIcons.isEmpty();
    if (editsImports && archiveSession_->HasMember("war3map.imp")) {
        const auto bytes = archiveSession_->ReadMember("war3map.imp");
        auto parsed = Resources::ImportTable::Parse(*bytes.value);
        AddDiagnostics(parsed.diagnostics);
        if (!parsed.Succeeded() || !parsed.value) return fail(tr("IMPORT_TABLE_INVALID"), tr("原有导入清单无法安全编辑"));
        importTable = std::move(*parsed.value);
    }
    // 已有成员的改名 / 移动 / 删除：新路径写入原字节，旧路径不写入候选；导入清单同步。
    auto& removals = job->removals;
    const auto memberExists = [this](std::string_view path) { return archiveSession_->HasMember(path); };
    const auto effectiveAssets = AssetEntries();
    if (std::any_of(effectiveAssets.begin(), effectiveAssets.end(), [](const auto& entry) { return entry.ambiguous; })) {
        return fail(tr("ASSET_SOURCE_AMBIGUOUS"), tr("资源的规范路径具有多份来源，不能选择其中一份保存"));
    }
    std::set<std::string> pendingTargets;
    for (const auto& pending : pendingImports_) {
        if (!pendingTargets.insert(CanonicalMember(pending.archivePath.toStdString())).second) {
            return fail(tr("IMPORT_SOURCE_AMBIGUOUS"), tr("待导入资源含重复的规范路径，不能选择其中一份保存"));
        }
    }
    std::set<std::string> editedSources;
    for (const auto& edit : assetEdits_) {
        const std::string from = edit.from.toStdString();
        if (!editedSources.insert(CanonicalMember(from)).second) {
            return fail(tr("ASSET_SOURCE_AMBIGUOUS"), tr("同一原资源具有多个路径编辑，不能选择其中一份保存"));
        }
        const auto bytes = archiveSession_->ReadMember(from);
        if (!bytes.Succeeded() || !bytes.value) return fail(tr("ASSET_SOURCE_MISSING"), tr("要改动的资源在源归档中不存在：%1").arg(edit.from));
        const auto unregistered = importTable.Remove(from, memberExists);
        AddDiagnostics(unregistered.diagnostics);
        if (!unregistered.Succeeded()) return nullptr;
        // A 移到 B 后再导入新 A：B 保留旧字节，新 A 由已冻结的待导入内容覆盖。
        // 不能同时向归档写入同路径 removal+replacement。导入读取或身份核对失败仍丢弃整个任务。
        if (!pendingTargets.contains(CanonicalMember(from))) removals.push_back(from);
        if (edit.to.isEmpty()) continue;
        const std::string to = QString(edit.to).replace(QLatin1Char('/'), QLatin1Char('\\')).toStdString();
        if (IsManagedMember(CanonicalMember(to))) {
            return fail(tr("ASSET_MANAGED_MEMBER"), tr("资源不能覆盖编辑器当前管理的数据：%1").arg(edit.to));
        }
        replaceMember(to, *bytes.value);
        const auto registered = importTable.Upsert(to, memberExists);
        AddDiagnostics(registered.diagnostics);
        if (!registered.Succeeded()) return nullptr;
    }
    for (std::size_t importIndex = 0; importIndex < pendingImports_.size(); ++importIndex) {
        const auto& entry = pendingImports_[importIndex];
        const std::string name = QString(entry.archivePath).replace('\\', '/').toStdString();
        if (IsManagedMember(CanonicalMember(name))) {
            return fail(tr("IMPORT_MANAGED_MEMBER"), tr("导入资源不能覆盖编辑器当前管理的数据：%1").arg(entry.archivePath));
        }
        const auto expected = frozenScopes.find("imports/" + std::to_string(importIndex));
        if (expected == frozenScopes.end() || expected->second.size() < 64) {
            return fail(tr("IMPORT_SNAPSHOT_MISSING"), tr("缺少导入文件的完整快照"));
        }
        auto payload = ReadFrozenPendingImport(entry,
            std::string_view(expected->second).substr(expected->second.size() - 64));
        AddDiagnostics(payload.diagnostics);
        if (!payload.Succeeded()) {
            return fail(tr("IMPORT_SNAPSHOT_CHANGED"), tr("导入文件被占用或内容与保存开始时不同：%1").arg(entry.diskPath));
        }
        replaceMember(name, std::move(*payload.value));
        const auto registered = importTable.Upsert(name, [this](std::string_view path) { return archiveSession_->HasMember(path); });
        AddDiagnostics(registered.diagnostics);
        if (!registered.Succeeded()) return nullptr;
    }
    // 缺少暂停图标（DISBTN / DISPAS）的按钮图标：由按钮图标去色压暗生成，放进游戏查找的标准文件夹。
    const auto entries = AssetEntries();
    for (const QString& icon : missingDisabledIcons) {
        std::optional<std::vector<std::byte>> source;
        for (const auto& replacement : replacements) {
            if (CanonicalMember(replacement.name) == CanonicalMember(QString(icon).replace('\\', '/').toStdString())) source = replacement.data;
        }
        for (const auto& entry : entries) {
            if (!source && !entry.Pending() && AssetKey(entry.path) == AssetKey(icon)) {
                auto bytes = archiveSession_->ReadMember(entry.originalPath.toStdString());
                if (bytes.Succeeded() && bytes.value) source = std::move(*bytes.value);
            }
        }
        auto decoded = source ? Image::DecodeBlp(*source) : Result<Image::DecodedImage>::Failure("Image.Blp", "missing", "");
        auto encoded = decoded.Succeeded() && decoded.value
            ? Image::EncodeBlpPaletted(Image::MakeDisabledIcon(*decoded.value))
            : Result<std::vector<std::byte>>::Failure("Image.Blp", "decode", "");
        if (!encoded.Succeeded() || !encoded.value) {
            AddDiagnostic(tr("警告"), tr("资源"), tr("DISABLED_ICON_NOT_GENERATED"),
                tr("无法为 %1 生成暂停图标（只支持 BLP1 图标），游戏暂停时会显示为绿色方块。").arg(icon));
            continue;
        }
        const std::string disabled = DisabledIconPath(icon).toStdString();
        replaceMember(disabled, std::move(*encoded.value));
        const auto registered = importTable.Upsert(disabled, [this](std::string_view path) { return archiveSession_->HasMember(path); });
        AddDiagnostics(registered.diagnostics);
        if (!registered.Succeeded()) return nullptr;
        AddDiagnostic(tr("信息"), tr("资源"), tr("DISABLED_ICON_GENERATED"),
            tr("已为 %1 生成暂停图标 %2").arg(icon, QString::fromStdString(disabled)));
    }
    if (editsImports) {
        auto encoded = importTable.Serialize();
        AddDiagnostics(encoded.diagnostics);
        if (!encoded.Succeeded() || !encoded.value) return nullptr;
        replaceMember("war3map.imp", std::move(*encoded.value));
    }

    // 使用预放对象的GUI图将实际成员/分类依赖纳入同一保存任务。前缀只选择需要检查
    // 的候选，真正资格由sealed快照及共用resolver给出，不按名字猜对象存在。
    const bool objectChanges = std::any_of(objectDirty_.begin(), objectDirty_.end(), [](bool dirty) { return dirty; });
    const auto savedRuntime = editRevision_.Baseline(EditSaveTarget::Map);
    const auto runtimeApproval = [](const EditScopeSnapshot& scopes) {
        const auto found = scopes.find("runtime/approval");
        return found == scopes.end() ? std::string{} : found->second;
    };
    const bool runtimeChanged = !savedRuntime || runtimeApproval(savedRuntime->scopes) != runtimeApproval(frozenScopes);
    // 带测试场景的副本和运行库批准变化都需要重建本次脚本。
    const bool hasChanges = triggerDirty_ || objectChanges || editsImports || runtimeChanged || (mapEdits_ && mapEdits_->IsDirty())
        || luaMapTarget_ || testScenario_;
    const auto objectNames = triggerDocument_ && hasChanges ? ActivePreplacedNames(*triggerDocument_)
        : Result<std::vector<std::string>>::Success({});
    AddDiagnostics(objectNames.diagnostics);
    if (!objectNames.Succeeded() || !objectNames.value)
        return fail(tr("REPACK_MAP_OBJECT_REFERENCE_INVALID"), tr("无法确定运行T中的预放对象依赖。"));
    if (!objectNames.value->empty()) {
        Services::ObjectCatalogSources classification;
        classification.archive = job->archive.get();
        classification.gameRoot = war3GameRoot_;
        classification.campaign = campaignScope_.get();
        classification.replacements = job->replacements;
        classification.removals = job->removals;
        classification.overlayIdentity = job->sourceSha256;
        auto references = Services::CaptureArchiveMapObjectReferences(*job->archive, war3GameRoot_, job->replacements, job->removals, &classification);
        AddDiagnostics(references.diagnostics);
        if (!references.Succeeded() || !references.value)
            return fail(tr("REPACK_MAP_OBJECT_REFERENCES_UNAVAILABLE"), tr("无法冻结当前T使用的预放对象来源。"));
        bool used = false;
        for (const auto& name : *objectNames.value) {
            const auto resolved = Trigger::ResolveVariableReference(*triggerDocument_, name, references.value->get());
            AddDiagnostics(resolved.diagnostics);
            if (!resolved.Succeeded() || !resolved.value)
                return fail(tr("REPACK_MAP_OBJECT_REFERENCE_INVALID"), tr("T中的预放单位/物品引用已失效。"));
            used |= resolved.value->kind == Trigger::VariableReferenceKind::PreplacedObject;
        }
        if (used) {
            const auto inputs = (*references.value)->Inputs();
            if (std::any_of(inputs.begin(), inputs.end(), [](const auto& input) {
                return input.scope == Trigger::MapObjectReferenceSourceScope::Campaign;
            })) return fail(tr("CAMPAIGN_CLASSIFICATION_SAVE_UNFROZEN"),
                tr("预放引用使用了战役分类表，后台保存尚未冻结这份来源；未写出地图。"));
            job->mapObjectReferences = std::move(*references.value);
            job->mapObjectReferenceGameRoot = war3GameRoot_;
        }
    }

    // 有任何修改时重建游戏脚本：物编与导入的声音也会进入地图初始化代码。执行阶段通过语法检查才写入，
    // 未通过则中止保存；工具链不可用时沿用源脚本并警告。
    if (hasChanges) {
        if (PrepareScriptRebuild(*job) == ScriptRebuild::Failed) {
            return fail(tr("REPACK_SCRIPT_REJECTED"), tr("无法准备游戏脚本重建，已中止保存；详见构建诊断。"));
        }
        // 最终W3I与外矩形共同决定游戏初始化，范围或尺寸命令不能沿用旧脚本。
        if (mapEdits_ && (mapEdits_->HasTerrainBoundsChanges() || mapEdits_->HasTerrainResizeChanges()))
            job->preserveUnaffectedScript = false;
    }
    if (!ObserveEditState("冻结地图保存输入")) {
        return fail(tr("SAVE_REVISION_UNAVAILABLE"), tr("无法复核冻结后的编辑修订，地图保存已中止。"));
    }
    if (editRevision_.Generation() != frozenGeneration || editRevision_.Revision() != frozenRevision
        || editRevision_.Current() != frozenScopes) {
        return fail(tr("SAVE_REVISION_CHANGED"), tr("准备保存时编辑内容发生变化，地图保存已中止。"));
    }
    if (objectWriteCatalog) {
        const auto* task = job.get();
        job->beforePublish = [catalog = std::move(objectWriteCatalog), gameRoot = war3GameRoot_, task,
            previous = std::move(job->beforePublish)]() {
            if (previous) {
                const auto status = previous();
                if (!status.Succeeded()) return status;
            }
            return VerifyFrozenObjectWriteSources(*task, *catalog, gameRoot);
        };
    }
    if (!auxiliarySaveMembers.empty()) {
        // 模板 import 在脚本生成之后合并，不能覆盖本次初始化所依据的结构编辑。
        const auto* task = job.get();
        job->beforePublish = [members = std::move(auxiliarySaveMembers), task,
            previous = std::move(job->beforePublish)]() {
            if (previous) {
                const auto status = previous();
                if (!status.Succeeded()) return status;
            }
            return Services::VerifyAuxiliarySaveOutput(members, task->replacements, task->removals);
        };
    }
    if (explicitMinimap) {
        // 只查看本任务已有列表，不在界面线程捕获资源、解析额外快照或生成小地图。
        // 原脚本Prepare只冻结script输入；native/template实际输出仍在worker原Run中形成。
        if (!job->removals.empty() || job->replacements.size() != 1
            || CanonicalMember(job->replacements.front().name) != "war3map.w3e")
            return fail(tr("MINIMAP_SAVE_DOMAIN"), tr("补建准备必须只有唯一的高度W3E输出，不能夹带通行性、其它地图数据或资源成员。"));
    }
    job->preparedRevision = MapSaveRevisionBinding{frozenGeneration, frozenRevision, frozenScopes, luaMapTargetGeneration_};
    InstallMapSaveDerivedPreparation(*job);
    return job;
}

} // namespace War3Map::QtProject
