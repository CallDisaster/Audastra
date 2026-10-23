#include "EditorProject.h"
#include "ProjectSaveJob.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUndoStack>

#include <exception>

namespace War3Map::QtProject {
namespace {
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Path(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
QString AbsoluteName(const std::filesystem::path& path) {
    const QFileInfo info(Path(path));
    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName()));
}
} // namespace

Result<std::unique_ptr<ProjectSaveJob>> EditorProject::PrepareProjectSave(
    const std::filesystem::path& target, bool recovery,
    std::optional<std::string> expectedRecoveryTargetSha256, std::shared_ptr<ProjectSaveControl> control) {
    const auto fail = [](const QString& message) {
        return Result<std::unique_ptr<ProjectSaveJob>>::Failure("工程", "PROJECT_SAVE_FAILED", message.toStdString());
    };
    try {
        if (target.empty() || projectRoot_.empty()) return fail(tr("未打开工程或保存路径为空"));
        if (!recovery && expectedRecoveryTargetSha256) return fail(tr("手动工程保存不能借用恢复目标更新身份"));
        if (!recovery && control) return fail(tr("手动工程保存不能借用恢复任务的取消控制器"));
        if (control && !control->CanRun()) return fail(tr("取消控制器已完成，不能复用于另一份恢复快照"));
        const auto sourceRoot = QDir::fromNativeSeparators(QFileInfo(Path(projectRoot_)).canonicalFilePath());
        const QFileInfo outputInfo(Path(target));
        const auto outputParent = QDir::fromNativeSeparators(QFileInfo(outputInfo.absolutePath()).canonicalFilePath());
        if (outputParent.isEmpty() || (!sourceRoot.isEmpty()
            && (outputParent.compare(sourceRoot, Qt::CaseInsensitive) == 0
                || outputParent.startsWith(sourceRoot + '/', Qt::CaseInsensitive))))
            return fail(tr("工程必须保存到源工作区之外的独立位置"));
        // 这里只核对缓存及路径关系；整图 SHA、文件身份和可信读取全部留给 Run。
        if (archiveSession_) {
            std::error_code sourceError;
            if (!std::filesystem::equivalent(sourceArchivePath_, archiveSession_->SourcePath(), sourceError) || sourceError)
                return fail(tr("地图来源不可达或与当前归档快照不一致"));
        } else if (!sourceArchivePath_.empty()) return fail(tr("地图快照不完整，不能创建恢复工程"));
        else if (!directorySnapshot_) return fail(tr("源工作区缺少来源快照"));

        if (!ObserveEditState("保存工程前核对")) return fail(tr("无法确认当前编辑修订，保存基线保持不变"));
        auto job = std::unique_ptr<ProjectSaveJob>(new ProjectSaveJob);
        job->generation_ = editRevision_.Generation();
        job->revision_ = editRevision_.Revision();
        job->scopes_ = editRevision_.Current();
        job->sourceRoot_ = projectRoot_;
        job->sourceArchivePath_ = sourceArchivePath_;
        job->sourceSha256_ = archiveSession_ ? archiveSession_->SourceSha256() : directorySnapshot_->Snapshot().aggregateSha256;
        job->target_ = target;
        job->uiSha256_ = uiCatalog_ ? uiCatalog_->Sha256() : std::string{};
        job->recovery_ = recovery;
        if (recovery) job->options_.control = control ? std::move(control) : std::make_shared<ProjectSaveControl>();
        if (recovery) job->cancelledOutcome_ = {{}, ProjectSaveCancelledStatus().diagnostics};
        job->archive_ = archiveSession_;
        job->archiveOwner_ = archiveSession_;
        if (directorySnapshot_) job->directory_ = *directorySnapshot_;
        job->restoredFiles_ = restoredFiles_;
        job->mapWorkspace_ = mapWorkspace_;
        job->manifest_ = QJsonObject{{"format", "forge-project"}, {"version", 1},
            {"savedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"sourceKind", archiveSession_ ? "archive" : "directory"},
            {"uiRoot", Path(compilerDataRoot_.empty() ? activeUiDataRoot_ : compilerDataRoot_)},
            {"uiSha256", Text(job->uiSha256_)}, {"recovery", recovery}};
        // 恢复时据此直接打开作者原图（逐字节等于基线时），“保存地图”才能写回原位置。
        if (archiveSession_ && !AuthorMapPath().empty()) job->manifest_["sourceMapPath"] = Path(AuthorMapPath());
        std::uint64_t total = 0;
        const auto append = [&job, &total](std::string name, std::vector<std::byte> payload) {
            constexpr std::uint64_t limit = 512ull * 1024 * 1024;
            if (payload.size() > limit - total) return false;
            total += payload.size();
            job->overlays_.push_back({std::move(name), std::move(payload)});
            return true;
        };
        QJsonArray objectCloneSources;
        for (std::size_t index = 0; index < objectDocuments_.size(); ++index) {
            const auto& document = objectDocuments_[index];
            if (!document) continue;
            if (!document->CloneSources().empty()) {
                const auto& native = nativeObjectDocuments_[index];
                if (!native) return fail(tr("克隆来源缺少实际 native 基线，未保存不完整工程"));
                auto probe = *document;
                if (!probe.RestoreCloneSources(document->CloneSources(), *native,
                    objectFieldSchema_.get(), standardCatalog_.get()).Succeeded())
                    return fail(tr("克隆来源、父级或拓扑无法从 native 基线确认"));
                auto nativeBytes = ObjectData::NativeObjectCodec(document->Type()).Serialize(*native);
                if (!nativeBytes.Succeeded() || !nativeBytes.value) return fail(tr("克隆来源的 native 基线无法无损序列化"));
                ProjectSaveJob::NativeBaseline baseline;
                baseline.member = ObjectData::NativeFileName(document->Type());
                baseline.archiveMemberPresent = archiveSession_ && archiveSession_->HasMember(baseline.member);
                if (!baseline.archiveMemberPresent && directorySnapshot_) {
                    for (const auto& file : directorySnapshot_->Snapshot().files) {
                        const auto relative = file.relativePath.generic_string();
                        if (relative != baseline.member && relative != "map/" + baseline.member) continue;
                        if (baseline.directoryMember) return fail(tr("克隆来源存在多个 native 基线"));
                        baseline.directoryMember = relative;
                        baseline.directorySha256 = file.sha256;
                    }
                }
                const bool sourcePresent = baseline.archiveMemberPresent || baseline.directoryMember.has_value();
                if (!sourcePresent) {
                    const auto empty = ObjectData::NativeObjectDocument::CreateEmpty(document->Type());
                    if (!empty.Succeeded() || !empty.value) return fail(tr("缺失 native 成员的经典空基线无法建立"));
                    const auto emptyBytes = ObjectData::NativeObjectCodec(document->Type()).Serialize(*empty.value);
                    if (!emptyBytes.Succeeded() || emptyBytes.value != nativeBytes.value)
                        return fail(tr("缺失源成员只能绑定经典 v2 空基线，未用内存对象冒充来源"));
                }
                const auto nativeHash = Sha256(*nativeBytes.value);
                if (!nativeHash.Succeeded() || !nativeHash.value) return fail(tr("克隆来源的 native 身份无法计算"));
                QJsonArray sources;
                for (const auto& source : document->CloneSources())
                    sources.push_back(QJsonObject{{"new_id", Text(source.newRawcode)}, {"source_id", Text(source.sourceRawcode)}});
                objectCloneSources.push_back(QJsonObject{{"kind", Text(ObjectData::ToString(document->Type()))},
                    {"native_sha256", Text(*nativeHash.value)}, {"source_present", sourcePresent}, {"sources", sources}});
                if (sourcePresent) {
                    baseline.expected = std::move(*nativeBytes.value);
                    job->nativeBaselines_.push_back(std::move(baseline));
                }
            }
            auto serialized = ObjectData::LniObjectCodec(document->Type()).Serialize(*document);
            if (!serialized.Succeeded() || !serialized.value) return fail(tr("物编文档不能完整保存"));
            if (!append("overlay/table/" + std::string(ObjectData::LniFileName(document->Type())), std::move(*serialized.value)))
                return fail(tr("工程超过容量限制"));
        }
        if (!objectCloneSources.isEmpty()) job->manifest_["objectCloneSources"] = objectCloneSources;
        if (triggerDocument_) {
            if (!uiCatalog_) return fail(tr("触发器定义不可用"));
            auto encoded = Trigger::WtgWctCodec{}.Serialize(*triggerDocument_, *uiCatalog_);
            if (!encoded.Succeeded() || !encoded.value) return fail(tr("触发器未通过结构检查"));
            if (!append("overlay/map/war3map.wtg", std::move(encoded.value->wtg))
                || !append("overlay/map/war3map.wct", std::move(encoded.value->wct))) return fail(tr("工程超过容量限制"));
            if (!guiConversionSnapshots_.Empty()) {
                const auto source = std::string(archiveSession_ ? "archive:" : "directory:") + job->sourceSha256_;
                auto snapshots = guiConversionSnapshots_.Serialize(*triggerDocument_, *uiCatalog_, source);
                if (!snapshots.Succeeded() || !snapshots.value) return fail(tr("转换前快照不能完整保存"));
                if (!append("overlay/editor/gui-conversion-snapshots.bin", std::move(*snapshots.value))) return fail(tr("工程超过容量限制"));
                job->manifest_["guiConversionSnapshots"] = 1;
            }
        }
        QJsonArray mapDataRemovals;
        if (mapEdits_) {
            for (auto& member : mapEdits_->Replacements())
                if (!append("overlay/map/" + member.name, std::move(member.data))) return fail(tr("工程超过容量限制"));
            for (const auto& name : mapEdits_->Removals()) mapDataRemovals.push_back(Text(name));
            auto restrictions = mapEdits_->SerializePathingRestrictionDraft();
            if (!restrictions.Succeeded() || !restrictions.value) return fail(tr("附加通行限制无法绑定原始来源"));
            if (!restrictions.value->empty()) {
                const auto restrictionVersion = std::to_integer<unsigned>((*restrictions.value)[3]);
                if (!append("overlay/editor/pathing-restrictions.bin", std::move(*restrictions.value)))
                    return fail(tr("工程超过容量限制"));
                job->manifest_["pathingRestrictions"] = static_cast<int>(restrictionVersion);
            }
            std::string journalWarning = terrainJournalWarning_;
            try {
                auto prefix = mapEdits_->ExportTerrainStrokeJournal();
                if (prefix.Succeeded() && prefix.value && !prefix.value->strokes.empty()) {
                    // 不在UI线程做全图SHA/MPQ/目录读取。只冻结有界前缀和无历史基线，
                    // worker借本job来源租约补实际member身份，绝不访问活动会话。
                    auto baseline = mapEdits_->FreezeForSave();
                    job->terrainJournalPrefix_ = std::move(*prefix.value);
                    job->terrainJournalBaseline_ = std::move(baseline);
                    // 旧raw草稿不能因后续一笔可表达命令清除未恢复警告。
                } else if (mapEdits_->RequiresTerrainStrokeJournal() || (!prefix.Succeeded() && mapEdits_->Terrain())) {
                    journalWarning = "草稿已完整保留；历史跨保存边界、混合编辑或超出前向记录预算，前向笔划及水刷来源未恢复。";
                }
            } catch (const std::exception&) {
                job->terrainJournalPrefix_.reset(); job->terrainJournalBaseline_.reset();
                journalWarning = "草稿已完整保留；前向记录冻结超过可用预算，前向笔划及水刷来源未恢复。";
            }
            if (!journalWarning.empty()) {
                job->manifest_["terrainJournalWarning"] = Text(journalWarning);
                auto warning = Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_DRAFT_ONLY", journalWarning);
                for (auto& diagnostic : warning.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
                job->warnings_ = std::move(warning.diagnostics);
            }
        }
        job->manifest_["mapDataRemovals"] = mapDataRemovals;
        if (uiCatalog_) {
            // 只嵌入已解析目录的原字节，不复制整个 YDWE/游戏安装目录。
            if (!append("definitions/ui/config", uiCatalog_->ConfigDocument().SerializeLossless())) return fail(tr("工程超过容量限制"));
            for (const auto& document : uiCatalog_->Documents()) {
                const auto name = Path(document.Source()).replace('\\', '/').toStdString();
                if (!append("definitions/" + name, document.SerializeLossless())) return fail(tr("工程超过容量限制"));
            }
            job->manifest_["uiEmbedded"] = true;
            job->manifest_["uiLibraries"] = UiLibraryUsageJson();
        }
        QJsonArray imports;
        for (std::size_t i = 0; i < pendingImports_.size(); ++i) {
            const auto& entry = pendingImports_[i];
            const auto member = "imports/" + std::to_string(i);
            const auto expected = job->scopes_.find(member);
            if (expected == job->scopes_.end() || expected->second.size() < 64) return fail(tr("缺少导入快照"));
            const auto sha = expected->second.substr(expected->second.size() - 64);
            job->imports_.push_back({entry, member, sha});
            imports.push_back(QJsonObject{{"member", Text(member)}, {"path", entry.archivePath},
                {"owned", static_cast<bool>(entry.ownedPayload)}, {"runtimeLuaDraft", entry.runtimeLuaDraft}, {"sha256", Text(sha)}});
        }
        job->manifest_["imports"] = imports;
        QJsonArray assetEdits;
        for (const auto& edit : assetEdits_) assetEdits.push_back(QJsonObject{{"from", edit.from}, {"to", edit.to}});
        job->manifest_["assetEdits"] = assetEdits;
        QJsonArray edits;
        for (auto* stack : objectUndoStacks_)
            if (stack) for (int i = 0; i < stack->index(); ++i) edits.push_back(stack->text(i));
        if (triggerUndoStack_) for (int i = 0; i < triggerUndoStack_->index(); ++i) edits.push_back(triggerUndoStack_->text(i));
        job->manifest_["edits"] = edits;
        QJsonArray journal = QJsonDocument::fromJson(QByteArray::fromStdString(restoredProjectJournalJson_)).array();
        for (const auto& entry : editRevision_.Journal()) {
            QJsonArray changes;
            for (const auto& change : entry.changes)
                changes.push_back(QJsonObject{{"scope", Text(change.scope)},
                    {"before", change.before ? QJsonValue(Text(*change.before)) : QJsonValue(QJsonValue::Null)},
                    {"after", change.after ? QJsonValue(Text(*change.after)) : QJsonValue(QJsonValue::Null)}});
            journal.push_back(QJsonObject{{"revision", QString::number(entry.revision)}, {"description", Text(entry.description)}, {"changes", changes}});
        }
        job->manifest_["revision"] = QString::number(job->revision_);
        job->manifest_["journal"] = journal;
        std::vector<std::string> importSha256;
        importSha256.reserve(job->imports_.size());
        for (const auto& entry : job->imports_) importSha256.push_back(entry.sha256);
        job->fingerprint_ = ProjectFingerprint(importSha256);
        if (job->fingerprint_.empty()) return fail(tr("无法确认编辑文档指纹"));
        job->manifest_["documentFingerprint"] = Text(job->fingerprint_);
        job->options_.protectedSourcePaths.push_back(projectRoot_);
        if (!sourceArchivePath_.empty()) job->options_.protectedSourcePaths.push_back(sourceArchivePath_);
        if (recovery) job->options_.expectedTargetSha256 = std::move(expectedRecoveryTargetSha256);
        else if (!projectFilePath_.empty()
            && AbsoluteName(target).compare(AbsoluteName(projectFilePath_), Qt::CaseInsensitive) == 0)
            job->options_.expectedTargetSha256 = projectFileSha256_;
        // 所有可能失败的输入冻结都已完成；恢复任务不会占据手动保存的 Project 票据。
        if (!recovery) {
            auto ticket = editRevision_.BeginSave(EditSaveTarget::Project);
            if (!ticket.Succeeded() || !ticket.value) return fail(tr("已有工程保存正在进行"));
            job->ticket_ = *ticket.value;
        }
        return Result<std::unique_ptr<ProjectSaveJob>>::Success(std::move(job));
    } catch (const std::exception&) {
        return fail(tr("工程保存准备发生内部异常，当前编辑基线保持不变"));
    } catch (...) {
        return fail(tr("工程保存准备发生未知内部异常，当前编辑基线保持不变"));
    }
}

Result<ProjectSaveResult> EditorProject::CommitProjectSave(ProjectSaveJob& job) {
    const auto reject = [](std::string code, const QString& reason) {
        return Result<ProjectSaveResult>::Failure("工程", std::move(code), reason.toStdString());
    };
    if (!job.Completed()) return reject("PROJECT_SAVE_NOT_COMPLETED", tr("工程保存任务尚未完成"));
    if (job.committed_) return reject("PROJECT_SAVE_ALREADY_COMMITTED", tr("工程保存任务已经提交"));
    job.committed_ = true;
    struct Finish {
        ProjectSaveJob& job;
        ~Finish() { job.ReleaseCommitLeases(); }
    } finish{job};
    const bool sameGeneration = editRevision_.Generation() == job.generation_;
    const auto endManual = [this, &job, sameGeneration](bool success) {
        if (!job.recovery_ && job.ticket_ && sameGeneration) return editRevision_.CompleteSave(*job.ticket_, success).Succeeded();
        return job.recovery_;
    };
    if (!job.outcome_.Succeeded()) {
        (void)endManual(false);
        return job.outcome_;
    }
    const bool sameSource = projectRoot_ == job.sourceRoot_ && sourceArchivePath_ == job.sourceArchivePath_
        && (job.sourceArchivePath_.empty()
            ? (!archiveSession_ && directorySnapshot_ && directorySnapshot_->Snapshot().aggregateSha256 == job.sourceSha256_)
            : (archiveSession_ && archiveSession_->SourceSha256() == job.sourceSha256_
                && !job.archiveOwner_.owner_before(archiveSession_) && !archiveSession_.owner_before(job.archiveOwner_)));
    if (!sameGeneration || !sameSource) {
        (void)endManual(false);
        return reject("PROJECT_SAVE_STALE", tr("工程快照已写出，但会话或来源已切换；该文件仅保留为历史快照，当前编辑和保存基线保持不变。"));
    }
    if (job.recovery_) {
        // 恢复代表可信捕获时的历史修订，绝不证明当前磁盘来源仍可写回。
        // 目标父目录事件可与真实来源事件并发；保留所有 source flags，不能以自己的
        // 恢复写入清疑虑或抑制通知。相同会话的晚到编辑留给调用方排下一周期。
        auto written = job.outcome_;
        try { written.diagnostics.insert(written.diagnostics.end(), job.warnings_.begin(), job.warnings_.end()); }
        catch (...) {} // 警告已持久化进工程，不将已发布恢复文件伪报为失败。
        return written;
    }
    bool baselineCommitted = false;
    try {
        if (!ObserveEditState("工程保存提交前复核") || editRevision_.Generation() != job.generation_
            || editRevision_.Revision() != job.revision_ || editRevision_.Current() != job.scopes_
            || ProjectFingerprint() != job.fingerprint_
            || (uiCatalog_ ? uiCatalog_->Sha256() : std::string{}) != job.uiSha256_) {
            (void)endManual(false);
            return reject("PROJECT_SAVE_REVISION_CHANGED", tr("工程已写出，但保存期间编辑状态发生变化；当前编辑未标记为已保存。"));
        }
        // 所有会分配的回执/身份副本先准备，提交后只 swap 小字段。
        auto written = job.outcome_;
        written.diagnostics.insert(written.diagnostics.end(), job.warnings_.begin(), job.warnings_.end());
        auto fingerprint = job.fingerprint_;
        auto path = job.outcome_.value->path;
        auto sha256 = job.outcome_.value->sha256;
        if (!endManual(true)) return reject("PROJECT_SAVE_TICKET_CHANGED", tr("工程已写出，但冻结保存票据已失效；当前保存基线保持不变。"));
        baselineCommitted = true;
        sourceWatchQuietUntil_ = QDateTime::currentMSecsSinceEpoch() + 3000;
        lastProjectFingerprint_.swap(fingerprint);
        projectFilePath_.swap(path);
        projectFileSha256_.swap(sha256);
        UpdateEditDirtyFlags();
        RefreshProjectWatch();
        return written;
    } catch (...) {
        if (baselineCommitted) {
            // 磁盘及保存基线已成功，刷新异常不能伪报“未保存”或回滚已发布文件。
            auto warning = Status::Failure("工程", "PROJECT_SAVE_VIEW_REFRESH_FAILED",
                "工程及保存基线已保存，但界面状态刷新发生内部异常；请重新打开已保存工程核对。");
            for (auto& diagnostic : warning.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
            auto written = job.outcome_;
            written.diagnostics.insert(written.diagnostics.end(), warning.diagnostics.begin(), warning.diagnostics.end());
            return written;
        }
        (void)endManual(false);
        return reject("PROJECT_SAVE_COMMIT_EXCEPTION", tr("工程已写出，但保存提交发生内部异常；当前编辑未标记为已保存。"));
    }
}

Status EditorProject::CancelPreparedProjectSave(ProjectSaveJob& job) {
    auto cancelled = Result<ProjectSaveResult>::Failure("工程", "PROJECT_SAVE_CANCELLED", "工程保存任务已取消，当前编辑基线保持不变。");
    auto expected = ProjectSaveJob::Phase::Prepared;
    if (!job.phase_.compare_exchange_strong(expected, ProjectSaveJob::Phase::Cancelling,
        std::memory_order_acq_rel))
        return Status::Failure("工程", "PROJECT_SAVE_ALREADY_STARTED", "工程任务已经运行、完成或取消，不能作为尚未运行的任务取消。");
    // 只能从 UI 调用。旧 generation 不接触新会话的保存票据。
    Status ended = Status::Ok();
    if (!job.recovery_ && job.ticket_ && editRevision_.Generation() == job.generation_)
        ended = editRevision_.CompleteSave(*job.ticket_, false);
    job.outcome_ = std::move(cancelled);
    if (job.options_.control) {
        (void)job.options_.control->RequestCancellation();
        (void)job.options_.control->Complete();
    }
    job.ReleaseRunLeases();
    job.ReleaseCommitLeases();
    job.phase_.store(ProjectSaveJob::Phase::Cancelled, std::memory_order_release);
    return ended;
}

bool EditorProject::RequestProjectSaveCancellation(ProjectSaveJob& job) noexcept {
    return job.RequestCancellation();
}

Result<ProjectSaveResult> EditorProject::SaveProjectToPath(const std::filesystem::path& target, bool recovery) {
    auto prepared = PrepareProjectSave(target, recovery);
    if (!prepared.Succeeded() || !prepared.value) return {{}, std::move(prepared.diagnostics)};
    auto job = std::move(*prepared.value);
    RunProjectSave(*job);
    return CommitProjectSave(*job);
}

} // namespace War3Map::QtProject
