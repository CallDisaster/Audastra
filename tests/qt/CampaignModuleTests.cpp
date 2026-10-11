#include "CampaignModule.h"
#include "AssetPropertiesDialog.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/IO/FileBytes.h"
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QCheckBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace War3Map;
namespace {
std::vector<std::byte> Bytes(std::string_view value) {
    std::vector<std::byte> bytes; for (unsigned char c : value) bytes.push_back(static_cast<std::byte>(c)); return bytes;
}
std::filesystem::path Path(const QTemporaryDir& root, const char* name) { return std::filesystem::path(root.filePath(name).toStdWString()); }
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
    module.NoteChapterSaved(opened); QVERIFY(module.HasUncommittedChapter());
    const auto output = Path(root, "with-chapter.w3n");
    QVERIFY(!module.SaveTo(output)); QVERIFY(!std::filesystem::exists(output));
    QVERIFY(module.UpdateOpenedChapter()); QVERIFY(!module.HasUncommittedChapter()); QVERIFY(module.SaveTo(output));
    const auto bytes = session->ReadMember(session->Revision(), "Chapter1.w3x");
    const auto expected = IO::ReadFileBytes(candidate, 1024u * 1024u); QVERIFY(bytes.value); QVERIFY(expected.value); QCOMPARE(*bytes.value, *expected.value);
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
