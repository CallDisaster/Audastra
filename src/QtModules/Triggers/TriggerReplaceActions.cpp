#include "TriggerModule.h"
#include "TriggerReplaceDialog.h"

#include "EditorProject.h"
#include "TriggerCommands.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QPointer>
#include <QPromise>
#include <QThreadPool>
#include <QUndoStack>
#include <QVariantMap>

#include <array>
#include <algorithm>
#include <limits>
#include <exception>
#include <chrono>

namespace War3Map::Modules {
namespace {
using namespace Services;
Status Unavailable() { return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_SOURCE_UNAVAILABLE", "触发器、目录或来源未准备；保留当前编辑，请重新查找。"); }
template<class T> Result<T> UnavailableResult() { return {std::nullopt, Unavailable().diagnostics}; }

// 只借用已接受的生效文档；未加载且未证明缺席的域保持Unknown，不补查磁盘。
std::array<ProjectSearchObjectSource, 7> ObjectSources(const QtProject::EditorProject& project) {
    std::array<ProjectSearchObjectSource, 7> sources;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto& source = sources[index]; source.type = static_cast<ObjectData::ObjectType>(index);
        if (const auto* lni = project.objectDocuments_[index].get()) {
            source.state = lni->IsStructurallySafe() && lni->Type() == source.type ? ProjectSearchState::Ready : ProjectSearchState::Partial;
            source.records = lni->Objects();
        } else if (const auto* native = project.nativeObjectDocuments_[index].get()) {
            source.origin = ProjectSearchObjectOrigin::Native;
            source.state = native->IsStructurallySafe() && native->Type() == source.type ? ProjectSearchState::Ready : ProjectSearchState::Partial;
            source.records = native->Objects();
        }
    }
    return sources;
}

template<class Callback>
auto WithContext(const QtProject::EditorProject& project, std::string revision,
    const Trigger::TriggerTypeSystem* types, const MapDocument* strings, Callback&& callback) {
    const auto sources = ObjectSources(project);
    TriggerReplaceContext context;
    context.revision = std::move(revision); context.document = project.triggerDocument_.get(); context.catalog = project.uiCatalog_.get();
    context.types = types; context.objects = sources; context.standardObjects = project.standardCatalog_.get();
    context.standardObjectsReady = context.standardObjects != nullptr;
    // gg_资格必须来自真实prepared snapshot；本入口不为普通查询隐式读地图。
    if (strings && strings->Member(MapDataKind::Strings).state == MapDataState::Ready && strings->Strings()) context.strings = &*strings->Strings();
    else if (!project.mapEdits_ && project.mapDocument_ && project.mapDocument_->Member(MapDataKind::Strings).state == MapDataState::Ready && project.mapDocument_->Strings())
        context.strings = &*project.mapDocument_->Strings();
    return callback(context);
}
}

std::string TriggerModule::TriggerReplaceRevision() const {
    if (!project_.triggerDocument_ || !project_.uiCatalog_) return {};
    return std::to_string(project_.editRevision_.Generation()) + "/" + std::to_string(project_.editRevision_.Revision())
        + "/" + std::to_string(triggerReplaceSourceEpoch_) + "/" + std::to_string(project_.mapEdits_ ? project_.mapEdits_->Revision() : 0)
        + "/" + project_.uiCatalog_->Sha256();
}
void TriggerModule::InvalidateTriggerReplace(bool sourcesChanged) {
    if (triggerReplaceStop_) triggerReplaceStop_->request_stop();
    if (triggerReplaceSourceEpoch_ == (std::numeric_limits<std::uint64_t>::max)()) {
        // 理论溢出不复用任何旧计划；真实编辑Generation/Revision仍参与身份。
        triggerReplaceSourceEpoch_ = 0;
    } else ++triggerReplaceSourceEpoch_;
    triggerReplacePreparedRevision_.clear();
    if (sourcesChanged) {
        triggerReplaceStrings_.reset(); triggerReplaceMapPrepared_ = false;
        triggerReplaceTypes_.reset(); triggerReplaceCatalog_ = nullptr; triggerReplaceCatalogSha_.clear();
    }
    if (triggerReplaceDialog_) triggerReplaceDialog_->ContextChanged();
}
Status TriggerModule::PrepareTriggerReplaceSources() {
    if (!project_.triggerDocument_ || !project_.uiCatalog_ || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_)
        return Unavailable();
    const auto revision = TriggerReplaceRevision();
    if (triggerReplacePreparedRevision_ == revision && triggerReplaceTypes_ && triggerReplaceCatalog_ == project_.uiCatalog_.get()
        && triggerReplaceCatalogSha_ == project_.uiCatalog_->Sha256()) return Status::Ok();
    if (!triggerReplaceTypes_ || triggerReplaceCatalog_ != project_.uiCatalog_.get() || triggerReplaceCatalogSha_ != project_.uiCatalog_->Sha256()) {
        constexpr std::uintmax_t maxBytes = 16U * 1024U * 1024U;
        constexpr std::size_t maxDefinitions = 32768, maxSources = 1024;
        const auto& manifest = project_.uiCatalog_->Manifest();
        if (manifest.size() > maxSources) return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_TYPE_BUDGET", "函数目录来源数量超过类型准备预算；没有构建类型缓存。");
        std::uintmax_t bytes = 0; std::size_t definitions = 0;
        for (const auto& source : manifest) {
            if (!source.present || !source.parsed || source.kind != Trigger::UiDefinitionKind::Define) continue;
            if (source.size > maxBytes - bytes || source.definitionCount > maxDefinitions - definitions)
                return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_TYPE_BUDGET", "已接受目录的类型/预设来源超过16 MiB或32768条；没有构建类型缓存。");
            bytes += source.size; definitions += source.definitionCount;
        }
        triggerReplaceTypes_ = std::make_unique<Trigger::TriggerTypeSystem>(*project_.uiCatalog_);
        triggerReplaceCatalog_ = project_.uiCatalog_.get(); triggerReplaceCatalogSha_ = project_.uiCatalog_->Sha256();
    }
    const auto mapRevision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
    if (!triggerReplaceMapPrepared_ || triggerReplaceMapRevision_ != mapRevision) {
        triggerReplaceStrings_.reset(); triggerReplaceMapRevision_ = mapRevision;
        if (project_.mapEdits_) {
            // 用户明确开始新查询时才准备当前WTS；只取该成员，不读取文件、不复制地形或其他地图正文。
            const auto snapshot = project_.mapEdits_->Snapshot(MapDataKind::Strings, TriggerReplaceLimits{}.maxNameTextBytes);
            if (snapshot.Succeeded() && snapshot.value) triggerReplaceStrings_ = std::make_unique<MapDocument>(std::move(*snapshot.value));
            // 缺席/损坏/过预算保持nullptr，共享Query如实raw-only Partial，不回退旧基线WTS。
        }
        triggerReplaceMapPrepared_ = true;
    }
    if (revision != TriggerReplaceRevision()) return Unavailable();
    triggerReplacePreparedRevision_ = revision;
    return Status::Ok();
}

void TriggerModule::ShowFindReplace() {
    Widget();
    if (!triggerReplaceDialog_) {
        const QPointer<TriggerModule> self(this);
        const auto ready = [self] {
            return self && self->project_.triggerDocument_ && self->project_.uiCatalog_ && self->triggerReplaceTypes_
                && self->triggerReplaceCatalog_ == self->project_.uiCatalog_.get()
                && self->triggerReplaceCatalogSha_ == self->project_.uiCatalog_->Sha256()
                && self->triggerReplacePreparedRevision_ == self->TriggerReplaceRevision()
                && !self->project_.sourceChangeSuspected_ && !self->project_.sourceChangeConfirmed_;
        };
        triggerReplaceDialog_ = new TriggerReplaceDialog(
            [self] { return self ? self->TriggerReplaceRevision() : std::string{}; },
            [self] { return self ? self->PrepareTriggerReplaceSources() : Unavailable(); },
            [self, ready](std::string_view revision, const TriggerReplaceQuery& query, const TriggerReplaceLimits& limits, const TriggerReplaceCursor* cursor) {
                if (!ready()) return UnavailableResult<TriggerReplacePage>();
                return WithContext(self->project_, self->TriggerReplaceRevision(), self->triggerReplaceTypes_.get(), self->triggerReplaceStrings_.get(),
                    [&](const auto& context) { return QueryTriggerReplace(context, revision, query, limits, cursor); });
            },
            [self, ready](std::string_view revision, TriggerReplacementKind kind, std::string_view filter, const TriggerReplaceLimits& limits, const TriggerReplaceCursor* cursor) {
                if (!ready()) return UnavailableResult<TriggerReplacementOptionsPage>();
                return WithContext(self->project_, self->TriggerReplaceRevision(), self->triggerReplaceTypes_.get(), self->triggerReplaceStrings_.get(),
                    [&](const auto& context) { return QueryTriggerReplacementOptions(context, revision, kind, filter, limits, cursor); });
            },
            [self, ready](std::string_view revision, const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof,
                std::span<const std::string> keys, const TriggerReplacement& replacement, const TriggerReplaceLimits& limits) {
                if (!ready()) return UnavailableResult<TriggerReplacePreviewResult>();
                return WithContext(self->project_, self->TriggerReplaceRevision(), self->triggerReplaceTypes_.get(), self->triggerReplaceStrings_.get(),
                    [&](const auto& context) { return PrepareTriggerReplacement(context, revision, query, proof, keys, replacement, limits); });
            },
            [self, ready](std::string_view revision, const TriggerReplacePreview& preview, const TriggerReplaceLimits& limits) {
                if (!ready() || revision != self->TriggerReplaceRevision()) return Unavailable();
                const auto expected = std::string(revision);
                if (!self->CommitPendingEdits()) return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_DRAFT_BLOCKED", "草稿不能提交；没有应用替换，草稿保持。");
                if (!self || !ready() || expected != self->TriggerReplaceRevision())
                    return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_DRAFT_CHANGED_REVISION", "草稿提交改变了真实修订；旧预览已拒绝，请按当前正文重新查找和预览。");
                const auto verified = WithContext(self->project_, self->TriggerReplaceRevision(), self->triggerReplaceTypes_.get(), self->triggerReplaceStrings_.get(),
                    [&](const auto& context) { return CheckTriggerReplacementPublicationContext(context, expected, preview, limits); });
                if (!verified.Succeeded()) return verified;
                auto* stack = self->project_.triggerUndoStack_;
                if (!stack) return Unavailable();
                auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(self->project_, preview.ScalarReceipt(),
                    QObject::tr("批量替换触发器参数（%1项）").arg(QString::number(static_cast<qulonglong>(preview.Rows().size()))));
                if (!command->Ready()) return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_PUBLICATION_REJECTED", "当前会话或来源不再接受该快照；没有应用替换。");
                const auto published = command->FirstPublication(); stack->push(command.release());
                if (!published || !*published) return Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_PUBLICATION_REJECTED", "Core 标量事务发布被拒绝；没有应用替换。");
                if (self) self->ShowStatus(QObject::tr("已整批替换 %1 项；一次撤销可还原。").arg(QString::number(static_cast<qulonglong>(preview.Rows().size()))), 10000);
                return Status::Ok();
            },
            [self](const TriggerReplaceHit& hit) {
                if (!self) return;
                QVariantMap extra{{QStringLiteral("triggerId"), QString::number(hit.triggerId)}, {QStringLiteral("stableIdentity"), true}};
                if (hit.argumentId) extra.insert(QStringLiteral("argumentId"), QString::number(*hit.argumentId));
                (void)self->Reveal({"triggers", "eca", QString::number(hit.ecaId), std::move(extra)});
            }, page_);
        triggerReplaceDialog_->SetAsyncPreview([self, ready](std::string_view revision, const TriggerReplaceQuery& query,
            const TriggerReplaceQueryProof& proof, std::span<const std::string> keys, const TriggerReplacement& replacement,
            const TriggerReplaceLimits& limits) -> Result<TriggerReplaceDialog::PreviewTask> {
            if (!ready() || revision != self->TriggerReplaceRevision()) return UnavailableResult<TriggerReplaceDialog::PreviewTask>();
            const auto busy = self->triggerReplaceBusy_;
            if (busy->exchange(true)) return {std::nullopt, Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_PREVIEW_BUSY",
                "上一份预览仍在安全退出；请等待它完成后再预览，没有启动第二个后台任务。").diagnostics};
            // 在 worker 发布完成前释放 owned 输入并解除 single-flight；任何异常也解除。
            struct BusyReset {
                std::shared_ptr<std::atomic_bool> busy;
                bool transferred = false;
                ~BusyReset() { if (!transferred) busy->store(false); }
            } reset{busy};
            const auto expected = std::string(revision);
            const auto* document = self->project_.triggerDocument_.get(); const auto* catalog = self->project_.uiCatalog_.get();
            const auto generation = self->project_.editRevision_.Generation(); const auto editRevision = self->project_.editRevision_.Revision();
            const auto bound = [self, ready, document, catalog, generation, editRevision, expected] {
                return ready() && self->project_.triggerDocument_.get() == document && self->project_.uiCatalog_.get() == catalog
                    && self->project_.editRevision_.Generation() == generation && self->project_.editRevision_.Revision() == editRevision
                    && expected == self->TriggerReplaceRevision();
            };
            const auto planStarted = std::chrono::steady_clock::now();
            auto prepared = WithContext(self->project_, expected, self->triggerReplaceTypes_.get(), self->triggerReplaceStrings_.get(),
                [&](const auto& context) { return PrepareTriggerReplacementPlan(context, expected, query, proof, keys, replacement, limits); });
            const auto planDuration = std::chrono::steady_clock::now() - planStarted;
            if (!bound()) return UnavailableResult<TriggerReplaceDialog::PreviewTask>();
            if (!prepared.Succeeded() || !prepared.value) return {std::nullopt, std::move(prepared.diagnostics)};
            auto stop = std::make_shared<std::stop_source>();
            auto timing = std::make_shared<TriggerReplaceDialog::PreviewTiming>();
            timing->planNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(planDuration).count();
            auto promise = std::make_shared<QPromise<Result<TriggerReplacePreviewResult>>>(); promise->start();
            TriggerReplaceDialog::PreviewTask task{promise->future(), stop, timing};
            if (!prepared.value->plan) {
                // 保留所有逐项拒绝，不因为没有可发布 Plan 就静默隐藏拒绝行。
                TriggerReplacePreviewResult rejected; rejected.proof = prepared.value->proof;
                rejected.rows = std::move(prepared.value->rows); rejected.documentBytesCharged = prepared.value->documentBytesCharged;
                rejected.workUnitsCharged = prepared.value->workUnitsCharged;
                promise->addResult(Result<TriggerReplacePreviewResult>::Success(std::move(rejected))); promise->finish();
                return Result<TriggerReplaceDialog::PreviewTask>::Success(std::move(task));
            }
            auto plan = std::move(*prepared.value->plan);
            // 同一直线一次 clone；没有事件排放/后台借用，也不保存第二份 candidate。
            const auto cloneStarted = std::chrono::steady_clock::now();
            auto ownedDocument = std::make_unique<Trigger::TriggerDocument>(*document);
            auto ownedCatalog = std::make_unique<Trigger::UiCatalog>(*catalog);
            timing->cloneNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cloneStarted).count();
            if (!bound()) return UnavailableResult<TriggerReplaceDialog::PreviewTask>();
            self->triggerReplaceStop_ = stop;
            timing->queuedAtNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            QThreadPool::globalInstance()->start([plan = std::move(plan), document = std::move(ownedDocument),
                catalog = std::move(ownedCatalog), limits, stop, promise, busy, timing]() mutable {
                struct Finish {
                    std::shared_ptr<std::atomic_bool> busy;
                    std::shared_ptr<QPromise<Result<TriggerReplacePreviewResult>>> promise;
                    std::shared_ptr<TriggerReplaceDialog::PreviewTiming> timing;
                    std::chrono::steady_clock::time_point started;
                    ~Finish() {
                        const auto finished = std::chrono::steady_clock::now();
                        timing->workerNanoseconds.store(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
                        timing->finishedAtNanoseconds.store(std::chrono::duration_cast<std::chrono::nanoseconds>(finished.time_since_epoch()).count());
                        busy->store(false); promise->finish();
                    }
                } finish{busy, promise, timing, std::chrono::steady_clock::now()};
                timing->queueNanoseconds.store(std::chrono::duration_cast<std::chrono::nanoseconds>(finish.started.time_since_epoch()).count() - timing->queuedAtNanoseconds);
                try {
                    auto result = CompleteTriggerReplacementPlan(plan, std::move(*document), std::move(*catalog), limits, stop->get_token());
                    document.reset(); catalog.reset();
                    promise->addResult(std::move(result));
                } catch (...) {
                    document.reset(); catalog.reset();
                    // 即使分配错误无法创建诊断，finish 仍结束 Future；GUI 会明确拒绝空结果。
                    try { promise->addResult(Result<TriggerReplacePreviewResult>{std::nullopt,
                        Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_ASYNC_FAILED", "后台预览未完成；没有修改当前地图。").diagnostics}); }
                    catch (...) {}
                }
            });
            reset.transferred = true;
            return Result<TriggerReplaceDialog::PreviewTask>::Success(std::move(task));
        });
    }
    const auto* trigger = CurrentTrigger();
    triggerReplaceDialog_->SetCurrentScope(trigger && !trigger->customTextFlag && trigger->type != 1 ? std::optional<StableId>(trigger->id) : std::nullopt,
        trigger ? QString::fromUtf8(trigger->name.text.data(), static_cast<qsizetype>(std::min<std::size_t>(256, trigger->name.text.size()))) : QString{});
    triggerReplaceDialog_->FocusQuery();
}

} // namespace War3Map::Modules
