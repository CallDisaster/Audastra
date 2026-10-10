#include "TriggerReplaceRealCase.h"
#include "TriggerReplaceDialog.h"
#include "TriggerModule.h"
#include "EditorProject.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Text/Utf8Scalar.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

using namespace War3Map;
using War3Map::Modules::TriggerReplaceDialog;
using War3Map::Modules::TriggerModule;

using namespace War3Map::Tests::TriggerReplaceReal;

// Qt GUI 默认日志可能没有 stdout；保留 Qt 原断言，同时逐失败发布有界阶段/原因。
#define REAL_VERIFY2(condition, message) do { \
    const bool observedPassed = static_cast<bool>(condition); \
    if (!observedPassed) ObserveFailure(receipt, QString::fromUtf8(message)); \
    QVERIFY2(observedPassed, message); \
} while (false)
#define REAL_VERIFY(condition) REAL_VERIFY2(condition, #condition)
#define REAL_COMPARE(actual, expected) do { \
    const auto observedActual = (actual); const auto observedExpected = (expected); \
    if (!(observedActual == observedExpected)) ObserveFailure(receipt, \
        QStringLiteral(#actual " == " #expected ": actual=") + ObservedValue(observedActual) \
            + QStringLiteral(", expected=") + ObservedValue(observedExpected)); \
    QCOMPARE(observedActual, observedExpected); \
} while (false)

// Qt 的静态资源入口必须在全局命名空间引用实际 qrc 名。
static void InitializeStyleResources() { Q_INIT_RESOURCE(war3map_uikit_style); }

class TriggerReplaceRealTests final : public QObject {
    Q_OBJECT
public:
    TriggerReplaceRealTests(QString map, QString ui, QString needle, QString target, QString expectedSha, double limit)
        : map_(std::move(map)), ui_(std::move(ui)), needle_(std::move(needle)), target_(std::move(target)),
          expectedSha_(std::move(expectedSha)), limitMs_(limit) {}
private slots:
    void OwnedLargeMapQtPreview256AndOneUndo();
private:
    QString map_, ui_, needle_, target_, expectedSha_;
    double limitMs_ = 1000;
};

void TriggerReplaceRealTests::OwnedLargeMapQtPreview256AndOneUndo() {
    Receipt receipt;
    receipt.data.insert("operation_limit_ms", limitMs_);
    receipt.data.insert("timing_gate", "maximum of every post-bind foreground phase and heartbeat gap <=1000ms; actual worker duration/queue/delivery reported separately, never average");
    receipt.data.insert("candidate", "real07-owned-async-preview");
    receipt.data.insert("cold_open_in_foreground_gate", false);
    receipt.data.insert("first_search_scope", "actual PrepareTriggerReplaceSources + QueryTriggerReplace + Populate + one Qt event drain");
    receipt.data.insert("snapshot_copy_scope", "separate public Strings Snapshot probe after product preparation; not a measured decomposition of first Search");
    receipt.data.insert("rss_initial_bytes", Rss()); receipt.data.insert("stage", "readonly-map-open");
    const QFileInfo source(map_), uiSource(ui_);
    REAL_VERIFY2(source.isAbsolute() && source.isFile() && source.size() >= 300ll * 1024 * 1024,
        "FORGE_NAVIGATION_OWNED_MAP must name an existing absolute large owned copy (at least 300 MiB)");
    REAL_VERIFY2(uiSource.isAbsolute() && uiSource.isDir(), "FORGE_YDWE_ROOT must name the existing absolute readonly catalog source");
    receipt.data.insert("map_bytes", QString::number(source.size()));
    QElapsedTimer setup; setup.start();
    auto opened = MapArchiveSession::Open(std::filesystem::path(map_.toStdWString()));
    receipt.data.insert("cold_map_open_ms", Milliseconds(setup));
    REAL_VERIFY2(opened.Succeeded() && opened.value, qPrintable(FailureText(opened.diagnostics)));
    const auto sourceSha = opened.value->SourceSha256();
    receipt.data.insert("map_sha256", QString::fromStdString(sourceSha));
    if (!expectedSha_.isEmpty()) REAL_COMPARE(QString::fromStdString(sourceSha), expectedSha_);
    setup.restart();
    const std::array<std::string, 2> triggerMembers{"war3map.wtg", "war3map.wct"};
    auto captured = opened.value->CaptureExactMembers(triggerMembers);
    receipt.data.insert("wtg_wct_source_capture_ms", Milliseconds(setup));
    REAL_VERIFY2(captured.Succeeded() && captured.value && captured.value->size() == 2, qPrintable(FailureText(captured.diagnostics)));
    REAL_VERIFY((*captured.value)[0].present && (*captured.value)[1].present);
    setup.restart();
    auto catalog = Trigger::UiCatalog::Load(std::filesystem::path(ui_.toStdWString()));
    REAL_VERIFY2(catalog.Succeeded() && catalog.value, qPrintable(FailureText(catalog.diagnostics)));
    auto parsed = Trigger::WtgWctCodec{}.Parse((*captured.value)[0].bytes, (*captured.value)[1].bytes, *catalog.value);
    REAL_VERIFY2(parsed.Succeeded() && parsed.value, qPrintable(FailureText(parsed.diagnostics)));
    receipt.data.insert("readonly_catalog_and_parse_ms", Milliseconds(setup));
    REAL_VERIFY(parsed.value->HasTrustedSourceSeal());
    receipt.data.insert("catalog_sha256", QString::fromStdString(catalog.value->Sha256()));
    receipt.data.insert("wtg_sha256", Utf8Display(parsed.value->SourceWtgSha256()));
    receipt.data.insert("wct_sha256", Utf8Display(parsed.value->SourceWctSha256()));
    receipt.data.insert("wtg_bytes", QString::number((*captured.value)[0].bytes.size()));
    receipt.data.insert("wct_bytes", QString::number((*captured.value)[1].bytes.size()));

    receipt.data.insert("stage", "accepted-typed-source-bind"); setup.restart();
    QtProject::EditorProject project;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath(); project.projectRoot_ = project.sourceArchivePath_.parent_path();
    project.uiDataRoot_ = project.activeUiDataRoot_ = std::filesystem::path(ui_.toStdWString());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.ResetEditTracking();
    REAL_VERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    std::size_t hostObserverCalls = 0;
    QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project, [&] {
        QElapsedTimer observer; observer.start(); ++hostObserverCalls;
        // 与实际 MainWindow indexChanged/Observe wrapper 相同的项目观察链；不能跳过仍有的全 Serialize。
        project.triggerDirty_ = project.restoredTriggerOverlay_ || !project.triggerUndoStack_->isClean();
        project.ObserveTriggerEdit(); project.UpdateEditDirtyFlags();
        receipt.timings[QStringLiteral("host_index_observer")].push_back(Milliseconds(observer));
    });
    receipt.data.insert("host_index_observer_connected", true);
    receipt.data.insert("host_index_observer_scope", "actual EditorProject observer/dirty chain, included in Apply/Undo/Redo; MainWindow chrome/recovery still outside this Module fixture");
    receipt.data.insert("accepted_source_bind_ms", Milliseconds(setup));
    const auto mapRevision = project.mapEdits_->Revision(), mapSession = project.mapEdits_->SessionIdentity();
    const auto generation = project.editRevision_.Generation();
    const auto* documentOwner = project.triggerDocument_.get();
    const auto wtgSha = std::string(documentOwner->SourceWtgSha256()), wctSha = std::string(documentOwner->SourceWctSha256());
    const auto catalogSha = project.uiCatalog_->Sha256();
    const auto originalArguments = documentOwner->argumentNodes;
    setup.restart();
    receipt.data.insert("stage", "baseline-serialize");
    const auto original = Trigger::WtgWctCodec{}.Serialize(*documentOwner, *project.uiCatalog_);
    receipt.data.insert("baseline_serialize_ms_outside_ui_gate", Milliseconds(setup));
    if (!original.Succeeded() || !original.value) ObserveFailure(receipt, QStringLiteral("Baseline Serialize failed: ") + FailureText(original.diagnostics));
    REAL_VERIFY2(original.Succeeded() && original.value, qPrintable(FailureText(original.diagnostics)));
    receipt.data.insert("stage", "baseline-source-byte-equality");
    const bool baselineWtgExact = original.value->wtg == (*captured.value)[0].bytes;
    const bool baselineWctExact = original.value->wct == (*captured.value)[1].bytes;
    receipt.data.insert("baseline_wtg_exact", baselineWtgExact); receipt.data.insert("baseline_wct_exact", baselineWctExact);
    receipt.data.insert("baseline_encoded_wtg_bytes", QString::number(original.value->wtg.size()));
    receipt.data.insert("baseline_encoded_wct_bytes", QString::number(original.value->wct.size()));
    if (!baselineWtgExact || !baselineWctExact) ObserveFailure(receipt,
        QStringLiteral("Baseline encoded bytes differ from accepted source: WTG exact=%1, WCT exact=%2")
            .arg(baselineWtgExact ? QStringLiteral("true") : QStringLiteral("false"),
                baselineWctExact ? QStringLiteral("true") : QStringLiteral("false")));
    REAL_VERIFY(baselineWtgExact && baselineWctExact);
    receipt.data.insert("stage", "choose-metadata-source-target");
    QJsonObject choiceStatistics;
    setup.restart(); QString choiceReason;
    const auto preparationRevision = std::to_string(generation) + "/" + std::to_string(project.editRevision_.Revision())
        + "/" + std::to_string(mapSession) + "/" + std::to_string(mapRevision) + "/" + catalogSha;
    const auto selectedCase = Choose(*documentOwner, *project.uiCatalog_, preparationRevision, needle_.toUtf8().toStdString(),
        target_.toUtf8().toStdString(), choiceStatistics, choiceReason);
    receipt.data.insert("readonly_case_preparation_ms_outside_ui_gate", Milliseconds(setup));
    receipt.data.insert("readonly_case_preparation_in_foreground_gate", false);
    receipt.data.insert("choice_metadata", choiceStatistics);
    if (!selectedCase) ObserveFailure(receipt, choiceReason);
    REAL_VERIFY2(selectedCase.has_value(), qPrintable(choiceReason));
    const auto choice = *selectedCase;
    receipt.data.insert("selected_target_id", QString::number(choice.targetId));
    receipt.data.insert("selected_type", Utf8Display(choice.type));
    receipt.data.insert("query_needle", Utf8Display(choice.needle));
    receipt.data.insert("distinct_source_eca_owners", QString::number(choice.sourceEcas));
    REAL_VERIFY(choice.rows.size() == 256 && choice.sourceEcas >= 2);
    receipt.data.insert("actual_argument_records", QString::number(originalArguments.size()));

    setup.restart();
    TriggerModule module(project); QWidget* page = module.Widget(); REAL_VERIFY(page);
    receipt.data.insert("initial_module_bind_ms_outside_ui_gate", Milliseconds(setup));
    receipt.data.insert("rss_post_bind_bytes", Rss()); receipt.data.insert("stage", "show-find-replace");
    for (int round = 0; round < 3; ++round) Measure(receipt, QStringLiteral("show_find_replace"), [&] { module.ShowFindReplace(); });
    QPointer<TriggerReplaceDialog> dialog(dynamic_cast<TriggerReplaceDialog*>(page->findChild<QDialog*>(QStringLiteral("triggerReplaceDialog"))));
    REAL_VERIFY(dialog && dialog->isVisible() && !dialog->isModal());
    struct WorkerCleanup {
        QPointer<TriggerReplaceDialog> dialog;
        ~WorkerCleanup() {
            if (dialog) dialog->CancelPreview();
            // 异常/断言提前退出也请求阶段取消；不强杀线程。超时由独立测试进程的外层 deadline 处理。
            if (!QThreadPool::globalInstance()->waitForDone(5000)) {
                std::fputs("TRIGGER_REPLACE_REAL_CLEANUP owned worker still stopping after 5000ms\n", stdout); std::fflush(stdout);
            }
        }
    } workerCleanup{dialog};
    auto* scope = Control<QComboBox>(*dialog, "triggerReplaceScope");
    auto* mode = Control<QComboBox>(*dialog, "triggerReplaceMode");
    auto* match = Control<QComboBox>(*dialog, "triggerReplaceMatch");
    auto* query = Control<QLineEdit>(*dialog, "triggerReplaceQuery");
    auto* tree = Control<QTreeWidget>(*dialog, "triggerReplaceResults");
    auto* search = Control<QPushButton>(*dialog, "triggerReplaceSearch");
    auto* next = Control<QPushButton>(*dialog, "triggerReplaceNext");
    auto* status = Control<QLabel>(*dialog, "triggerReplaceStatus");
    REAL_VERIFY(scope && mode && match && query && tree && search && next && status);
    scope->setCurrentIndex(0); mode->setCurrentIndex(mode->findData(static_cast<int>(Services::TriggerReplaceMode::ParameterValue)));
    match->setCurrentIndex(match->findData(static_cast<int>(Services::TriggerReplaceMatch::Exact)));
    query->setText(Utf8Display(choice.needle));
    receipt.data.insert("stage", "first-search-preparation-query");
    Measure(receipt, QStringLiteral("first_search_including_prepare"), [&] { search->click(); });
    REAL_VERIFY2(dialog && tree->topLevelItemCount() > 0, qPrintable(status->text()));
    receipt.data.insert("first_query_status", status->text());
    for (int round = 0; round < 3; ++round) Measure(receipt, QStringLiteral("warm_search"), [&] { search->click(); });
    REAL_VERIFY2(tree->topLevelItemCount() > 0, qPrintable(status->text()));
    REAL_COMPARE(project.triggerUndoStack_->count(), 0); REAL_COMPARE(project.mapEdits_->Revision(), mapRevision);

    receipt.data.insert("stage", "separate-readonly-snapshot-copy");
    for (int round = 0; round < 3; ++round) {
        const auto snapshot = Measure(receipt, QStringLiteral("separate_strings_snapshot_copy"), [&] {
            return project.mapEdits_->Snapshot(MapDataKind::Strings, Services::TriggerReplaceLimits{}.maxNameTextBytes);
        });
        REAL_VERIFY2(snapshot.Succeeded() && snapshot.value && snapshot.value->Strings(), qPrintable(FailureText(snapshot.diagnostics)));
        std::uint64_t bytes = 0;
        for (const auto& member : snapshot.value->Members()) for (const auto& raw : member.sources) bytes += raw.data.size();
        receipt.data.insert("separate_strings_snapshot_owned_raw_bytes", QString::number(bytes));
        receipt.data.insert("separate_strings_snapshot_entries", QString::number(snapshot.value->Strings()->entries.size()));
    }
    receipt.data.insert("rss_after_prepare_bytes", Rss());

    auto* filter = Control<QLineEdit>(*dialog, "triggerReplaceTargetFilter");
    auto* targetSearch = Control<QPushButton>(*dialog, "triggerReplaceTargetSearch");
    auto* targetNext = Control<QPushButton>(*dialog, "triggerReplaceTargetNext");
    auto* targets = Control<QComboBox>(*dialog, "triggerReplaceTarget");
    auto* targetStatus = Control<QLabel>(*dialog, "triggerReplaceTargetStatus");
    REAL_VERIFY(filter && targetSearch && targetNext && targets && targetStatus);
    filter->setText(Utf8Display(choice.targetName)); receipt.data.insert("stage", "actual-typed-target-options");
    for (int round = 0; round < 3; ++round) Measure(receipt, QStringLiteral("typed_target_query"), [&] { targetSearch->click(); });
    const QString targetPrefix = Utf8Display(choice.targetName) + QStringLiteral(" · ");
    int targetIndex = -1;
    for (std::size_t targetPage = 0; targetPage < 256; ++targetPage) {
        for (int i = 1; i < targets->count(); ++i) if (targets->itemText(i).startsWith(targetPrefix)) {
            REAL_VERIFY2(targetIndex < 0, "The displayed exact target is ambiguous"); targetIndex = i;
        }
        if (targetIndex >= 0 || !targetNext->isEnabled()) break;
        Measure(receipt, QStringLiteral("typed_target_next_page"), [&] { targetNext->click(); });
    }
    REAL_VERIFY2(targetIndex > 0, qPrintable(targetStatus->text())); targets->setCurrentIndex(targetIndex);
    receipt.data.insert("typed_target_actual_label", targets->currentText());
    REAL_COMPARE(project.triggerUndoStack_->count(), 0);

    receipt.data.insert("stage", "actual-cross-page-selection");
    std::set<StableId> selected;
    std::size_t pages = 0; QString selectionMismatch;
    while (selected.size() < 256 && pages < 128) {
        ++pages;
        Measure(receipt, QStringLiteral("select_page_items"), [&] {
            for (int i = 0; i < tree->topLevelItemCount() && selected.size() < 256; ++i) {
                auto* item = tree->topLevelItem(i);
                if (!(item->flags() & Qt::ItemIsUserCheckable)) continue;
                const auto id = DisplayedArgumentId(item->text(1));
                if (!id || selected.contains(*id) || !choice.rows.contains(*id)) continue;
                const auto& before = choice.rows.at(*id);
                if (item->text(2) != Utf8Display(before.oldValue) || item->text(3) != Utf8Display(before.declaredType)) {
                    selectionMismatch = QStringLiteral("Actual UI row no longer matches qualified source for argument%1; old/type must remain exact.").arg(*id);
                    continue;
                }
                item->setCheckState(0, Qt::Checked); selected.insert(*id);
            }
        });
        if (selected.size() == 256 || !next->isEnabled()) break;
        Measure(receipt, QStringLiteral("query_next_page"), [&] { next->click(); });
    }
    receipt.data.insert("actual_query_pages_used", QString::number(pages)); receipt.data.insert("selected_count", QString::number(selected.size()));
    REAL_VERIFY2(selectionMismatch.isEmpty(), qPrintable(selectionMismatch));
    REAL_VERIFY2(selected.size() == 256 && pages > 1, qPrintable(status->text()));
    for (const auto& [id, before] : choice.rows) { (void)before; REAL_VERIFY(selected.contains(id)); }
    auto* previewButton = Control<QPushButton>(*dialog, "triggerReplacePreview");
    auto* previewRows = Control<QTreeWidget>(*dialog, "triggerReplacePreviewRows");
    auto* apply = Control<QPushButton>(*dialog, "triggerReplaceApply");
    REAL_VERIFY(previewButton && previewRows && apply && previewButton->isEnabled());
    receipt.data.insert("stage", "shared-opaque-async-preview-256");
    QJsonArray asyncSamples;
    for (int round = 0; round < 3; ++round) {
        REAL_VERIFY(dialog && previewButton->isEnabled() && !dialog->PreviewPending());
        QElapsedTimer requestTimer; requestTimer.start();
        qint64 lastHeartbeat = 0, maximumGap = 0; std::size_t heartbeatCount = 0;
        const auto beat = [&] {
            const auto now = requestTimer.nsecsElapsed(); maximumGap = (std::max)(maximumGap, now - lastHeartbeat);
            lastHeartbeat = now; ++heartbeatCount;
        };
        QTimer heartbeat; heartbeat.setInterval(5); heartbeat.setTimerType(Qt::PreciseTimer);
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, beat); heartbeat.start();
        Measure(receipt, QStringLiteral("preview_schedule_gui_256"), [&] { previewButton->click(); });
        const auto scheduledAt = requestTimer.nsecsElapsed();
        receipt.data.insert("stage", QStringLiteral("async-preview-wait-round-%1").arg(round + 1));
        while (dialog && dialog->PreviewPending() && requestTimer.elapsed() < 30000) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QTest::qWait(1);
        }
        // 包括调度前沿、每次回调间隔、最终 adoption 后沿，不用平均心跳掩盖一次停顿。
        const auto completedAt = requestTimer.nsecsElapsed(); maximumGap = (std::max)(maximumGap, completedAt - lastHeartbeat); heartbeat.stop();
        QJsonObject sample{{"round", round + 1}, {"request_until_adoption_ms", static_cast<double>(completedAt) / 1000000.0},
            {"wait_after_gui_schedule_ms", static_cast<double>(completedAt - scheduledAt) / 1000000.0},
            {"heartbeat_count", QString::number(heartbeatCount)}, {"heartbeat_maximum_gap_ms", static_cast<double>(maximumGap) / 1000000.0}};
        if (dialog) if (const auto timing = dialog->LastPreviewTiming()) {
            sample.insert("gui_plan_ms", static_cast<double>(timing->planNanoseconds) / 1000000.0);
            sample.insert("gui_owned_clone_ms", static_cast<double>(timing->cloneNanoseconds) / 1000000.0);
            sample.insert("worker_actual_ms", timing->workerNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->workerNanoseconds.load()) / 1000000.0));
            sample.insert("worker_queue_ms", timing->queueNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->queueNanoseconds.load()) / 1000000.0));
            sample.insert("result_delivery_and_adoption_ms", timing->deliveryNanoseconds.load() < 0 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(static_cast<double>(timing->deliveryNanoseconds.load()) / 1000000.0));
        }
        asyncSamples.push_back(sample); receipt.data.insert("async_preview_samples", asyncSamples);
        REAL_VERIFY2(dialog && !dialog->PreviewPending(), "Owned background preparation did not finish within the separate 30000ms test deadline");
        REAL_VERIFY2(previewRows->topLevelItemCount() == 256 && apply->isEnabled(), qPrintable(status->text()));
        const auto timing = dialog->LastPreviewTiming();
        REAL_VERIFY2(timing && timing->workerNanoseconds.load() >= 0 && timing->queueNanoseconds.load() >= 0 && timing->deliveryNanoseconds.load() >= 0,
            "Successful actual Module preview must have completed owned-worker timing; missing/cancelled/stale duration is Unknown");
        REAL_VERIFY(timing->planNanoseconds >= 0 && timing->cloneNanoseconds >= 0);
        receipt.timings[QStringLiteral("preview_plan_gui_256")].push_back(static_cast<double>(timing->planNanoseconds) / 1000000.0);
        receipt.timings[QStringLiteral("preview_owned_clone_gui_256")].push_back(static_cast<double>(timing->cloneNanoseconds) / 1000000.0);
        REAL_VERIFY2(heartbeatCount > 0, "The actual event-loop heartbeat did not run while the large-map preview was being prepared");
        REAL_VERIFY2(static_cast<double>(maximumGap) / 1000000.0 <= limitMs_, "Actual event-loop maximum gap exceeded the unchanged finite <=1000ms foreground gate");
        REAL_COMPARE(project.triggerUndoStack_->count(), 0); REAL_COMPARE(project.mapEdits_->Revision(), mapRevision);
    }
    REAL_VERIFY2(previewRows->topLevelItemCount() == 256 && apply->isEnabled(), qPrintable(status->text()));
    std::set<StableId> previewIds;
    const auto replacement = previewRows->topLevelItem(0)->text(2).toUtf8().toStdString();
    REAL_VERIFY(replacement == choice.targetName || replacement == "udg_" + choice.targetName);
    for (int i = 0; i < previewRows->topLevelItemCount(); ++i) {
        const auto* row = previewRows->topLevelItem(i); const auto id = DisplayedArgumentId(row->text(0));
        REAL_VERIFY(id && previewIds.insert(*id).second);
        REAL_VERIFY(choice.rows.contains(*id));
        REAL_COMPARE(row->text(1), Utf8Display(choice.rows.at(*id).oldValue)); REAL_COMPARE(row->text(2), Utf8Display(replacement));
        REAL_COMPARE(row->text(3), Utf8Display(choice.type));
    }
    REAL_VERIFY(previewIds == selected); REAL_COMPARE(project.triggerUndoStack_->count(), 0);
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
    receipt.data.insert("rss_after_preview_bytes", Rss());

    receipt.data.insert("stage", "real-one-snapshot-undo-publication");
    Measure(receipt, QStringLiteral("apply_256_one_snapshot_undo"), [&] { apply->click(); });
    REAL_COMPARE(project.triggerUndoStack_->count(), 1); REAL_COMPARE(project.triggerUndoStack_->index(), 1);
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, true));
    REAL_VERIFY(project.triggerDocument_.get() == documentOwner && project.editRevision_.Generation() == generation);
    for (int round = 0; round < 3; ++round) {
        Measure(receipt, QStringLiteral("undo_256"), [&] { project.triggerUndoStack_->undo(); });
        REAL_COMPARE(project.triggerUndoStack_->index(), 0);
        REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
        if (round == 0) {
            setup.restart(); const auto undone = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
            receipt.data.insert("undo_wire_verification_ms_outside_ui_gate", Milliseconds(setup));
            REAL_VERIFY2(undone.Succeeded() && undone.value, qPrintable(FailureText(undone.diagnostics)));
            REAL_VERIFY(undone.value->wtg == original.value->wtg && undone.value->wct == original.value->wct);
        }
        Measure(receipt, QStringLiteral("redo_256"), [&] { project.triggerUndoStack_->redo(); });
        REAL_COMPARE(project.triggerUndoStack_->index(), 1); REAL_COMPARE(project.triggerUndoStack_->count(), 1);
        REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, true));
    }
    Measure(receipt, QStringLiteral("final_undo_256"), [&] { project.triggerUndoStack_->undo(); });
    REAL_VERIFY(ArgumentsMatch(originalArguments, *project.triggerDocument_, selected, replacement, false));
    REAL_COMPARE(project.triggerUndoStack_->count(), 1); REAL_COMPARE(project.triggerUndoStack_->index(), 0);
    REAL_VERIFY(project.triggerUndoStack_->canRedo());
    REAL_COMPARE(project.mapEdits_->Revision(), mapRevision); REAL_COMPARE(project.mapEdits_->SessionIdentity(), mapSession);
    REAL_COMPARE(std::string(project.triggerDocument_->SourceWtgSha256()), wtgSha);
    REAL_COMPARE(std::string(project.triggerDocument_->SourceWctSha256()), wctSha);
    REAL_COMPARE(project.uiCatalog_->Sha256(), catalogSha);
    for (const auto* stack : project.objectUndoStacks_) if (stack) REAL_COMPARE(stack->count(), 0);
    receipt.data.insert("stage", "readonly-source-final-verification"); setup.restart();
    const auto unchanged = project.archiveSession_->VerifySourceUnchanged();
    receipt.data.insert("source_final_sha_verification_ms_outside_ui_gate", Milliseconds(setup));
    REAL_VERIFY2(unchanged.Succeeded(), qPrintable(FailureText(unchanged.diagnostics)));
    receipt.data.insert("source_unchanged", true); receipt.data.insert("wtg_wct_undo_byte_exact", true);
    receipt.data.insert("one_undo_command", true); receipt.data.insert("snapshot_probe_is_separate_measurement", true);
    receipt.data.insert("host_index_observer_calls", QString::number(hostObserverCalls)); REAL_COMPARE(hostObserverCalls, std::size_t{8});
    receipt.data.insert("stage", "maximum-latency-gate");
    for (const auto& [name, samples] : receipt.timings) {
        REAL_VERIFY(!samples.empty());
        REAL_VERIFY2(*std::max_element(samples.begin(), samples.end()) <= limitMs_,
            qPrintable(name + QStringLiteral(" exceeded the finite <=1000ms maximum foreground gate; report the slow stage without widening it")));
    }
    receipt.data.insert("stage", "complete"); receipt.data.insert("passed", true);
}

int main(int argc, char** argv) {
    const auto map = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP"), ui = qEnvironmentVariable("FORGE_YDWE_ROOT");
    if (map.isEmpty() || ui.isEmpty()) {
        std::fputs("TRIGGER_REPLACE_REAL_RECEIPT {\"schema\":1,\"skipped\":true,\"reason\":\"FORGE_NAVIGATION_OWNED_MAP and FORGE_YDWE_ROOT are required readonly sources\"}\n", stdout);
        return 77;
    }
    if (!qEnvironmentVariable("FORGE_TRIGGER_REPLACE_SOURCE_VARIABLE").isEmpty()) {
        std::fputs("The256-edit case uses ParameterValue Exact across actual existing scalar slots; SOURCE_VARIABLE filtering is unsupported, use FORGE_TRIGGER_REPLACE_QUERY_NEEDLE for an actual nonempty exact raw value.\n", stderr); return 2;
    }
    const auto requestedNeedle = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_QUERY_NEEDLE");
    const auto needle = requestedNeedle; // 未指定时只从当前已持有的真实原参数值派生，不猜常量。
    double limit = 1000;
    const auto requestedLimit = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_MAX_OPERATION_MS");
    if (!requestedLimit.isEmpty()) {
        bool valid = false; limit = requestedLimit.toDouble(&valid);
        if (!valid || !std::isfinite(limit) || limit <= 0 || limit > 1000) {
            std::fputs("FORGE_TRIGGER_REPLACE_MAX_OPERATION_MS may only tighten a finite positive 1000ms gate\n", stderr); return 2;
        }
    }
    const auto expectedSha = qEnvironmentVariable("FORGE_TRIGGER_REPLACE_MAP_SHA256");
    if (!expectedSha.isEmpty() && (expectedSha.size() != 64 || !std::all_of(expectedSha.begin(), expectedSha.end(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    }))) { std::fputs("FORGE_TRIGGER_REPLACE_MAP_SHA256 must be a lowercase 64-hex identity\n", stderr); return 2; }
    QTemporaryDir settings(QDir::tempPath() + QStringLiteral("/AudastraTriggerReplaceReal-XXXXXX"));
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraOwnedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TriggerReplaceRealTests"));
    QApplication application(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        std::fputs("TriggerReplaceRealTests requires QT_QPA_PLATFORM=offscreen\n", stderr); return 2;
    }
    auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")); if (!fusion) return 2;
    QApplication::setStyle(fusion); InitializeStyleResources();
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    TriggerReplaceRealTests tests(map, ui, needle,
        qEnvironmentVariable("FORGE_TRIGGER_REPLACE_TARGET_VARIABLE"), expectedSha, limit);
    return QTest::qExec(&tests, argc, argv);
}

#include "TriggerReplaceRealTests.moc"
