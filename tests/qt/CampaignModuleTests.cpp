#include "CampaignModule.h"
#include "AssetPropertiesDialog.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/IO/FileBytes.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

using namespace War3Map;
namespace {
std::vector<std::byte> Bytes(std::string_view value) {
    std::vector<std::byte> bytes; for (unsigned char c : value) bytes.push_back(static_cast<std::byte>(c)); return bytes;
}
std::filesystem::path Path(const QTemporaryDir& root, const char* name) { return std::filesystem::path(root.filePath(name).toStdWString()); }
std::shared_ptr<Services::CampaignSession> MenuFixture(const QTemporaryDir& root, const char* name) {
    const auto chapter = Path(root, name);
    const std::vector<MapArchiveMember> members{{"war3map.w3i", Bytes("synthetic-container-only")}, {"test.bin", Bytes(name)}};
    if (!WriteVerifiedArchive(chapter, members).Succeeded()) return {};
    auto session = Services::CampaignSession::Create();
    Services::CampaignCommand map; map.operation = Services::CampaignOperation::AddMap; map.path = "Chapter1.w3x"; map.mapSource = chapter;
    if (!session->Apply(session->Revision(), map).Succeeded()) return {};
    Services::CampaignCommand button; button.operation = Services::CampaignOperation::AddButton;
    button.button.chapterTitle = "Chapter One"; button.button.mapTitle = "Original Title"; button.button.mapPath = map.path;
    if (!session->Apply(session->Revision(), button).Succeeded()) return {};
    return session;
}
std::shared_ptr<Services::CampaignSession> SharedFixture(const QTemporaryDir& root) {
    Campaign::CampaignInfo info; info.name = "Synthetic Campaign";
    auto native = ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Unit);
    if (!native.value || !native.value->AddCustomObject("H000", "hfoo").Succeeded()) return {};
    if (!native.value->AddModification({ObjectData::ObjectType::Unit, "H000"}, {"unam"}, ObjectData::NativeValueType::String, std::string("TRIGSTR_1")).Succeeded()) return {};
    const auto object = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit).Serialize(*native.value);
    const auto encoded = Campaign::SerializeInfo(info); if (!object.value || !encoded.value) return {};
    const std::vector<MapArchiveMember> members{{"war3campaign.w3f", *encoded.value}, {"war3campaign.w3u", *object.value},
        {"war3campaign.wts", Bytes("STRING 1\r\n{\r\nCampaign Hero\r\n}\r\n")}, {"Textures\\original.bin", Bytes("original")}};
    const auto path = Path(root, "shared.w3n");
    if (!WriteVerifiedArchive(path, members).Succeeded()) return {};
    auto opened = Services::CampaignSession::Open(path); return opened.value.value_or(nullptr);
}
}

class CampaignModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void PagesSettingsAndSharedHistory();
    void SharedObjectsUseCampaignStringsAndSameSaveService();
    void ChapterMustBeExplicitlyReturnedBeforeCampaignSave();
    void ChapterSaveAsFollowsNewSource();
    void ModalCommandsRejectForeignContext_data();
    void ModalCommandsRejectForeignContext();
    void ModalCommandsCommitCurrentContext_data();
    void ModalCommandsCommitCurrentContext();
    void ForeignRevisionKeepsLocalDraft();
    void ImportPropertiesUseCampaignFolder();
};

void CampaignModuleTests::PagesSettingsAndSharedHistory() {
    QtProject::EditorProject project; Modules::CampaignModule module(project);
    auto* page = module.Widget(); module.SetSession(Services::CampaignSession::Create());
    auto* tabs = page->findChild<QTabWidget*>("campaignTabs"); QVERIFY(tabs); QCOMPARE(tabs->count(), 5);
    QCOMPARE(tabs->tabText(0), QStringLiteral("章节")); QCOMPARE(tabs->tabText(4), QStringLiteral("设置"));
    auto* name = page->findChild<QLineEdit*>("campaignSettingText0"); QVERIFY(name);
    name->setText("Edited Campaign"); QVERIFY(QMetaObject::invokeMethod(name, "textEdited", Q_ARG(QString, name->text())));
    QVERIFY(module.HasDraft()); QVERIFY(module.FlushDraft()); QVERIFY(module.Dirty());
    QCOMPARE(module.Session()->Info().name, std::string("Edited Campaign")); QVERIFY(module.UndoStack()->canUndo());
    module.UndoStack()->undo(); QCOMPARE(module.Session()->Info().name, std::string{});
    module.UndoStack()->redo(); QCOMPARE(module.Session()->Info().name, std::string("Edited Campaign"));
}

void CampaignModuleTests::SharedObjectsUseCampaignStringsAndSameSaveService() {
    QTemporaryDir root; QVERIFY(root.isValid()); auto session = SharedFixture(root); QVERIFY(session);
    QtProject::EditorProject mainProject;
    const std::array<MapArchiveMember, 1> mapStrings{{{"war3map.wts", Bytes("STRING 1\n{\nMain Map Hero\n}\n")}}};
    mainProject.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(mapStrings));
    Modules::CampaignModule module(mainProject); (void)module.Widget(); module.SetSession(session);
    auto& shared = module.SharedProject(); QVERIFY(shared.nativeObjectDocuments_[0]); QVERIFY(shared.objectDocuments_[0]);
    const ObjectData::ObjectKey key{ObjectData::ObjectType::Unit, "H000"};
    QCOMPARE(ResolveProjectObjectPresentation(shared, key).name, QStringLiteral("Campaign Hero"));
    auto* document = shared.objectDocuments_[0].get(); QVERIFY(!document->Objects().empty());
    const auto field = document->Objects().front().modifications.front().key;
    QVERIFY(document->SetModification(key, field, std::string("Changed Shared Hero")).Succeeded());
    shared.objectDirty_[0] = true;
    const auto file = root.filePath("import.bin"); QFile imported(file); QVERIFY(imported.open(QIODevice::WriteOnly));
    QCOMPARE(imported.write("owned resource"), 14); imported.close();
    QCOMPARE(shared.AddImportFiles({file}), 1); QVERIFY(shared.pendingImports_.front().archivePath.startsWith("war3campaignImported\\"));
    QVERIFY(module.FlushDraft()); QVERIFY(!module.HasDraft()); QVERIFY(mainProject.objectDocuments_[0] == nullptr);
    const auto saved = Path(root, "edited.w3n"); QVERIFY(module.SaveTo(saved)); QVERIFY(!module.Dirty());
    auto archive = MapArchiveSession::Open(saved); QVERIFY(archive.value);
    QVERIFY(archive.value->HasMember("war3campaign.w3u")); QVERIFY(!archive.value->HasMember("war3map.w3u"));
    QVERIFY(archive.value->HasMember("war3campaignImported\\import.bin")); QVERIFY(archive.value->HasMember("Textures\\original.bin"));
    QCOMPARE(ResolveProjectObjectPresentation(module.SharedProject(), key).name, QStringLiteral("Changed Shared Hero"));
    module.UndoStack()->undo(); QVERIFY(module.Dirty());
    QCOMPARE(ResolveProjectObjectPresentation(module.SharedProject(), key).name, QStringLiteral("Campaign Hero"));
    module.UndoStack()->redo(); QVERIFY(!module.Dirty());
}

void CampaignModuleTests::ChapterMustBeExplicitlyReturnedBeforeCampaignSave() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto chapter = Path(root, "chapter.w3x");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", Bytes("synthetic-container-only")}, {"test.bin", Bytes("before")}};
    QVERIFY(WriteVerifiedArchive(chapter, members).Succeeded());
    QtProject::EditorProject project; Modules::CampaignModule module(project); (void)module.Widget();
    auto session = Services::CampaignSession::Create(); module.SetSession(session);
    Services::CampaignCommand add; add.operation = Services::CampaignOperation::AddMap; add.path = "Chapter1.w3x"; add.mapSource = chapter;
    QVERIFY(module.ApplyCommand(add)); const auto id = session->Info().maps.front().id;
    connect(&module, &Modules::CampaignModule::ChapterOpenRequested, &project, [&](const QString& path) { project.sourceArchivePath_ = std::filesystem::path(path.toStdWString()); });
    QVERIFY(module.OpenChapter(id)); const auto opened = project.sourceArchivePath_;
    QVERIFY(opened != chapter); auto map = MapArchiveSession::Open(opened); QVERIFY(map.value);
    const auto candidate = Path(root, "replacement.w3x");
    const std::array<MemberReplacement, 1> replacements{{{"test.bin", Bytes("after")}}};
    QVERIFY(map.value->SaveCandidate(candidate, replacements).Succeeded());
    QVERIFY(QFile::remove(QString::fromStdWString(opened.wstring())));
    QVERIFY(QFile::copy(QString::fromStdWString(candidate.wstring()), QString::fromStdWString(opened.wstring())));
    module.NoteChapterSaved(opened, opened); QVERIFY(module.HasUncommittedChapter());
    const auto output = Path(root, "with-chapter.w3n");
    QVERIFY(!module.SaveTo(output)); QVERIFY(!std::filesystem::exists(output));
    QVERIFY(module.UpdateOpenedChapter()); QVERIFY(!module.HasUncommittedChapter()); QVERIFY(module.SaveTo(output));
    const auto bytes = session->ReadMember(session->Revision(), "Chapter1.w3x");
    const auto expected = IO::ReadFileBytes(candidate, 1024u * 1024u); QVERIFY(bytes.value); QVERIFY(expected.value); QCOMPARE(*bytes.value, *expected.value);
}

void CampaignModuleTests::ChapterSaveAsFollowsNewSource() {
    QTemporaryDir root; QVERIFY(root.isValid()); auto session = MenuFixture(root, "chapter.w3x"); QVERIFY(session);
    QtProject::EditorProject project; Modules::CampaignModule module(project); (void)module.Widget(); module.SetSession(session);
    connect(&module, &Modules::CampaignModule::ChapterOpenRequested, &project, [&](const QString& path) { project.sourceArchivePath_ = std::filesystem::path(path.toStdWString()); });
    const auto chapterId = session->Info().maps.front().id; QVERIFY(module.OpenChapter(chapterId));
    const auto original = project.sourceArchivePath_;
    const auto originalBytes = IO::ReadFileBytes(original, 1024u * 1024u); QVERIFY(originalBytes.value);
    auto map = MapArchiveSession::Open(original); QVERIFY(map.value);
    const auto savedAs = Path(root, "saved-as.w3x");
    const std::array<MemberReplacement, 1> replacements{{{"test.bin", Bytes("saved to a new path")}}};
    QVERIFY(map.value->SaveCandidate(savedAs, replacements).Succeeded());
    project.sourceArchivePath_ = savedAs;
    module.NoteChapterSaved(Path(root, "unrelated.w3x"), savedAs); QVERIFY(!module.HasUncommittedChapter());
    module.NoteChapterSaved(original, savedAs); QVERIFY(module.HasUncommittedChapter());
    const auto output = Path(root, "saved-as-chapter.w3n");
    QVERIFY(!module.SaveTo(output)); QVERIFY(!std::filesystem::exists(output));
    QVERIFY(module.UpdateOpenedChapter()); QVERIFY(!module.HasUncommittedChapter()); QVERIFY(module.SaveTo(output));
    const auto expected = IO::ReadFileBytes(savedAs, 1024u * 1024u); QVERIFY(expected.value);
    const auto member = session->ReadMember(session->Revision(), "Chapter1.w3x"); QVERIFY(member.value); QCOMPARE(*member.value, *expected.value);
    const auto unchanged = IO::ReadFileBytes(original, 1024u * 1024u); QVERIFY(unchanged.value); QCOMPARE(*unchanged.value, *originalBytes.value);

    // 另一个入口已替换章节时，另存来源迁移也不能绕过原成员指纹检查。
    const auto external = Path(root, "external.w3x");
    const std::array<MemberReplacement, 1> foreign{{{"test.bin", Bytes("external replacement")}}};
    QVERIFY(map.value->SaveCandidate(external, foreign).Succeeded());
    Services::CampaignCommand replace; replace.operation = Services::CampaignOperation::ReplaceMap;
    replace.id = chapterId; replace.mapSource = external; QVERIFY(session->Apply(session->Revision(), replace).Succeeded());
    module.RefreshFromService(); module.NoteChapterSaved(savedAs, savedAs);
    QVERIFY(module.HasUncommittedChapter()); QVERIFY(!module.UpdateOpenedChapter()); QVERIFY(module.HasUncommittedChapter());
    const auto kept = session->ReadMember(session->Revision(), "Chapter1.w3x");
    const auto externalBytes = IO::ReadFileBytes(external, 1024u * 1024u); QVERIFY(kept.value); QVERIFY(externalBytes.value); QCOMPARE(*kept.value, *externalBytes.value);
}

void CampaignModuleTests::ModalCommandsRejectForeignContext_data() {
    QTest::addColumn<int>("operation"); QTest::addColumn<bool>("switchSession");
    for (int operation = 0; operation < 4; ++operation) {
        const std::array<const char*, 4> names{"edit-button", "add-button", "add-map", "replace-map"};
        QTest::newRow((QByteArray(names[operation]) + "-revision").constData()) << operation << false;
        QTest::newRow((QByteArray(names[operation]) + "-session").constData()) << operation << true;
    }
}
void CampaignModuleTests::ModalCommandsRejectForeignContext() {
    QFETCH(int, operation); QFETCH(bool, switchSession);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto original = MenuFixture(root, "original.w3x"); auto replacement = MenuFixture(root, "replacement.w3x");
    QVERIFY(original); QVERIFY(replacement); QCOMPARE(original->Info().buttons.front().id, replacement->Info().buttons.front().id);
    QtProject::EditorProject project; Modules::CampaignModule module(project); auto* page = module.Widget(); module.SetSession(original);
    const bool menu = operation < 2;
    auto* table = page->findChild<QTableWidget*>(menu ? "campaignButtons" : "campaignMaps"); QVERIFY(table); table->selectRow(0);
    const std::array<const char*, 4> controls{"editCampaignButton", "addCampaignButton", "addCampaignMap", "replaceCampaignMap"};
    auto* open = page->findChild<QPushButton*>(controls[operation]); QVERIFY(open);
    bool visited = false, guarded = false, changed = false, stayedOpen = false, inputKept = false, errorShown = false;
    const QString draft = menu ? QStringLiteral("Unsaved menu input") : root.filePath("replacement.w3x");
    QTimer watchdog;
    watchdog.setSingleShot(true);
    connect(&watchdog, &QTimer::timeout, &module, [&] {
        if (auto* dialog = page->findChild<QDialog*>(menu ? "campaignButtonDialog" : "campaignMapDialog")) dialog->reject();
    });
    watchdog.start(1000);
    QTimer::singleShot(0, &module, [&] {
        auto* dialog = page->findChild<QDialog*>(menu ? "campaignButtonDialog" : "campaignMapDialog");
        if (!dialog) return;
        auto* input = dialog->findChild<QLineEdit*>(menu ? "campaignButtonMapTitle" : "campaignMapSource");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* error = dialog->findChild<QLabel*>(menu ? "campaignButtonEditError" : "campaignMapEditError");
        if (!input || !buttons || !error) { dialog->reject(); return; }
        visited = true; guarded = module.HasModalOperation(); input->setText(draft);
        if (switchSession) {
            module.SetSession(replacement); changed = true;
        } else {
            Services::CampaignCommand command; command.operation = Services::CampaignOperation::UpdateButton;
            command.id = original->Info().buttons.front().id; command.button = original->Info().buttons.front();
            command.button.mapTitle = "Updated elsewhere";
            changed = original->Apply(original->Revision(), command).Succeeded(); module.RefreshFromService();
        }
        buttons->button(QDialogButtonBox::Ok)->click();
        stayedOpen = dialog->isVisible(); inputKept = input->text() == draft; errorShown = !error->text().isEmpty();
        dialog->reject();
    });
    open->click(); watchdog.stop();
    QVERIFY(visited); QVERIFY(guarded); QVERIFY(changed); QVERIFY(stayedOpen); QVERIFY(inputKept); QVERIFY(errorShown);
    QVERIFY(!module.HasModalOperation()); QCOMPARE(original->Info().maps.size(), std::size_t{1}); QCOMPARE(original->Info().buttons.size(), std::size_t{1});
    QCOMPARE(original->Info().buttons.front().mapTitle, switchSession ? std::string("Original Title") : std::string("Updated elsewhere"));
    QCOMPARE(replacement->Info().buttons.front().mapTitle, std::string("Original Title"));
    QCOMPARE(replacement->Info().maps.size(), std::size_t{1}); QCOMPARE(replacement->Info().buttons.size(), std::size_t{1});
    const auto kept = original->ReadMember(original->Revision(), "Chapter1.w3x");
    const auto expected = IO::ReadFileBytes(Path(root, "original.w3x"), 1024u * 1024u);
    QVERIFY(kept.value); QVERIFY(expected.value); QCOMPARE(*kept.value, *expected.value);
}

void CampaignModuleTests::ModalCommandsCommitCurrentContext_data() {
    QTest::addColumn<int>("operation");
    QTest::newRow("edit-button") << 0; QTest::newRow("add-button") << 1;
    QTest::newRow("add-map") << 2; QTest::newRow("replace-map") << 3;
}
void CampaignModuleTests::ModalCommandsCommitCurrentContext() {
    QFETCH(int, operation);
    QTemporaryDir root; QVERIFY(root.isValid()); auto session = MenuFixture(root, "chapter.w3x"); QVERIFY(session);
    const auto source = Path(root, "updated.w3x");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", Bytes("synthetic-container-only")}, {"test.bin", Bytes("updated")}};
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    QtProject::EditorProject project; Modules::CampaignModule module(project); auto* page = module.Widget(); module.SetSession(session);
    const bool menu = operation < 2;
    auto* table = page->findChild<QTableWidget*>(menu ? "campaignButtons" : "campaignMaps"); QVERIFY(table); table->selectRow(0);
    // 应用设置会重建表格；弹框必须保留应用前选择的稳定身份。
    auto* setting = page->findChild<QLineEdit*>("campaignSettingText0"); QVERIFY(setting); setting->setText("Settings draft");
    QVERIFY(QMetaObject::invokeMethod(setting, "textEdited", Q_ARG(QString, setting->text())));
    const std::array<const char*, 4> names{"editCampaignButton", "addCampaignButton", "addCampaignMap", "replaceCampaignMap"};
    auto* open = page->findChild<QPushButton*>(names[operation]); QVERIFY(open);
    bool visited = false, committed = false;
    QTimer watchdog; watchdog.setSingleShot(true);
    connect(&watchdog, &QTimer::timeout, &module, [&] {
        if (auto* dialog = page->findChild<QDialog*>(menu ? "campaignButtonDialog" : "campaignMapDialog")) dialog->reject();
    });
    watchdog.start(1000);
    QTimer::singleShot(0, &module, [&] {
        auto* dialog = page->findChild<QDialog*>(menu ? "campaignButtonDialog" : "campaignMapDialog");
        if (!dialog) return;
        auto* input = dialog->findChild<QLineEdit*>(menu ? "campaignButtonMapTitle" : "campaignMapSource");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!input || !buttons) { dialog->reject(); return; }
        visited = true; input->setText(menu ? QStringLiteral("New title") : QString::fromStdWString(source.wstring()));
        buttons->button(QDialogButtonBox::Ok)->click();
        committed = dialog->result() == QDialog::Accepted;
        if (!committed) dialog->reject();
    });
    open->click(); watchdog.stop();
    QVERIFY(visited); QVERIFY2(committed, qPrintable(module.LastError())); QVERIFY(!module.HasModalOperation()); QVERIFY(!module.HasDraft());
    QCOMPARE(session->Info().name, std::string("Settings draft"));
    QCOMPARE(session->Info().maps.size(), operation == 2 ? std::size_t{2} : std::size_t{1});
    QCOMPARE(session->Info().buttons.size(), operation == 1 ? std::size_t{2} : std::size_t{1});
    if (menu) {
        QCOMPARE(operation == 0 ? session->Info().buttons.front().mapTitle : session->Info().buttons.back().mapTitle, std::string("New title"));
    } else {
        const auto updated = session->ReadMember(session->Revision(), operation == 2 ? "updated.w3x" : "Chapter1.w3x");
        const auto expected = IO::ReadFileBytes(source, 1024u * 1024u); QVERIFY(updated.value); QVERIFY(expected.value); QCOMPARE(*updated.value, *expected.value);
    }
}

void CampaignModuleTests::ForeignRevisionKeepsLocalDraft() {
    QtProject::EditorProject project; Modules::CampaignModule module(project); auto* page = module.Widget();
    auto session = Services::CampaignSession::Create(); module.SetSession(session);
    auto* name = page->findChild<QLineEdit*>("campaignSettingText0"); name->setText("Kept draft");
    QVERIFY(QMetaObject::invokeMethod(name, "textEdited", Q_ARG(QString, name->text())));
    Services::CampaignCommand edit; edit.operation = Services::CampaignOperation::SetInfo; edit.info = session->Info(); edit.info.author = "Other editor";
    QVERIFY(session->Apply(session->Revision(), edit).Succeeded());
    QVERIFY(!module.FlushDraft()); QVERIFY(module.HasDraft()); QCOMPARE(name->text(), QStringLiteral("Kept draft"));
    QCOMPARE(session->Info().name, std::string{});
}
void CampaignModuleTests::ImportPropertiesUseCampaignFolder() {
    QtProject::AssetEntry entry; entry.path = "Textures\\sample.bin"; entry.originalPath = entry.path; entry.size = 4;
    Modules::AssetPropertiesDialog dialog(entry, [](const QString& value) { return QtProject::EditorProject::CheckAssetPath(value); }, {}, nullptr, false, true);
    auto* custom = dialog.findChild<QCheckBox*>("assetCustomPath"); QVERIFY(custom); QVERIFY(custom->isChecked());
    custom->setChecked(false); QCOMPARE(dialog.Path(), QStringLiteral("war3campaignImported\\sample.bin"));
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv);
    CampaignModuleTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "CampaignModuleTests.moc"
