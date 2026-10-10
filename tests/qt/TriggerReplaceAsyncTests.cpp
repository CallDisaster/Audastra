#include "TriggerModule.h"
#include "TriggerReplaceDialog.h"
#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPromise>
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
#include <filesystem>
#include <stdexcept>
#include <utility>

using namespace War3Map;
using namespace War3Map::Services;
using War3Map::Modules::TriggerReplaceDialog;

static void InitializeStyle() { Q_INIT_RESOURCE(war3map_uikit_style); }

namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<class T> T Take(Result<T>&& result) {
    if (!result.Succeeded() || !result.value) {
        std::string message; for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
struct Wire {
    std::vector<std::byte> bytes;
    void I32(std::int32_t value) {
        for (int shift = 0; shift != 32; shift += 8) bytes.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 255U));
    }
    void String(std::string_view value) { for (char c : value) bytes.push_back(static_cast<std::byte>(c)); bytes.push_back(std::byte{}); }
};
std::vector<std::byte> Wtg() {
    Wire w; for (char c : std::string_view("WTG!")) w.bytes.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(1); w.I32(1); w.String("Owned async fixture"); w.I32(0); w.I32(2); w.I32(2);
    for (const auto* name : {"Count", "NextCount"}) { w.String(name); w.String("integer"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.String(""); }
    w.I32(1); w.String("Owned GUI"); w.String(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1); w.I32(64);
    for (int i = 0; i != 64; ++i) { w.I32(2); w.String("UseInt"); w.I32(1); w.I32(1); w.String("Count"); w.I32(0); w.I32(0); w.I32(0); }
    return std::move(w.bytes);
}
std::vector<std::byte> Wct() { Wire w; w.I32(1); w.String(""); w.I32(0); w.I32(1); w.I32(0); return std::move(w.bytes); }
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    Require(QDir().mkpath(QString::fromStdWString(path.parent_path().wstring())), "owned fixture directory");
    QFile file(QString::fromStdWString(path.wstring())); Require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "new owned fixture");
    Require(file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()), "owned fixture bytes");
}
void Write(const std::filesystem::path& path, std::string_view bytes) { Write(path, std::as_bytes(std::span(bytes.data(), bytes.size()))); }
QPushButton* Button(QWidget& widget, const char* name) { return widget.findChild<QPushButton*>(QString::fromLatin1(name)); }
QTreeWidget* Tree(QWidget& widget, const char* name) { return widget.findChild<QTreeWidget*>(QString::fromLatin1(name)); }
QLineEdit* Edit(QWidget& widget, const char* name) { return widget.findChild<QLineEdit*>(QString::fromLatin1(name)); }
void FindAndChoose(TriggerReplaceDialog& dialog) {
    Edit(dialog, "triggerReplaceQuery")->setText(QStringLiteral("Count")); Button(dialog, "triggerReplaceSearch")->click();
    Edit(dialog, "triggerReplaceTargetFilter")->setText(QStringLiteral("NextCount")); Button(dialog, "triggerReplaceTargetSearch")->click();
    auto* targets = dialog.findChild<QComboBox*>(QStringLiteral("triggerReplaceTarget"));
    Require(targets && targets->count() == 2, "real unique typed target"); targets->setCurrentIndex(1);
    Button(dialog, "triggerReplaceCheckPage")->click();
}
struct Fixture {
    QTemporaryDir temp;
    QtProject::EditorProject project;
    std::unique_ptr<Modules::TriggerModule> module;
    std::unique_ptr<Trigger::TriggerTypeSystem> types;
    Trigger::EncodedTriggerDocument original;
    std::filesystem::path root;
    std::string wtgSha, wctSha;
    QString error;
    bool Initialize(bool makeModule) {
        try {
            Require(temp.isValid(), "owned temporary root"); root = std::filesystem::path(temp.path().toStdWString());
            const auto ui = root / "catalog";
            Write(ui / "ui/config", "base\n"); Write(ui / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
            Write(ui / "ui/base/action.txt", "[UseInt]\ndescription=Use ${value}\n[[.args]]\ntype=integer\n");
            auto catalog = Take(Trigger::UiCatalog::Load(ui)); auto document = Take(Trigger::WtgWctCodec{}.Parse(Wtg(), Wct(), catalog));
            original = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog));
            Write(root / "source/map/war3map.wtg", original.wtg); Write(root / "source/map/war3map.wct", original.wct);
            wtgSha = Take(Sha256File(root / "source/map/war3map.wtg")); wctSha = Take(Sha256File(root / "source/map/war3map.wct"));
            project.projectRoot_ = root / "source";
            project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(project.projectRoot_)));
            project.uiDataRoot_ = project.activeUiDataRoot_ = ui;
            project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(catalog));
            project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(document));
            types = std::make_unique<Trigger::TriggerTypeSystem>(*project.uiCatalog_);
            project.triggerUndoStack_ = new QUndoStack(&project);
            QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project, [this] { project.ObserveTriggerEdit(); });
            project.ResetEditTracking(); Require(project.editTrackingReady_ && !project.editTrackingFailed_, "real edit tracking");
            if (makeModule) {
                module = std::make_unique<Modules::TriggerModule>(project); module->Widget();
                Require(module->Reveal({"triggers", "trigger", QString::number(project.triggerDocument_->triggers.front().id), {}}), "real stable trigger selection");
                module->ShowFindReplace();
            }
            return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }
    TriggerReplaceDialog* Dialog() { return dynamic_cast<TriggerReplaceDialog*>(module->Widget()->findChild<QDialog*>(QStringLiteral("triggerReplaceDialog"))); }
    TriggerReplaceContext Context(std::string revision) const {
        TriggerReplaceContext context; context.revision = std::move(revision); context.document = project.triggerDocument_.get();
        context.catalog = project.uiCatalog_.get(); context.types = types.get(); return context;
    }
    bool Unchanged() const {
        const auto encoded = Take(Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_));
        return original.wtg == encoded.wtg && original.wct == encoded.wct && project.triggerUndoStack_->count() == 0
            && wtgSha == Take(Sha256File(root / "source/map/war3map.wtg")) && wctSha == Take(Sha256File(root / "source/map/war3map.wct"));
    }
};
// 手动 Future 只控制交付时序；结果仍来自真实 Query/Plan/Complete，不伪造 opaque preview。
struct Controlled {
    Fixture& fixture;
    std::string revision = "owned/1";
    std::unique_ptr<TriggerReplaceDialog> dialog;
    std::shared_ptr<QPromise<Result<TriggerReplacePreviewResult>>> promise;
    std::shared_ptr<std::stop_source> stop;
    std::shared_ptr<TriggerReplaceDialog::PreviewTiming> timing;
    std::optional<Result<TriggerReplacePreviewResult>> result;
    int schedules = 0, applies = 0;
    std::function<void()> duringSchedule;
    std::function<void()> duringRevision;
    explicit Controlled(Fixture& f) : fixture(f) {
        dialog = std::make_unique<TriggerReplaceDialog>([this] {
            const auto owned = revision; if (duringRevision) { const auto callback = std::exchange(duringRevision, {}); callback(); } return owned;
        }, [] { return Status::Ok(); }, [this](auto expected, const auto& query, const auto& limits, const auto* cursor) {
            return QueryTriggerReplace(fixture.Context(revision), expected, query, limits, cursor);
        }, [this](auto expected, auto kind, auto filter, const auto& limits, const auto* cursor) {
            return QueryTriggerReplacementOptions(fixture.Context(revision), expected, kind, filter, limits, cursor);
        }, TriggerReplaceDialog::Preview{}, [this](auto, const auto&, const auto&) { ++applies; return Status::Ok(); },
            std::function<void(const TriggerReplaceHit&)>{});
        dialog->SetAsyncPreview([this](auto expected, const auto& query, const auto& proof, auto keys, const auto& replacement, const auto& limits) {
            ++schedules; promise = std::make_shared<QPromise<Result<TriggerReplacePreviewResult>>>(); promise->start(); stop = std::make_shared<std::stop_source>();
            const auto planned = Take(PrepareTriggerReplacementPlan(fixture.Context(revision), expected, query, proof, keys, replacement, limits));
            Require(planned.plan.has_value(), "actual service plan");
            result = CompleteTriggerReplacementPlan(*planned.plan, *fixture.project.triggerDocument_, *fixture.project.uiCatalog_, limits);
            Require(result->Succeeded() && result->value && result->value->preview.has_value(), "actual service completed preview");
            timing = std::make_shared<TriggerReplaceDialog::PreviewTiming>();
            TriggerReplaceDialog::PreviewTask task{promise->future(), stop, timing};
            if (duringSchedule) { const auto callback = std::exchange(duringSchedule, {}); callback(); }
            return Result<TriggerReplaceDialog::PreviewTask>::Success(std::move(task));
        });
        dialog->show(); FindAndChoose(*dialog);
    }
    void Finish() { Require(promise && result.has_value(), "scheduled future"); promise->addResult(std::move(*result)); result.reset(); promise->finish(); }
};
} // namespace

class TriggerReplaceAsyncTests final : public QObject {
    Q_OBJECT
private slots:
    void ActualModuleUsesOwnedWorkerAndOneUndo();
    void CancelKeepsSingleFlightUntilFinished();
    void HiddenAndChangedSourcesDiscardLateValues();
    void ProviderAndRevisionMayDeleteDialog();
    void ActualModuleMayBeDestroyedWithOwnedWork();
    void CompletionErrorsKeepTimingUnknown();
};

void TriggerReplaceAsyncTests::ActualModuleUsesOwnedWorkerAndOneUndo() {
    Fixture f; QVERIFY2(f.Initialize(true), qPrintable(f.error)); auto* dialog = f.Dialog(); QVERIFY(dialog); FindAndChoose(*dialog);
    Button(*dialog, "triggerReplaceNext")->click(); Button(*dialog, "triggerReplaceCheckPage")->click();
    QCOMPARE(Tree(*dialog, "triggerReplaceResults")->topLevelItemCount(), 14);
    Button(*dialog, "triggerReplacePreview")->click(); QVERIFY(dialog->PreviewPending());
    QVERIFY(!Button(*dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(!Button(*dialog, "triggerReplacePreview")->isEnabled()); QVERIFY(f.Unchanged());
    bool heartbeat = false; QTimer::singleShot(0, this, [&] { heartbeat = true; });
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 5000); QTRY_VERIFY_WITH_TIMEOUT(!dialog->PreviewPending(), 5000);
    QCOMPARE(Tree(*dialog, "triggerReplacePreviewRows")->topLevelItemCount(), 64);
    const auto timing = dialog->LastPreviewTiming(); QVERIFY(timing);
    QVERIFY(timing->planNanoseconds > 0 && timing->cloneNanoseconds > 0 && timing->workerNanoseconds.load() > 0);
    QVERIFY(timing->queueNanoseconds.load() >= 0 && timing->deliveryNanoseconds.load() >= 0);
    QVERIFY2(Button(*dialog, "triggerReplaceApply")->isEnabled(), qPrintable(dialog->findChild<QLabel*>("triggerReplaceStatus")->text()));
    Button(*dialog, "triggerReplaceApply")->click(); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    QVERIFY(std::all_of(f.project.triggerDocument_->argumentNodes.begin(), f.project.triggerDocument_->argumentNodes.end(),
        [](const auto& argument) { return argument.type == 1 && argument.value.text == "NextCount"; }));
    f.project.triggerUndoStack_->undo();
    const auto encoded = Take(Trigger::WtgWctCodec{}.Serialize(*f.project.triggerDocument_, *f.project.uiCatalog_));
    QVERIFY(encoded.wtg == f.original.wtg && encoded.wct == f.original.wct);
    f.project.triggerUndoStack_->redo(); QVERIFY(f.wtgSha == Take(Sha256File(f.root / "source/map/war3map.wtg")));
    QVERIFY(f.wctSha == Take(Sha256File(f.root / "source/map/war3map.wct")));
}
void TriggerReplaceAsyncTests::CancelKeepsSingleFlightUntilFinished() {
    Fixture f; QVERIFY2(f.Initialize(false), qPrintable(f.error)); Controlled h(f); auto& dialog = *h.dialog;
    Button(dialog, "triggerReplacePreview")->click(); QVERIFY(dialog.PreviewPending()); QCOMPARE(h.schedules, 1);
    Button(dialog, "triggerReplaceCancelPreview")->click(); QVERIFY(h.stop->stop_requested()); QVERIFY(dialog.PreviewPending());
    QVERIFY(!dialog.LastPreviewTiming());
    QVERIFY(!Button(dialog, "triggerReplacePreview")->isEnabled()); Button(dialog, "triggerReplacePreview")->click(); QCOMPARE(h.schedules, 1);
    h.Finish(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.PreviewPending(), 5000);
    QVERIFY(!Button(dialog, "triggerReplaceApply")->isEnabled()); QCOMPARE(Tree(dialog, "triggerReplacePreviewRows")->topLevelItemCount(), 0);
    QCOMPARE(h.applies, 0); QVERIFY(f.Unchanged());
    QVERIFY(!dialog.LastPreviewTiming());
    Button(dialog, "triggerReplacePreview")->click(); QCOMPARE(h.schedules, 2); h.Finish();
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.PreviewPending(), 5000); QVERIFY(Button(dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged());
    QVERIFY(dialog.LastPreviewTiming()); dialog.ContextChanged(); QVERIFY(!dialog.LastPreviewTiming());
}
void TriggerReplaceAsyncTests::HiddenAndChangedSourcesDiscardLateValues() {
    Fixture f; QVERIFY2(f.Initialize(false), qPrintable(f.error)); Controlled h(f);
    Button(*h.dialog, "triggerReplacePreview")->click(); h.dialog->hide(); QVERIFY(h.stop->stop_requested()); h.Finish();
    QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000); QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled());
    h.dialog->show(); FindAndChoose(*h.dialog); Button(*h.dialog, "triggerReplacePreview")->click();
    h.revision = "owned/2"; h.Finish(); QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000);
    QCOMPARE(Tree(*h.dialog, "triggerReplacePreviewRows")->topLevelItemCount(), 0); QCOMPARE(Tree(*h.dialog, "triggerReplaceResults")->topLevelItemCount(), 0);
    QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged());
    QVERIFY(!h.dialog->LastPreviewTiming());
    FindAndChoose(*h.dialog); Button(*h.dialog, "triggerReplacePreview")->click(); h.dialog->ContextChanged(); QVERIFY(h.stop->stop_requested()); h.Finish();
    QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000); QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged());
}
void TriggerReplaceAsyncTests::ProviderAndRevisionMayDeleteDialog() {
    Fixture f; QVERIFY2(f.Initialize(false), qPrintable(f.error));
    { Controlled h(f); QPointer<TriggerReplaceDialog> owner(h.dialog.get());
      h.duringSchedule = [&] { h.dialog.reset(); }; Button(*h.dialog, "triggerReplacePreview")->click();
      QVERIFY(owner.isNull()); QVERIFY(h.stop->stop_requested()); h.Finish(); QApplication::processEvents(); QVERIFY(f.Unchanged()); }
    { Controlled h(f); QPointer<TriggerReplaceDialog> owner(h.dialog.get()); Button(*h.dialog, "triggerReplacePreview")->click();
      h.duringRevision = [&] { h.dialog.reset(); }; h.Finish(); QTRY_VERIFY_WITH_TIMEOUT(owner.isNull(), 5000); QVERIFY(f.Unchanged()); }
    { Controlled h(f); Button(*h.dialog, "triggerReplacePreview")->click();
      h.duringRevision = [&] { h.dialog->ContextChanged(); }; h.Finish(); QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000);
      QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged()); }
}
void TriggerReplaceAsyncTests::ActualModuleMayBeDestroyedWithOwnedWork() {
    Fixture f; QVERIFY2(f.Initialize(true), qPrintable(f.error)); auto* dialog = f.Dialog(); FindAndChoose(*dialog);
    Button(*dialog, "triggerReplacePreview")->click(); QVERIFY(dialog->PreviewPending()); QPointer<TriggerReplaceDialog> owner(dialog);
    f.module.reset(); QVERIFY(owner.isNull());
    // 串行独立 Qt test 中有界等待 owned worker 收束；不向 worker 借用对象、强杀线程或排 GUI 事件。
    QVERIFY2(QThreadPool::globalInstance()->waitForDone(5000), "owned CPU task did not finish within test cleanup limit");
    QApplication::processEvents(); QVERIFY(f.Unchanged());
}
void TriggerReplaceAsyncTests::CompletionErrorsKeepTimingUnknown() {
    Fixture f; QVERIFY2(f.Initialize(false), qPrintable(f.error)); Controlled h(f);
    Button(*h.dialog, "triggerReplacePreview")->click();
    h.result = Result<TriggerReplacePreviewResult>{std::nullopt, Status::Failure("Owned.Test", "OWNED_FAILURE", "owned test failure").diagnostics};
    h.Finish(); QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000);
    QVERIFY(!h.dialog->LastPreviewTiming()); QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged());
    Button(*h.dialog, "triggerReplacePreview")->click();
    h.duringRevision = [] { throw std::runtime_error("owned revision failure"); }; h.Finish();
    QTRY_VERIFY_WITH_TIMEOUT(!h.dialog->PreviewPending(), 5000);
    QVERIFY(!h.dialog->LastPreviewTiming()); QVERIFY(!Button(*h.dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.Unchanged());
}

int main(int argc, char** argv) {
    QTemporaryDir settings; if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraOwnedAsyncTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TriggerReplaceAsync"));
    QApplication app(argc, argv); if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    InitializeStyle(); app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    TriggerReplaceAsyncTests tests; return QTest::qExec(&tests, argc, argv);
}

#include "TriggerReplaceAsyncTests.moc"
