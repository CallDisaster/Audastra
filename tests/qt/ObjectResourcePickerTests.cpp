#include "Widgets/ObjectResourcePicker.h"
#include "ObjectModels.h"
#include "ObjectResourcePickerAdapter.h"
#include "EditorProject.h"
#include "ObjectModule.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "SoundPreviewPlayer.h"
#include "WorldAnimationPlayer.h"
#include "WorldModelPreview.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ProfileList.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QSaveFile>
#include <QScreen>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>
#include <QtTest>
#include <Windows.h>
#include "../AnimationTestModel.h"
#include <cmath>
#include <map>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
ObjectResourcePickerServices ServicesFor(bool* current, QStringList* previews = nullptr) {
    ObjectResourcePickerServices result; result.revision = "picker:1";
    result.isCurrent = [current] { return *current; };
    result.load = [](Services::ObjectResourceOrigin origin, const auto&) {
        auto catalog = std::make_shared<Services::ObjectResourceCatalog>();
        catalog->revision = "picker:1"; catalog->identity = std::to_string(static_cast<int>(origin)); catalog->origin = origin;
        Services::ObjectResourceCandidate candidate;
        candidate.id = "fixture-model"; candidate.title = "合成模型"; candidate.value = "Basic.mdx";
        candidate.sourceId = "synthetic"; candidate.kind = Services::ObjectResourceKind::Model; candidate.origin = origin;
        catalog->rows.push_back(candidate);
        return Result<std::shared_ptr<const Services::ObjectResourceCatalog>>::Success(std::move(catalog));
    };
    if (previews) result.preview = [previews](QWidget* parent) {
        return ObjectResourcePreviewHandle{new QLabel(QStringLiteral("合成预览记录器"), parent),
            [previews](const QString& value, auto) { previews->push_back(value); }};
    };
    return result;
}
QPushButton* Apply(QWidget& panel) { return panel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply); }
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
bool InstallModelDefinitions(QtProject::EditorProject& project, bool evidence = false) {
    using namespace ObjectData;
    std::map<std::string, std::string> files{
        {"units.slk", "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"file\"\nC;X1;Y2;K\"u001\"\nC;X2;K\"Basic.mdx\"\nE\n"},
        {"fields.slk", "ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"slk\"\nC;X4;K\"type\"\n"
            "C;X1;Y2;K\"umdl\"\nC;X2;K\"file\"\nC;X3;K\"UnitUI\"\nC;X4;K\"model\"\nE\n"}};
    if (evidence) {
        files["units.slk"] = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"file\"\nC;X3;K\"Art\"\nC;X4;K\"Missiles\"\nC;X5;K\"Sound\"\n"
            "C;X1;Y2;K\"u001\"\nC;X2;K\"EvidenceModel.mdx\"\nC;X3;K\"EvidenceIcon.blp\"\n"
            "C;X4;K\"EvidenceModel.mdx,EvidenceModelB.mdx\"\nC;X5;K\"EvidenceSound.wav\"\nE\n";
        files["fields.slk"] = "ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"slk\"\nC;X4;K\"type\"\n"
            "C;X1;Y2;K\"umdl\"\nC;X2;K\"file\"\nC;X3;K\"UnitUI\"\nC;X4;K\"model\"\n"
            "C;X1;Y3;K\"uico\"\nC;X2;K\"Art\"\nC;X3;K\"UnitUI\"\nC;X4;K\"icon\"\n"
            "C;X1;Y4;K\"umis\"\nC;X2;K\"Missiles\"\nC;X3;K\"UnitUI\"\nC;X4;K\"modelList\"\n"
            "C;X1;Y5;K\"usnd\"\nC;X2;K\"Sound\"\nC;X3;K\"UnitUI\"\nC;X4;K\"soundFile\"\nE\n";
    }
    const auto reader = [&](const std::string& path) {
        const auto found = files.find(path);
        return found != files.end() ? Result<std::vector<std::byte>>::Success(Bytes(found->second))
            : Result<std::vector<std::byte>>::Failure("fixture", "missing", path);
    };
    ObjectFieldSchemaOptions options; options.sources = {{ObjectType::Unit, "fields.slk"}};
    options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
    auto schema = ObjectFieldSchema::Load(reader, options);
    CatalogLoadOptions catalogOptions; catalogOptions.sources = {{ObjectType::Unit, {"units.slk"}, {}}}; catalogOptions.stringFiles.clear();
    auto catalog = StandardObjectCatalog::Load(reader, catalogOptions);
    if (!schema.Succeeded() || !catalog.Succeeded()) return false;
    project.standardCatalog_ = std::make_shared<const StandardObjectCatalog>(std::move(*catalog.value));
    project.objectFieldSchema_ = std::make_shared<const ObjectFieldSchema>(std::move(*schema.value));
    return true;
}
}
class ObjectResourcePickerTests final : public QObject {
    Q_OBJECT
private slots:
    void SourcesHoverAndKeyboard();
    void OrderedValuesAndUnchangedOriginal();
    void StaleDraftDoesNotCommit();
    void FieldDelegateUsesSameUndoAndSerialization();
    void StandardFieldUsesSameCommand();
    void CommitMayCloseOwner();
    void AdapterOwnsSnapshotAndRejectsOversizedSource();
    void OwnedProductionResourceEvidence();
};
void ObjectResourcePickerTests::SourcesHoverAndKeyboard() {
    bool current = true; QStringList previews;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Missing.mdx", ServicesFor(&current, &previews));
    QPointer<ObjectResourcePicker> guard(picker); QString submitted;
    picker->SetCommit([&](const QString& value) { submitted = value; return true; }); picker->show();
    auto* list = picker->findChild<QTreeWidget*>("resourceCandidates");
    auto* tabs = picker->findChild<QTabWidget*>("resourceSourceTabs");
    QTRY_COMPARE(list->topLevelItemCount(), 1);
    auto* item = list->topLevelItem(0);
    QTest::mouseMove(list->viewport(), list->visualItemRect(item).center());
    QTRY_VERIFY(previews.contains(QStringLiteral("Basic.mdx")));
    tabs->setCurrentIndex(1); QTRY_COMPARE(list->topLevelItemCount(), 1);
    list->setCurrentItem(list->topLevelItem(0)); QTest::keyClick(list, Qt::Key_Return);
    QCOMPARE(picker->findChild<QLineEdit*>("resourcePath")->text(), QStringLiteral("Basic.mdx"));
    tabs->setCurrentIndex(2); auto* path = picker->findChild<QLineEdit*>("resourcePath");
    path->setText(QStringLiteral("Custom.mdx")); QTest::keyClick(path, Qt::Key_Return);
    QCOMPARE(submitted, QStringLiteral("Custom.mdx")); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::OrderedValuesAndUnchangedOriginal() {
    bool current = true;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, true, true},
        "  First.mdx ,\"Second,Part.mdx\"", ServicesFor(&current));
    QPointer<ObjectResourcePicker> guard(picker); int commits = 0;
    picker->SetCommit([&](const QString&) { ++commits; return true; }); picker->show();
    QCOMPARE(picker->Value(), QStringLiteral("  First.mdx ,\"Second,Part.mdx\""));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QTRY_VERIFY(!guard); QCOMPARE(commits, 0);
    picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, true, true}, "First.mdx,Second.mdx", ServicesFor(&current));
    guard = picker;
    picker->show(); auto* list = picker->findChild<QListWidget*>("resourceOrderedValues");
    auto* path = picker->findChild<QLineEdit*>("resourcePath"); path->setText(QStringLiteral("Third.mdx"));
    QTest::mouseClick(picker->findChild<QPushButton*>("resourceAdd"), Qt::LeftButton);
    QTest::mouseClick(picker->findChild<QPushButton*>("resourceMoveUp"), Qt::LeftButton);
    QCOMPARE(picker->Value(), QStringLiteral("First.mdx,Third.mdx,Second.mdx"));
    list->setCurrentRow(0); QTest::mouseClick(picker->findChild<QPushButton*>("resourceRemove"), Qt::LeftButton);
    QCOMPARE(picker->Value(), QStringLiteral("Third.mdx,Second.mdx")); picker->close(); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::StaleDraftDoesNotCommit() {
    bool current = true;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Missing.mdx", ServicesFor(&current));
    const QPointer<ObjectResourcePicker> guard(picker);
    int commits = 0; picker->SetCommit([&](const QString&) { ++commits; return true; }); picker->show();
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("Changed.mdx")); current = false;
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QCOMPARE(commits, 0); QVERIFY(picker->isVisible());
    QVERIFY(picker->findChild<QLabel*>("resourceStatus")->text().contains(QStringLiteral("工程已变化"))); picker->close(); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::FieldDelegateUsesSameUndoAndSerialization() {
    using namespace ObjectData;
    QtProject::EditorProject project; QVERIFY(InstallModelDefinitions(project));
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes("[u001]\n_parent = \"u001\"\nfile = \"Missing.mdx\"\n"));
    QVERIFY(parsed.Succeeded()); auto* document = dynamic_cast<LniObjectDocument*>(parsed.document.get()); QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u001"}; QUndoStack undo; ObjectFieldModel model;
    model.SetUndoStack(&undo); model.SetContext(project.standardCatalog_.get(), project.objectFieldSchema_.get()); model.SetDocument(document, &object);
    int row = -1; for (int i = 0; i < model.rowCount(); ++i) if (model.DefinitionAt(i) && model.DefinitionAt(i)->fieldId == "umdl") row = i;
    QVERIFY(row >= 0); const auto index = model.index(row, 3); QVERIFY(index.flags() & Qt::ItemIsEditable);
    QTableView view; view.setModel(&model); auto* delegate = new ObjectFieldDelegate(&view); view.setItemDelegate(delegate);
    bool current = true; delegate->SetResourcePickerFactory([&] { return ServicesFor(&current); });
    view.resize(760, 480); view.show(); view.setCurrentIndex(index); QTest::keyClick(&view, Qt::Key_Return);
    QTRY_VERIFY(view.findChild<ObjectResourcePicker*>());
    auto* picker = view.findChild<ObjectResourcePicker*>(); QPointer<ObjectResourcePicker> guard(picker);
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("Changed.mdx")); QTest::mouseClick(Apply(*picker), Qt::LeftButton);
    QTRY_VERIFY(!guard); QCOMPARE(undo.count(), 1);
    QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Changed.mdx"));
    auto saved = LniObjectCodec(ObjectType::Unit).Serialize(*document); QVERIFY(saved.Succeeded());
    auto readback = LniObjectCodec(ObjectType::Unit).Parse(*saved.value); QVERIFY(readback.Succeeded());
    const auto* recovered = dynamic_cast<LniObjectDocument*>(readback.document.get()); QVERIFY(recovered);
    QCOMPARE(QString::fromStdString(ValueToDisplayString(recovered->Objects().front().modifications.front().value)), QStringLiteral("Changed.mdx"));
    undo.undo(); QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Missing.mdx"));
    undo.redo(); QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Changed.mdx"));
}
void ObjectResourcePickerTests::StandardFieldUsesSameCommand() {
    using namespace ObjectData;
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    QtProject::EditorProject project; QVERIFY(InstallModelDefinitions(project));
    project.projectRoot_ = workspace.path().toStdWString(); project.sourceArchivePath_ = project.projectRoot_ / "synthetic.w3x";
    auto empty = LniObjectCodec(ObjectType::Unit).Parse({}); QVERIFY(empty.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(empty.document.release()));
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit, 2); QVERIFY(native.Succeeded());
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
    for (auto& stack : project.objectUndoStacks_) stack = new QUndoStack(&project);
    bool current = true; ObjectModule module(project);
    module.SetResourcePickerFactory([&] { return ServicesFor(&current); });
    auto* page = module.Widget(); page->resize(1200, 780); page->show(); module.UpdateObjectReferenceEntries();
    Shell::NavigationTarget target; target.module = "objects"; target.kind = "object"; target.id = "unit:u001";
    target.extra.insert("source", "standard"); QVERIFY(module.Reveal(target));
    auto* view = page->findChild<QTableView*>("standardObjectFields"); QVERIFY(view);
    int row = -1;
    for (int i = 0; i < view->model()->rowCount(); ++i)
        if (view->model()->index(i, 0).data(Qt::UserRole).toString() == "file") row = i;
    QVERIFY(row >= 0); const auto index = view->model()->index(row, 1);
    view->setCurrentIndex(index); page->activateWindow(); view->setFocus();
    QTRY_VERIFY(view->hasFocus()); QTest::keyClick(view, Qt::Key_Return);
    QTRY_VERIFY(page->findChild<ObjectResourcePicker*>());
    auto* picker = page->findChild<ObjectResourcePicker*>(); QPointer<ObjectResourcePicker> guard(picker);
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("StandardOverride.mdx"));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QTRY_VERIFY(!guard);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);
    const auto* object = project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}); QVERIFY(object);
    QCOMPARE(ValueToDisplayString(object->modifications.front().value), std::string("StandardOverride.mdx"));
    QCOMPARE(project.standardCatalog_->Find(ObjectType::Unit, "u001")->Field("file"), std::string_view("Basic.mdx"));
    project.objectUndoStacks_[0]->undo(); QVERIFY(!project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}));
    project.objectUndoStacks_[0]->redo(); QVERIFY(project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}));
    auto saved = LniObjectCodec(ObjectType::Unit).Serialize(*project.objectDocuments_[0]); QVERIFY(saved.Succeeded());
    auto readback = LniObjectCodec(ObjectType::Unit).Parse(*saved.value); QVERIFY(readback.Succeeded());
    const auto* recovered = dynamic_cast<LniObjectDocument*>(readback.document.get()); QVERIFY(recovered);
    QCOMPARE(ValueToDisplayString(recovered->Objects().front().modifications.front().value), std::string("StandardOverride.mdx"));
}
void ObjectResourcePickerTests::CommitMayCloseOwner() {
    bool current = true; auto* owner = new QWidget;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Before.mdx", ServicesFor(&current), owner);
    const QPointer<ObjectResourcePicker> guard(picker);
    picker->SetCommit([owner](const QString&) { delete owner; return true; }); picker->show();
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("After.mdx"));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QVERIFY(!guard);
}
void ObjectResourcePickerTests::AdapterOwnsSnapshotAndRejectsOversizedSource() {
    using namespace ObjectData;
    auto project = std::make_unique<QtProject::EditorProject>(); QVERIFY(InstallModelDefinitions(*project));
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes("[u001]\nfile = \"Frozen.mdx\"\n")); QVERIFY(parsed.Succeeded());
    project->objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
    auto factory = QtApp::MakeObjectResourcePickerFactory(*project); auto services = factory();
    QVERIFY(services.isCurrent()); project.reset(); QVERIFY(!services.isCurrent());
    auto catalog = services.load(Services::ObjectResourceOrigin::Object, [](std::size_t) { return true; }); QVERIFY(catalog.Succeeded());
    QVERIFY(std::any_of((*catalog.value)->rows.begin(), (*catalog.value)->rows.end(), [](const auto& row) { return row.value == "Frozen.mdx"; }));
    project = std::make_unique<QtProject::EditorProject>(); QVERIFY(InstallModelDefinitions(*project));
    std::string excessive;
    for (int i = 0; i < 32769; ++i) excessive += '[' + QString::number(i, 16).rightJustified(4, '0').toStdString() + "]\n";
    parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes(excessive)); QVERIFY(parsed.Succeeded());
    project->objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
    factory = QtApp::MakeObjectResourcePickerFactory(*project); services = factory();
    catalog = services.load(Services::ObjectResourceOrigin::Object, [](std::size_t) { return true; }); QVERIFY(!catalog.Succeeded());
    QCOMPARE(catalog.diagnostics.front().code, std::string("OBJECT_RESOURCE_SNAPSHOT_LIMIT"));
}

// 仅显式补证时启用：生产Adapter、资源选择器、D3D11完成帧与声音播放器，
// 全部地图资源由本用例原创生成；不使用上面的QLabel预览替身。
void ObjectResourcePickerTests::OwnedProductionResourceEvidence() {
    using namespace ObjectData;
    const auto output = qEnvironmentVariable("AUD_RESOURCE_PREVIEW_EVIDENCE_DIR");
    if (output.isEmpty()) QSKIP("Explicit production resource evidence is not enabled");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    QVERIFY(QDir::isAbsolutePath(output)); QVERIFY(QDir().mkpath(output));
    const auto receiptPath = QDir(output).filePath(QStringLiteral("resource-preview-receipt.json"));
    QVERIFY2(!QFileInfo::exists(receiptPath), "Preserve earlier evidence");
    QElapsedTimer clock; clock.start();
    QString stage = QStringLiteral("private-desktop");
    bool completed = false;
    QJsonArray images, sources, motion;
    QJsonObject receipt{{"scope", "production-resource-picker-components"}, {"game_runtime_verified", false},
        {"main_window_visual_verified", false}, {"audio_human_listening_verified", false}, {"desktop_input_used", false}};
    struct Finalize {
        std::function<void()> action;
        ~Finalize() { action(); }
    } finalize{[&] {
        receipt.insert("completed", completed && !QTest::currentTestFailed());
        receipt.insert("last_stage", stage); receipt.insert("elapsed_ms", clock.elapsed());
        receipt.insert("images", images); receipt.insert("sources", sources); receipt.insert("motion", motion);
        QSaveFile file(receiptPath);
        const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QTest::qFail("Cannot preserve production preview receipt", __FILE__, __LINE__);
    }};
    const auto desktopName = [](HDESK desktop) {
        wchar_t name[256]{}; DWORD needed = 0;
        return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)
            ? QString::fromWCharArray(name) : QString{};
    };
    const auto ownDesktop = desktopName(GetThreadDesktop(GetCurrentThreadId()));
    const auto inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const auto inputName = desktopName(inputDesktop);
    if (inputDesktop) CloseDesktop(inputDesktop);
    receipt.insert("private_desktop", ownDesktop); receipt.insert("input_desktop", inputName);
    QVERIFY2(!ownDesktop.isEmpty() && !inputName.isEmpty() && ownDesktop != inputName
        && ownDesktop == qEnvironmentVariable("AUD_RESOURCE_PREVIEW_DESKTOP"), "Use the owned private Desktop host");
    UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto root = std::filesystem::path(fixture.path().toStdWString());
    const auto archivePath = root / "original-resource-evidence.w3x";
    auto modelBytes = AnimationTest::Bytes();
    // 原创32x32调色板BLP与短正弦波；低振幅避免短试听突兀。
    std::vector<std::byte> texture(1180 + 32 * 32);
    std::memcpy(texture.data(), "BLP1", 4);
    const auto word = [](auto& bytes, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (8 * i)) & 255u);
    };
    word(texture, 4, 1); word(texture, 12, 32); word(texture, 16, 32); word(texture, 28, 1180); word(texture, 92, 32 * 32);
    texture[156] = std::byte{60}; texture[157] = std::byte{160}; texture[158] = std::byte{240};
    texture[160] = std::byte{170}; texture[161] = std::byte{55}; texture[162] = std::byte{85};
    for (std::size_t i = 0; i < 32 * 32; ++i) texture[1180 + i] = static_cast<std::byte>(((i / 32) / 4 + (i % 32) / 4) % 2);
    constexpr std::uint32_t samples = 22050;
    std::vector<std::byte> sound(44 + samples * 2);
    std::memcpy(sound.data(), "RIFF", 4); word(sound, 4, static_cast<std::uint32_t>(sound.size() - 8));
    std::memcpy(sound.data() + 8, "WAVEfmt ", 8); word(sound, 16, 16);
    sound[20] = std::byte{1}; sound[22] = std::byte{1}; word(sound, 24, samples); word(sound, 28, samples * 2);
    sound[32] = std::byte{2}; sound[34] = std::byte{16}; std::memcpy(sound.data() + 36, "data", 4); word(sound, 40, samples * 2);
    for (std::uint32_t i = 0; i < samples; ++i) {
        const auto value = static_cast<std::int16_t>(500 * std::sin(6.283185307179586 * 440 * i / samples));
        sound[44 + i * 2] = static_cast<std::byte>(static_cast<std::uint16_t>(value) & 255);
        sound[45 + i * 2] = static_cast<std::byte>(static_cast<std::uint16_t>(value) >> 8);
    }
    std::vector<Mpq::MpqBuildFile> files{{"EvidenceModel.mdx", modelBytes}, {"EvidenceModelB.mdx", modelBytes},
        {"EvidenceIcon.blp", texture}, {"Textures/Crate.blp", texture}, {"EvidenceSound.wav", sound}};
    auto imports = Resources::ImportTable::CreateEmpty();
    for (const auto& file : files) QVERIFY(imports.Upsert(file.name).Succeeded());
    auto importBytes = imports.Serialize(); QVERIFY(importBytes.Succeeded());
    files.push_back({"war3map.imp", *importBytes.value});
    QVERIFY(Mpq::WriteMpqArchive(archivePath, files).Succeeded());
    auto archive = MapArchiveSession::Open(archivePath); QVERIFY(archive.Succeeded());
    const auto originalSha = Sha256File(archivePath); QVERIFY(originalSha.Succeeded());
    receipt.insert("source_sha256", QString::fromStdString(*originalSha.value));
    QtProject::EditorProject project; QVERIFY(InstallModelDefinitions(project, true));
    project.projectRoot_ = root; project.sourceArchivePath_ = archivePath;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.war3GameRoot_ = qEnvironmentVariable("FORGE_WORLD_GAME_ROOT").toStdWString();
    // 四类字段使用同一正式资源服务与原创物编元数据。
    auto ownedData = LniObjectCodec(ObjectData::ObjectType::Unit).Parse(Bytes("[u001]\nfile=\"EvidenceModel.mdx\"\n"));
    QVERIFY(ownedData.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(ownedData.document.release()));
    QVERIFY(project.objectDocuments_[0]);
    auto factory = QtApp::MakeObjectResourcePickerFactory(project);
    const auto captureWidget = [&](QWidget& widget, const QString& name) {
        const auto image = widget.grab();
        if (image.isNull() || !image.save(QDir(output).filePath(name + QStringLiteral(".png")))) return false;
        images.append(QJsonObject{{"file", name + ".png"}, {"method", "QWidget::grab"}, {"resampled", false},
            {"logical_width", widget.width()}, {"logical_height", widget.height()}, {"dpr", widget.devicePixelRatioF()},
            {"physical_width", image.width()}, {"physical_height", image.height()}});
        return true;
    };
    const auto waitUntil = [](const std::function<bool()>& predicate, int timeout = 10000) {
        QElapsedTimer wait; wait.start();
        while (!predicate()) { if (wait.elapsed() >= timeout) return false; QTest::qWait(10); }
        return true;
    };
    bool animationVerified = false, audioVerified = false;
    const std::array<std::pair<Services::ObjectResourceField, QString>, 4> fields{{
        {{Services::ObjectResourceKind::Model, false, true}, "EvidenceModel.mdx"},
        {{Services::ObjectResourceKind::Icon, false, true}, "EvidenceIcon.blp"},
        {{Services::ObjectResourceKind::Model, true, true}, "EvidenceModel.mdx,EvidenceModelB.mdx"},
        {{Services::ObjectResourceKind::SoundFile, false, true}, "EvidenceSound.wav"}}};
    for (std::size_t field = 0; field < fields.size(); ++field) {
        stage = QStringLiteral("field-%1").arg(field);
        QWidget owner; owner.setWindowTitle(QStringLiteral("原创资源 · 生产选择面板补证"));
        owner.resize(1920, 1080); owner.show();
        auto* picker = new ObjectResourcePicker(fields[field].first, fields[field].second, factory(), &owner);
        QPointer<ObjectResourcePicker> pickerGuard(picker);
        picker->setAttribute(Qt::WA_ShowWithoutActivating); picker->show();
        auto* tabs = picker->findChild<QTabWidget*>("resourceSourceTabs");
        auto* list = picker->findChild<QTreeWidget*>("resourceCandidates");
        auto* path = picker->findChild<QLineEdit*>("resourcePath");
        auto* search = picker->findChild<QLineEdit*>("resourceSearch");
        auto* status = picker->findChild<QLabel*>("resourceStatus");
        auto* source = picker->findChild<QLabel*>("resourcePreviewSource");
        QVERIFY(tabs && list && path && search && status && source);
        const auto chosen = fields[field].first.orderedList ? QStringLiteral("EvidenceModel.mdx") : fields[field].second;
        for (int origin = 0; origin < 3; ++origin) {
            tabs->setCurrentIndex(origin);
            if (origin < 2) {
                QVERIFY(waitUntil([&] { return !status->text().contains(QStringLiteral("正在建立")); }));
                search->setText(chosen);
                // 等正式100ms搜索去抖结束再借用item；Requery会清空并重建整个候选列表。
                QTest::qWait(130);
                QTreeWidgetItem* item = nullptr;
                QVERIFY2(waitUntil([&] {
                    for (int row = 0; row < list->topLevelItemCount(); ++row) {
                        auto* candidate = list->topLevelItem(row);
                        if (!candidate->data(0, Qt::UserRole + 1).toBool()
                            && candidate->data(0, Qt::UserRole).toString().compare(chosen, Qt::CaseInsensitive) == 0) {
                            item = candidate; return true;
                        }
                    }
                    return false;
                }), qPrintable(status->text()));
                list->setCurrentItem(item); list->scrollToItem(item);
                const auto point = list->visualItemRect(item).center();
                QMouseEvent hover(QEvent::MouseMove, point, list->viewport()->mapToGlobal(point),
                    Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &hover); // 不移动系统鼠标。
                QTest::qWait(170); // 正式面板的悬停去抖为140ms。
                QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(item).center());
                list->setFocus(); QTest::keyClick(list, Qt::Key_Return);
            } else {
                path->setFocus(); path->selectAll(); QTest::keyClicks(path, chosen);
            }
            const bool ready = waitUntil([&] { return !source->text().contains(QStringLiteral("正在读取"))
                && source->text().contains(QStringLiteral("world-map-")); });
            sources.append(QJsonObject{{"field", int(field)}, {"tab", origin}, {"source_text", source->text()},
                {"candidate_rows", list->topLevelItemCount()}, {"status", status->text()}, {"selected_path", path->text()}, {"preview_ready", ready}});
            if (!ready) captureWidget(*picker, QStringLiteral("failed-field-%1-source-%2").arg(field).arg(origin));
            QVERIFY2(ready, qPrintable(source->text()));
            QVERIFY(captureWidget(*picker, QStringLiteral("field-%1-source-%2").arg(field).arg(origin)));
        }
        if (field == 0) {
            auto* player = picker->findChild<WorldAnimationPlayer*>();
            auto* viewport = picker->findChild<WorldViewport*>();
            auto* play = picker->findChild<QPushButton*>("worldUnitPreviewPlayPause");
            auto* action = picker->findChild<QComboBox*>("worldUnitPreviewAction");
            QVERIFY(player && viewport && play && action);
            QVERIFY2(waitUntil([&] { return player->Ready() && viewport->RendererReady() && player->Frame(); }, 20000), qPrintable(viewport->StatusText()));
            player->SetPlaying(false); QVERIFY(waitUntil([&] { return !player->Busy(); }));
            QVERIFY(action->count() >= 2); action->setCurrentIndex(1);
            QVERIFY(waitUntil([&] { return !player->Busy() && player->Frame()->states.front().sequenceIndex == 1; }));
            std::array<Render::CapturedFrameRgba, 2> captured;
            const auto completedFrame = [&](int index) {
                return waitUntil([&] {
                    auto current = viewport->CaptureCompletedFrame(viewport->DocumentRevision());
                    if (!current.Succeeded() || !current.value->animation
                        || current.value->animation->contentVersion != player->Frame()->contentVersion) return false;
                    captured[index] = std::move(*current.value); return true;
                });
            };
            QVERIFY(completedFrame(0));
            const auto start = player->Frame()->states.front().elapsedMs;
            QTest::mouseClick(play, Qt::LeftButton);
            QVERIFY(waitUntil([&] { return player->Playing() && player->Frame()->states.front().elapsedMs >= start + 200; }));
            QTest::mouseClick(play, Qt::LeftButton); QVERIFY(waitUntil([&] { return !player->Busy(); }));
            QVERIFY(!player->Playing()); QVERIFY(completedFrame(1));
            QVERIFY(captured[0].rgba != captured[1].rgba);
            QVERIFY(captured[0].animation->poses.front().matrixHash != captured[1].animation->poses.front().matrixHash);
            for (int index = 0; index < 2; ++index) {
                const auto& frame = captured[index]; const auto name = QStringLiteral("model-completed-%1.png").arg(index);
                const QImage image(reinterpret_cast<const uchar*>(frame.rgba.data()), int(frame.view.physicalWidth),
                    int(frame.view.physicalHeight), int(frame.view.physicalWidth * 4), QImage::Format_RGBA8888);
                QVERIFY(image.save(QDir(output).filePath(name)));
                motion.append(QJsonObject{{"file", name}, {"method", "CaptureCompletedFrame"},
                    {"frame", QString::number(frame.view.identity.frameCount)}, {"content_version", QString::number(frame.animation->contentVersion)},
                    {"elapsed_ms", QString::number(frame.animation->states.front().elapsedMs)},
                    {"rgba_sha256", QString::fromStdString(*Sha256(frame.rgba).value)}});
            }
            receipt.insert("renderer", viewport->BackendDiagnosticSnapshot()); animationVerified = true;
        }
        if (field == 3) {
            auto* play = picker->findChild<QPushButton*>("resourceSoundPlay"); QVERIFY(play);
            QVERIFY(waitUntil([&] { return play->isEnabled(); })); QTest::mouseClick(play, Qt::LeftButton);
            auto* audio = picker->findChild<SoundPreviewPlayer*>(); QVERIFY(audio);
            auto* media = audio->findChild<QMediaPlayer*>(); QVERIFY(media);
            QVERIFY2(waitUntil([&] { return media->playbackState() == QMediaPlayer::PlayingState && media->position() >= 100; }), qPrintable(media->errorString()));
            receipt.insert("audio", QJsonObject{{"explicit_play_button", true}, {"playing_state", int(media->playbackState())},
                {"position_ms", media->position()}, {"duration_ms", media->duration()}, {"error", media->errorString()}});
            audio->Stop(); QCOMPARE(media->playbackState(), QMediaPlayer::StoppedState); audioVerified = true;
        }
        if (field < 2 && !project.war3GameRoot_.empty()) {
            stage = QStringLiteral("game-catalog-%1").arg(field);
            tabs->setCurrentIndex(0);
            auto* browse = picker->findChild<QComboBox*>("resourceBrowseMode"); QVERIFY(browse);
            search->setText(field == 0 ? QStringLiteral(".mdx") : QStringLiteral(".blp"));
            QElapsedTimer indexing; indexing.start();
            qint64 previous = 0, longestGap = 0; int heartbeats = 0; bool progressSeen = false;
            QTimer heartbeat;
            connect(&heartbeat, &QTimer::timeout, &owner, [&] {
                const auto now = indexing.elapsed(); longestGap = std::max(longestGap, now - previous); previous = now; ++heartbeats;
                progressSeen |= status->text().contains(QStringLiteral("正在建立"));
            });
            heartbeat.start(10); browse->setCurrentIndex(1);
            QVERIFY2(waitUntil([&] { return list->topLevelItemCount() > 0 && !status->text().contains(QStringLiteral("正在建立")); }, 30000), qPrintable(status->text()));
            const auto coldMs = indexing.elapsed();
            const auto discovered = list->topLevelItem(0)->data(0, Qt::UserRole).toString();
            QVERIFY(!discovered.isEmpty()); search->setText(discovered);
            QVERIFY(waitUntil([&] { return list->topLevelItemCount() == 1
                && list->topLevelItem(0)->data(0, Qt::UserRole).toString() == discovered; }));
            heartbeat.stop();
            sources.append(QJsonObject{{"field", int(field)}, {"origin", "game-read-only"}, {"cold_index_ms", coldMs},
                {"warm_query_with_debounce_ms", indexing.elapsed() - coldMs}, {"ui_timer_ticks", heartbeats},
                {"largest_ui_timer_gap_ms", longestGap}, {"progress_seen", progressSeen}, {"status", status->text()}});
            QVERIFY(heartbeats > 0); QVERIFY(captureWidget(*picker, QStringLiteral("field-%1-game-catalog").arg(field)));
        }
        picker->close(); QTRY_VERIFY(!pickerGuard);
    }
    const auto after = Sha256File(archivePath); QVERIFY(after.Succeeded()); QCOMPARE(*after.value, *originalSha.value);
    receipt.insert("source_unchanged", true); receipt.insert("animation_completed_frames_verified", animationVerified);
    receipt.insert("audio_playback_state_verified", audioVerified);
    receipt.insert("game_catalog_measured", !project.war3GameRoot_.empty());
    receipt.insert("uncovered", "Full MainWindow anchor geometry and archive save reopen remain separate gates; audio evidence measures player state, not human listening.");
    QVERIFY(animationVerified && audioVerified); completed = true;
}
int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsEmpty("AUD_RESOURCE_PREVIEW_EVIDENCE_DIR")) {
        bool explicitCase = false;
        for (int i = 1; i < argc; ++i) explicitCase |= QByteArray(argv[i]) == "OwnedProductionResourceEvidence";
        if (!explicitCase || qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("windows")) return 2;
    } else qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    ObjectResourcePickerTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "ObjectResourcePickerTests.moc"
