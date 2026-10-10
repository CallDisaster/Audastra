#include "TriggerReplaceDialog.h"
#include "TriggerModule.h"
#include "EditorProject.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
using War3Map::Modules::TriggerReplaceDialog;

namespace {
struct Wire {
    std::vector<std::byte> bytes;
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 255U));
    }
    void Text(std::string_view value) { for (char c : value) bytes.push_back(static_cast<std::byte>(c)); bytes.push_back(std::byte{}); }
};
std::vector<std::byte> Wtg() {
    Wire w;
    for (char c : std::string_view("WTG!")) w.bytes.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(1); w.I32(1); w.Text("Owned replace fixture"); w.I32(0); w.I32(2); w.I32(2);
    for (auto name : {"Count", "NextCount"}) { w.Text(name); w.Text("integer"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.Text(""); }
    w.I32(1); w.Text("Owned GUI"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1); w.I32(64);
    for (int index = 0; index < 64; ++index) {
        w.I32(2); w.Text("UseInt"); w.I32(1); w.I32(1); w.Text("Count"); w.I32(0); w.I32(0); w.I32(0);
    }
    return std::move(w.bytes);
}
std::vector<std::byte> Wct() { Wire w; w.I32(1); w.Text(""); w.I32(0); w.I32(1); w.I32(0); return std::move(w.bytes); }
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<class T> T Take(Result<T>&& value) {
    if (!value.Succeeded() || !value.value) {
        std::string text; for (const auto& d : value.diagnostics) text += d.code + ": " + d.message + "\n";
        throw std::runtime_error(text);
    }
    return std::move(*value.value);
}
void Write(const std::filesystem::path& path, std::span<const std::byte> value) {
    Require(QDir().mkpath(QString::fromStdWString(path.parent_path().wstring())), "owned fixture parent");
    QFile file(QString::fromStdWString(path.wstring())); Require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "owned new file");
    Require(file.write(reinterpret_cast<const char*>(value.data()), static_cast<qint64>(value.size())) == static_cast<qint64>(value.size()), "owned fixture write");
}
void Write(const std::filesystem::path& path, std::string_view value) {
    Write(path, std::as_bytes(std::span(value.data(), value.size())));
}
struct Fixture {
    QTemporaryDir temp;
    QtProject::EditorProject project;
    std::unique_ptr<Modules::TriggerModule> module;
    std::filesystem::path root;
    std::string wtgSha, wctSha;
    Trigger::EncodedTriggerDocument original;
    QString error;
    bool Initialize() {
        try {
            Require(temp.isValid(), "owned temp directory"); root = std::filesystem::path(temp.path().toStdWString());
            const auto ui = root / "catalog";
            Write(ui / "ui/config", "base\n");
            Write(ui / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
            Write(ui / "ui/base/action.txt", "[UseInt]\ndescription=Use ${value}\n[[.args]]\ntype=integer\n");
            auto catalog = Take(Trigger::UiCatalog::Load(ui));
            auto document = Take(Trigger::WtgWctCodec{}.Parse(Wtg(), Wct(), catalog));
            original = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog));
            Write(root / "source/map/war3map.wtg", original.wtg); Write(root / "source/map/war3map.wct", original.wct);
            wtgSha = Take(Sha256File(root / "source/map/war3map.wtg")); wctSha = Take(Sha256File(root / "source/map/war3map.wct"));
            project.projectRoot_ = root / "source";
            project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(project.projectRoot_)));
            project.uiDataRoot_ = project.activeUiDataRoot_ = ui;
            project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(catalog));
            project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(document));
            project.triggerUndoStack_ = new QUndoStack(&project);
            QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project, [&] { project.ObserveTriggerEdit(); });
            project.ResetEditTracking(); Require(project.editTrackingReady_ && !project.editTrackingFailed_, "real edit revision initialized");
            module = std::make_unique<Modules::TriggerModule>(project); module->Widget();
            Require(module->Reveal({"triggers", "trigger", QString::number(project.triggerDocument_->triggers.front().id), {}}), "real trigger selection");
            module->ShowFindReplace(); return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }
    TriggerReplaceDialog* Dialog() const { return dynamic_cast<TriggerReplaceDialog*>(module->Widget()->findChild<QDialog*>(QStringLiteral("triggerReplaceDialog"))); }
    bool SourcesUnchanged() const {
        return Take(Sha256File(root / "source/map/war3map.wtg")) == wtgSha && Take(Sha256File(root / "source/map/war3map.wct")) == wctSha;
    }
    bool ArgumentsAre(std::string_view name) const {
        return project.triggerDocument_->argumentNodes.size() == 64 && std::all_of(project.triggerDocument_->argumentNodes.begin(),
            project.triggerDocument_->argumentNodes.end(), [&](const auto& argument) { return argument.type == 1 && argument.value.text == name; });
    }
};
QPushButton* Button(QWidget& owner, const char* name) { return owner.findChild<QPushButton*>(QString::fromLatin1(name)); }
QLineEdit* Edit(QWidget& owner, const char* name) { return owner.findChild<QLineEdit*>(QString::fromLatin1(name)); }
QTreeWidget* Tree(QWidget& owner, const char* name) { return owner.findChild<QTreeWidget*>(QString::fromLatin1(name)); }
bool Capture(QWidget& widget, const char* name) {
    const auto directory = qEnvironmentVariable("AUD_TRIGGER_REPLACE_CAPTURE_DIR");
    if (directory.isEmpty()) return true;
    if (!QFileInfo(directory).isAbsolute() || !QFileInfo(directory).isDir()) return false;
    const auto path = QDir(directory).filePath(QString::fromLatin1(name) + QStringLiteral(".png"));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    QApplication::processEvents();
    const auto pixels = widget.grab();
    const auto saved = pixels.save(&output, "PNG");
    if (saved) qInfo().noquote() << "TRIGGER_REPLACE_CAPTURE" << path << pixels.size() << pixels.devicePixelRatio();
    return saved;
}
void FindAndChooseTarget(TriggerReplaceDialog& dialog) {
    Edit(dialog, "triggerReplaceQuery")->setText(QStringLiteral("Count")); Button(dialog, "triggerReplaceSearch")->click();
    Edit(dialog, "triggerReplaceTargetFilter")->setText(QStringLiteral("NextCount")); Button(dialog, "triggerReplaceTargetSearch")->click();
    auto* targets = dialog.findChild<QComboBox*>(QStringLiteral("triggerReplaceTarget"));
    Require(targets && targets->count() == 2, "one unique typed variable option and placeholder"); targets->setCurrentIndex(1);
}
} // namespace

class TriggerReplaceDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void RealServiceMultiplePagesPublishOneUndoAndPreserveSources();
    void PreviewDoesNotCommitDraftAndApplyRejectsChangedRevision();
    void ReadOnlyHitsCannotBeSelectedAndEnterDoesNotApply();
    void ReentrantQueryAndRevisionCannotPublishStaleRows();
    void QueryMayDeleteDialogOwner();
};

void TriggerReplaceDialogTests::RealServiceMultiplePagesPublishOneUndoAndPreserveSources() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); auto* dialog = f.Dialog(); QVERIFY(dialog);
    FindAndChooseTarget(*dialog);
    auto* tree = Tree(*dialog, "triggerReplaceResults"); QCOMPARE(tree->topLevelItemCount(), 50);
    QVERIFY(Capture(*dialog, "u4-query"));
    Button(*dialog, "triggerReplaceCheckPage")->click(); Button(*dialog, "triggerReplaceNext")->click();
    QCOMPARE(tree->topLevelItemCount(), 14); Button(*dialog, "triggerReplaceCheckPage")->click();
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.ArgumentsAre("Count"));
    Button(*dialog, "triggerReplacePreview")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->PreviewPending(), 5000);
    QCOMPARE(Tree(*dialog, "triggerReplacePreviewRows")->topLevelItemCount(), 64);
    QVERIFY2(Button(*dialog, "triggerReplaceApply")->isEnabled(), qPrintable(dialog->findChild<QLabel*>("triggerReplaceStatus")->text()));
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.ArgumentsAre("Count"));
    QVERIFY(Capture(*dialog, "u4-preview"));
    Button(*dialog, "triggerReplaceApply")->click(); QCOMPARE(f.project.triggerUndoStack_->count(), 1); QVERIFY(f.ArgumentsAre("NextCount"));
    QVERIFY(f.SourcesUnchanged()); f.project.triggerUndoStack_->undo(); QVERIFY(f.ArgumentsAre("Count"));
    const auto reverted = Take(Trigger::WtgWctCodec{}.Serialize(*f.project.triggerDocument_, *f.project.uiCatalog_));
    QVERIFY(reverted.wtg == f.original.wtg && reverted.wct == f.original.wct);
    f.project.triggerUndoStack_->redo(); QVERIFY(f.ArgumentsAre("NextCount")); QVERIFY(f.SourcesUnchanged());
}
void TriggerReplaceDialogTests::PreviewDoesNotCommitDraftAndApplyRejectsChangedRevision() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); auto* dialog = f.Dialog(); QVERIFY(dialog);
    auto* comment = f.module->CommentEditor(); QVERIFY(comment); comment->setPlainText(QStringLiteral("unpublished property draft"));
    FindAndChooseTarget(*dialog); Button(*dialog, "triggerReplaceCheckPage")->click(); Button(*dialog, "triggerReplacePreview")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->PreviewPending(), 5000);
    QVERIFY2(Button(*dialog, "triggerReplaceApply")->isEnabled(), qPrintable(dialog->findChild<QLabel*>("triggerReplaceStatus")->text()));
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.project.triggerDocument_->triggers.front().description.text.empty());
    Button(*dialog, "triggerReplaceApply")->click();
    QVERIFY(f.ArgumentsAre("Count")); QCOMPARE(f.project.triggerDocument_->triggers.front().description.text, std::string("unpublished property draft"));
    QCOMPARE(f.project.triggerUndoStack_->count(), 1); // 唯一新增的是现有草稿提交，旧替换候选被真实修订门拒绝。
    QVERIFY(!Button(*dialog, "triggerReplaceApply")->isEnabled()); QVERIFY(f.SourcesUnchanged());
}
void TriggerReplaceDialogTests::ReadOnlyHitsCannotBeSelectedAndEnterDoesNotApply() {
    int queries = 0, applies = 0;
    TriggerReplaceDialog dialog([] { return std::string("test/1"); }, [] { return Status::Ok(); },
        [&](std::string_view revision, const TriggerReplaceQuery&, const TriggerReplaceLimits&, const TriggerReplaceCursor*) {
            ++queries; TriggerReplacePage page; page.proof.contextRevision = revision;
            page.coverage.state = TriggerReplaceState::Partial; page.coverage.matchedInScannedScope = 1;
            TriggerReplaceHit row; row.rowKey = "readonly"; row.triggerName = "Owned read-only callback fixture";
            row.eligibility = TriggerReplaceEligibility::ReadOnly; row.reasonCode = "TEST_CODE_READ_ONLY"; row.reason = "code is not replaceable";
            page.rows.push_back(row); return Result<TriggerReplacePage>::Success(std::move(page));
        }, {}, {}, [&](std::string_view, const TriggerReplacePreview&, const TriggerReplaceLimits&) { ++applies; return Status::Ok(); }, {});
    dialog.show(); auto* edit = Edit(dialog, "triggerReplaceQuery"); edit->setText(QStringLiteral("code")); edit->setFocus();
    QTest::keyClick(edit, Qt::Key_Return); QCOMPARE(queries, 1); QCOMPARE(applies, 0); QVERIFY(dialog.isVisible());
    auto* tree = Tree(dialog, "triggerReplaceResults"); QCOMPARE(tree->topLevelItemCount(), 1);
    QVERIFY(!(tree->topLevelItem(0)->flags() & Qt::ItemIsUserCheckable)); Button(dialog, "triggerReplaceCheckPage")->click();
    QVERIFY(!Button(dialog, "triggerReplacePreview")->isEnabled()); QVERIFY(!Button(dialog, "triggerReplaceApply")->isEnabled());
    const auto text = dialog.findChild<QLabel*>("triggerReplaceStatus")->text();
    QVERIFY(text.contains(QStringLiteral("已扫描范围")) && text.contains(QStringLiteral("总数尚未确认")));
}
void TriggerReplaceDialogTests::ReentrantQueryAndRevisionCannotPublishStaleRows() {
    TriggerReplaceDialog* current = nullptr; bool reenterQuery = false, reenterRevision = false; int calls = 0;
    TriggerReplaceDialog dialog([&] { if (reenterRevision) { reenterRevision = false; current->ContextChanged(); } return std::string("test/1"); },
        [] { return Status::Ok(); }, [&](std::string_view revision, const TriggerReplaceQuery&, const TriggerReplaceLimits&, const TriggerReplaceCursor*) {
            ++calls; if (reenterQuery) current->ContextChanged();
            TriggerReplacePage page; page.proof.contextRevision = revision; page.rows.push_back({}); return Result<TriggerReplacePage>::Success(std::move(page));
        }, {}, {}, {}, {});
    current = &dialog; Edit(dialog, "triggerReplaceQuery")->setText(QStringLiteral("test"));
    reenterQuery = true; Button(dialog, "triggerReplaceSearch")->click(); QCOMPARE(calls, 1); QCOMPARE(Tree(dialog, "triggerReplaceResults")->topLevelItemCount(), 0);
    reenterQuery = false; reenterRevision = true; Button(dialog, "triggerReplaceSearch")->click(); QCOMPARE(calls, 1); QCOMPARE(Tree(dialog, "triggerReplaceResults")->topLevelItemCount(), 0);
    Button(dialog, "triggerReplaceSearch")->click(); QCOMPARE(calls, 2); QCOMPARE(Tree(dialog, "triggerReplaceResults")->topLevelItemCount(), 1);
}
void TriggerReplaceDialogTests::QueryMayDeleteDialogOwner() {
    auto* parent = new QWidget; QPointer<QWidget> owner(parent); int calls = 0;
    auto* dialog = new TriggerReplaceDialog([] { return std::string("test/1"); }, [] { return Status::Ok(); },
        [&](std::string_view, const TriggerReplaceQuery&, const TriggerReplaceLimits&, const TriggerReplaceCursor*) {
            ++calls; delete parent; return Result<TriggerReplacePage>::Success({});
        }, {}, {}, {}, {}, parent);
    QPointer<TriggerReplaceDialog> guarded(dialog); Edit(*dialog, "triggerReplaceQuery")->setText(QStringLiteral("test"));
    Button(*dialog, "triggerReplaceSearch")->click(); QCOMPARE(calls, 1); QVERIFY(!owner && !guarded);
}

void InitializeTriggerReplaceTestResources() { Q_INIT_RESOURCE(war3map_uikit_style); }
int main(int argc, char** argv) {
    QTemporaryDir settings; if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QApplication app(argc, argv); QCoreApplication::setOrganizationName(QStringLiteral("AudastraOwnedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TriggerReplaceDialogTests"));
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion"))); InitializeTriggerReplaceTestResources();
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory())); UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    TriggerReplaceDialogTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "TriggerReplaceDialogTests.moc"
