#include "EditorProject.h"
#include "ObjectModule.h"
#include "ObjectModels.h"
#include "ObjectNames.h"
#include "SyntheticObjectCatalogFixture.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ObjectTreeBrowser.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Game/GameDataSource.h"

#include <QStandardItemModel>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>

namespace {
using namespace War3Map::ObjectData;

std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return {begin, begin + value.size()};
}

bool InstallSyntheticDefinitions(War3Map::QtProject::EditorProject& project) {
    const std::map<std::string, std::string> files{
        {"unit.slk", "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"HP\"\n"
            "C;X1;Y2;K\"hfoo\"\nC;X2;K200\nC;X1;Y3;K\"hrif\"\nC;X2;K180\nE\n"},
        {"unit-meta.slk", "ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;K\"field\"\n"
            "C;X3;K\"slk\"\nC;X4;K\"type\"\nC;X5;K\"displayName\"\n"
            "C;X6;K\"category\"\nC;X7;K\"minVal\"\nC;X8;K\"maxVal\"\n"
            "C;X1;Y2;K\"uhpm\"\nC;X2;K\"HP\"\nC;X3;K\"UnitBalance\"\n"
            "C;X4;K\"int\"\nC;X5;K\"生命上限\"\nC;X6;K\"stats\"\n"
            "C;X7;K0\nC;X8;K1000\nE\n"}};
    const auto reader = [&files](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end()
            ? War3Map::Result<std::vector<std::byte>>::Failure("test", "missing", path)
            : War3Map::Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    CatalogLoadOptions catalogOptions;
    catalogOptions.sources = {{ObjectType::Unit, {"unit.slk"}, {}}};
    catalogOptions.stringFiles.clear();
    auto catalog = StandardObjectCatalog::Load(reader, catalogOptions);
    ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.sources = {{ObjectType::Unit, "unit-meta.slk"}};
    schemaOptions.stringFiles.clear();
    schemaOptions.worldEditDataFile.clear();
    schemaOptions.unitEditorDataFile.clear();
    auto schema = ObjectFieldSchema::Load(reader, schemaOptions);
    if (!catalog.Succeeded() || !schema.Succeeded()) return false;
    if (!War3Map::Tests::InstallSyntheticGameCatalog(project, *catalog.value)) return false;
    project.objectFieldSchema_ = std::make_unique<ObjectFieldSchema>(std::move(*schema.value));
    return true;
}

std::unique_ptr<LniObjectDocument> EmptyUnitDocument() {
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse({});
    if (!parsed.Succeeded()) return {};
    return std::unique_ptr<LniObjectDocument>(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
}

QStandardItem* HealthItem(QStandardItemModel* model) {
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->item(row, 0)->data(Qt::UserRole).toString() == QStringLiteral("hp"))
            return model->item(row, 1);
    }
    return nullptr;
}

std::optional<std::int32_t> HealthOverride(const LniObjectDocument* document) {
    const auto* object = document ? document->FindObject({ObjectType::Unit, "hfoo"}) : nullptr;
    if (!object) return {};
    for (const auto& modification : object->modifications) {
        if (modification.key.fieldId == "uhpm") {
            if (const auto* value = std::get_if<std::int32_t>(&modification.value)) return *value;
        }
    }
    return {};
}
} // namespace

class ObjectDirectEditTests final : public QObject {
    Q_OBJECT
private slots:
    void FirstStandardFieldEditIsOneUndoableMapOverride();
    void InvalidAndUnavailableEditsLeaveDocumentUntouched();
    void BatchEditUsesOneUndoAndRejectsInvalidTarget();
    void GeneratedDocumentReplacementPreservesSelectionAndRebindsUndo();
    void MapFieldNamesUseSharedStringResolutionWithoutWritingRawValues();
    void RealAudastraBatchNamesReadOnlyProjection();
};

void ObjectDirectEditTests::RealAudastraBatchNamesReadOnlyProjection() {
    const QString samples = qEnvironmentVariable("WAR3_NAME_SAMPLE_DIR");
    const QString gameRoot = qEnvironmentVariable("WAR3_NAME_GAME_ROOT");
    const QString sourceMap = qEnvironmentVariable("WAR3_NAME_SOURCE_MAP");
    const QString output = qEnvironmentVariable("WAR3_TEST_SCREENSHOT_DIR");
    if (samples.isEmpty() || gameRoot.isEmpty() || sourceMap.isEmpty() || output.isEmpty())
        QSKIP("真实来源批量列表验收需明确 sample/game/source-map/output opt-in；默认不读作者文件。");
    const auto read = [&](const char* member) {
        QFile file(QDir(samples).filePath(QString::fromLatin1(member)));
        if (!file.open(QIODevice::ReadOnly)) return std::vector<std::byte>{};
        const auto data = file.readAll();
        const auto* begin = reinterpret_cast<const std::byte*>(data.constData());
        return std::vector<std::byte>(begin, begin + data.size());
    };
    const auto nativeBytes = read("war3map.w3u"), wts = read("war3map.wts");
    QVERIFY(!nativeBytes.empty() && !wts.empty());
    auto native = NativeObjectCodec(ObjectType::Unit).Parse(nativeBytes);
    QVERIFY(native.Succeeded() && native.document);
    auto projected = ConvertNativeObjectDataToLni(ObjectType::Unit, nativeBytes);
    QVERIFY(projected.Succeeded() && projected.value);
    auto editable = LniObjectCodec(ObjectType::Unit).Parse(*projected.value);
    QVERIFY(editable.Succeeded() && editable.document);
    auto game = War3Map::Game::GameDataSource::MountStrict(gameRoot.toStdWString());
    QVERIFY(game.Succeeded() && game.value);
    auto catalog = StandardObjectCatalog::Load(*game.value);
    auto schema = ObjectFieldSchema::Load(*game.value);
    QVERIFY(catalog.Succeeded() && catalog.value && schema.Succeeded() && schema.value);
    War3Map::QtProject::EditorProject project;
    project.nativeObjectDocuments_[0].reset(dynamic_cast<NativeObjectDocument*>(native.document.release()));
    project.objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(editable.document.release()));
    QVERIFY(project.nativeObjectDocuments_[0] && project.objectDocuments_[0]);
    QVERIFY(project.objectDocuments_[0]->IsStructurallySafe());
    project.standardCatalog_ = std::make_unique<StandardObjectCatalog>(std::move(*catalog.value));
    project.objectFieldSchema_ = std::make_unique<ObjectFieldSchema>(std::move(*schema.value));
    project.mapDocument_ = std::make_unique<War3Map::MapDocument>(War3Map::MapDocument::FromMembers(
        std::vector<War3Map::MapArchiveMember>{{"war3map.wts", wts}, {"war3map.w3u", nativeBytes}}));
    project.mapEdits_ = std::make_unique<War3Map::MapData::MapDataEditSession>(*project.mapDocument_);
    project.objectUndoStacks_[0] = new QUndoStack(&project);
    QVERIFY(project.iconProvider_->SetGameRoot(gameRoot.toStdWString()));
    QVERIFY(project.iconProvider_->SetMapArchive(sourceMap.toStdWString()));
    const auto nativeBefore = NativeObjectCodec(ObjectType::Unit).Serialize(*project.nativeObjectDocuments_[0]);
    QVERIFY(nativeBefore.Succeeded() && nativeBefore.value && *nativeBefore.value == nativeBytes);
    const auto lniFingerprint = project.objectDocuments_[0]->DocumentFingerprint();
    const auto etyr = ResolveProjectObjectPresentation(project, {ObjectType::Unit, "Etyr"});
    const auto hamg = ResolveProjectObjectPresentation(project, {ObjectType::Unit, "Hamg"});
    QCOMPARE(etyr.name, QStringLiteral("和蔼可亲的精灵龙"));
    QCOMPARE(etyr.nameReference, QStringLiteral("TRIGSTR_469"));
    QCOMPARE(hamg.name, QStringLiteral("大魔法师"));
    QVERIFY(hamg.nameReference.isEmpty());
    qInfo() << "real-source-batch" << "Etyr" << etyr.name << etyr.nameReference << "Hamg" << hamg.name;
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
    War3Map::UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    War3Map::Modules::ObjectModule module(project);
    auto* page = module.Widget(); page->resize(1450, 900); page->show();
    module.objectFieldModel_->SetContext(project.standardCatalog_.get(), project.objectFieldSchema_.get());
    module.SelectFirstType();
    module.UpdateObjectReferenceEntries();
    module.SelectMapObject(0, "Hamg");
    const auto capture = [&](bool rawMode) {
        module.showRawFields_->setChecked(rawMode);
        bool opened = false, verified = false, saved = false;
        QObject observationScope;
        QTimer::singleShot(0, &observationScope, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            opened = true;
            auto* objects = dialog->findChild<QListWidget*>("batchObjectList");
            if (!objects) { dialog->reject(); return; }
            QListWidgetItem* etyrItem = nullptr;
            QListWidgetItem* hamgItem = nullptr;
            for (int row = 0; row < objects->count(); ++row) {
                auto* item = objects->item(row);
                if (item->data(Qt::UserRole).toString() == "Etyr") etyrItem = item;
                if (item->data(Qt::UserRole).toString() == "Hamg") hamgItem = item;
            }
            if (!etyrItem || !hamgItem) { dialog->reject(); return; }
            verified = etyrItem->text().contains(rawMode ? QStringLiteral("Etyr  ·  Etyr") : etyr.name)
                && hamgItem->text().contains(rawMode ? QStringLiteral("Hamg  ·  Hamg") : hamg.name)
                && etyrItem->toolTip().contains(QStringLiteral("TRIGSTR_469"))
                && (rawMode || (!etyrItem->icon().isNull() && !hamgItem->icon().isNull()));
            dialog->resize(850, 620);
            auto* provenance = new QLabel(rawMode
                ? QStringLiteral("当前程序原始字段对照（非旧程序截图）· Etyr 名称来源 TRIGSTR_469")
                : QStringLiteral("当前程序统一名称解析 · 真实 W3U/WTS 经生产内存 LNI 投影 · 未应用修改"), dialog);
            provenance->setWordWrap(true);
            static_cast<QVBoxLayout*>(dialog->layout())->insertWidget(0, provenance);
            QApplication::processEvents();
            objects->scrollToItem(etyrItem, QAbstractItemView::PositionAtTop);
            QApplication::processEvents();
            verified = verified && objects->viewport()->rect().contains(objects->visualItemRect(etyrItem))
                && objects->viewport()->rect().contains(objects->visualItemRect(hamgItem));
            QDir().mkpath(output);
            saved = dialog->grab().save(QDir(output).filePath(rawMode
                ? QStringLiteral("n1-real-batch-raw-projection.png") : QStringLiteral("n1-real-batch-resolved-projection.png")));
            dialog->reject();
        });
        // 真实批量入口与同一字段/对象列表；定时器只观察并取消，绝不触发“应用修改”。
        module.BatchEditObjects();
        return opened && verified && saved;
    };
    QVERIFY(capture(true));
    QVERIFY(capture(false));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 0);
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), lniFingerprint);
    const auto nativeAfter = NativeObjectCodec(ObjectType::Unit).Serialize(*project.nativeObjectDocuments_[0]);
    QVERIFY(nativeAfter.Succeeded() && nativeAfter.value && *nativeAfter.value == nativeBytes);
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
}

void ObjectDirectEditTests::MapFieldNamesUseSharedStringResolutionWithoutWritingRawValues() {
    War3Map::QtProject::EditorProject project;
    QVERIFY(InstallSyntheticDefinitions(project));
    const ObjectKey key{ObjectType::Unit, "H001"};
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes("[H001]\n_parent=\"hfoo\"\n"));
    QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
    QVERIFY(project.objectDocuments_[0]);
    const std::string reference = "\"TRIGSTR_008\"";
    QVERIFY(project.objectDocuments_[0]->SetModification(key, ModificationKey{"unam"}, reference).Succeeded());
    const auto fingerprint = project.objectDocuments_[0]->DocumentFingerprint();
    project.objectUndoStacks_[0] = new QUndoStack(&project);
    const std::string sourceBody = "|cffff0000\xba\xec\xc9\xab|r\r\n\xb5\xda\xb6\xfe\xd0\xd0";
    const auto wts = Bytes("STRING 8\r\n{\r\n" + sourceBody + "\r\n}\r\n");
    project.mapDocument_ = std::make_unique<War3Map::MapDocument>(War3Map::MapDocument::FromMembers(
        std::vector<War3Map::MapArchiveMember>{{"war3map.wts", wts}}));
    project.mapEdits_ = std::make_unique<War3Map::MapData::MapDataEditSession>(*project.mapDocument_);
    War3Map::Modules::ObjectModule module(project);
    (void)module.Widget();
    module.SelectFirstType();
    module.UpdateObjectReferenceEntries();
    module.SelectMapObject(0, "H001");
    auto* fields = module.objectFieldModel_;
    QVERIFY(module.HasLoadedContent());
    QCOMPARE(module.objectListView_->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("H001"));
    const auto nameValue = [&](int role) {
        for (int row = 0; row < fields->rowCount(); ++row) {
            if (fields->data(fields->index(row, 0), Qt::UserRole).toString() == "unam")
                return fields->data(fields->index(row, 3), role).toString();
        }
        return QString{};
    };
    QVERIFY(nameValue(Qt::DisplayRole).contains(QStringLiteral("红色")));
    QVERIFY(nameValue(Qt::DisplayRole).contains(QStringLiteral("第二行")));
    QCOMPARE(nameValue(Qt::EditRole), QString::fromStdString(reference));
    module.showRawFields_->setChecked(true);
    QCOMPARE(nameValue(Qt::DisplayRole), QString::fromStdString(reference));
    module.showRawFields_->setChecked(false);
    auto strings = *project.mapEdits_->Snapshot().Strings();
    strings.entries[0].value = "当前正文";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded());
    emit project.MapDocumentChanged();
    QCOMPARE(nameValue(Qt::DisplayRole), QStringLiteral("当前正文"));
    QCOMPARE(nameValue(Qt::EditRole), QString::fromStdString(reference));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), fingerprint);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 0);
    QCOMPARE(project.mapDocument_->ResolveString("TRIGSTR_8").value, std::optional<std::string>(sourceBody));
}

void ObjectDirectEditTests::FirstStandardFieldEditIsOneUndoableMapOverride() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    War3Map::QtProject::EditorProject project;
    project.projectRoot_ = std::filesystem::path(workspace.path().toStdWString());
    project.sourceArchivePath_ = project.projectRoot_ / "synthetic.w3x";
    project.objectDocuments_[0] = EmptyUnitDocument();
    QVERIFY(project.objectDocuments_[0]);
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit, 2);
    QVERIFY(native.Succeeded());
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
    project.objectUndoStacks_[0] = new QUndoStack(&project);
    QVERIFY(InstallSyntheticDefinitions(project));

    War3Map::Modules::ObjectModule module(project);
    module.Widget();
    module.UpdateObjectReferenceEntries();
    module.standardObjectBrowser_->SetCurrentId(QStringLiteral("hfoo"));
    auto* health = HealthItem(module.standardFieldModel_);
    QVERIFY(health);
    QVERIFY(health->isEditable());
    QCOMPARE(health->text(), QStringLiteral("200"));
    QVERIFY(!project.objectDocuments_[0]->FindObject({ObjectType::Unit, "hfoo"}));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 0);

    health->setText(QStringLiteral("350"));
    QTRY_COMPARE(module.objectSourceTabs_->currentIndex(), 1);
    QCOMPARE(HealthOverride(project.objectDocuments_[0].get()).value_or(-1), 350);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);
    QCOMPARE(project.standardCatalog_->Find(ObjectType::Unit, "hfoo")->Field("hp"), std::string_view("200"));
    project.objectUndoStacks_[0]->undo();
    QVERIFY(!project.objectDocuments_[0]->FindObject({ObjectType::Unit, "hfoo"}));
    project.objectUndoStacks_[0]->redo();
    QCOMPARE(HealthOverride(project.objectDocuments_[0].get()).value_or(-1), 350);
}

void ObjectDirectEditTests::InvalidAndUnavailableEditsLeaveDocumentUntouched() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    War3Map::QtProject::EditorProject project;
    project.projectRoot_ = std::filesystem::path(workspace.path().toStdWString());
    project.sourceArchivePath_ = project.projectRoot_ / "synthetic.w3x";
    project.objectDocuments_[0] = EmptyUnitDocument();
    QVERIFY(project.objectDocuments_[0]);
    project.objectUndoStacks_[0] = new QUndoStack(&project);
    QVERIFY(InstallSyntheticDefinitions(project));
    War3Map::Modules::ObjectModule module(project);
    module.Widget();
    module.UpdateObjectReferenceEntries();
    module.standardObjectBrowser_->SetCurrentId(QStringLiteral("hfoo"));
    auto* health = HealthItem(module.standardFieldModel_);
    QVERIFY(health);
    const auto original = project.objectDocuments_[0]->DocumentFingerprint();
    QVERIFY(!module.CommitStandardFieldValue(QStringLiteral("hfoo"), QStringLiteral("hp"), QStringLiteral("200")));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), original);

    health->setText(QStringLiteral("1001")); // 超出元数据范围。
    QTRY_COMPARE(health->text(), QStringLiteral("200"));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), original);
    health->setText(QStringLiteral("300")); // 没有可编辑的原生基线。
    QTRY_COMPARE(health->text(), QStringLiteral("200"));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), original);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 0);

    project.objectFieldSchema_.reset();
    QVERIFY(!module.CommitStandardFieldValue(QStringLiteral("hfoo"), QStringLiteral("hp"), QStringLiteral("300")));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), original);
}

void ObjectDirectEditTests::BatchEditUsesOneUndoAndRejectsInvalidTarget() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    War3Map::QtProject::EditorProject project;
    project.projectRoot_ = std::filesystem::path(workspace.path().toStdWString());
    project.sourceArchivePath_ = project.projectRoot_ / "synthetic.w3x";
    project.objectDocuments_[0] = EmptyUnitDocument();
    QVERIFY(project.objectDocuments_[0]);
    QVERIFY(project.objectDocuments_[0]->AddOriginalObject("hfoo").Succeeded());
    QVERIFY(project.objectDocuments_[0]->AddOriginalObject("hrif").Succeeded());
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit, 2);
    QVERIFY(native.Succeeded());
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
    project.objectUndoStacks_[0] = new QUndoStack(&project);
    QVERIFY(InstallSyntheticDefinitions(project));
    War3Map::Modules::ObjectModule module(project);
    module.Widget();
    const std::vector<ObjectKey> targets{{ObjectType::Unit, "hfoo"}, {ObjectType::Unit, "hrif"}};
    const ModificationKey health{"uhpm"};
    const auto before = project.objectDocuments_[0]->DocumentFingerprint();
    QVERIFY(module.CommitBatchFieldValue(targets, health, QStringLiteral("350")));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);
    QCOMPARE(HealthOverride(project.objectDocuments_[0].get()).value_or(-1), 350);
    const auto* second = project.objectDocuments_[0]->FindObject({ObjectType::Unit, "hrif"});
    QVERIFY(second);
    QCOMPARE(std::get<std::int32_t>(second->modifications.front().value), 350);
    project.objectUndoStacks_[0]->undo();
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), before);
    project.objectUndoStacks_[0]->redo();
    const auto edited = project.objectDocuments_[0]->DocumentFingerprint();
    QVERIFY(!module.CommitBatchFieldValue(targets, health, QStringLiteral("1001")));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), edited);
    auto invalid = targets;
    invalid[1].rawcode = "miss";
    QVERIFY(!module.CommitBatchFieldValue(invalid, health, QStringLiteral("450")));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), edited);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);

    bool dialogOpened = false;
    QTimer::singleShot(0, &module, [&] {
        auto* dialog = module.Widget()->findChild<QDialog*>();
        if (!dialog) return;
        dialogOpened = true;
        auto* objects = dialog->findChild<QListWidget*>(QStringLiteral("batchObjectList"));
        auto* fields = dialog->findChild<QComboBox*>(QStringLiteral("batchField"));
        auto* entry = dialog->findChild<QLineEdit*>(QStringLiteral("batchValue"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!objects || !fields || !entry || !buttons) { dialog->reject(); return; }
        for (int row = 0; row < objects->count(); ++row) objects->item(row)->setCheckState(Qt::Checked);
        for (int row = 0; row < fields->count(); ++row) {
            if (fields->itemText(row).contains(QStringLiteral("生命上限"))) fields->setCurrentIndex(row);
        }
        entry->setText(QStringLiteral("470"));
        buttons->button(QDialogButtonBox::Apply)->click();
        if (dialog->isVisible()) dialog->reject();
    });
    module.BatchEditObjects();
    QVERIFY(dialogOpened);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 2);
    QCOMPARE(HealthOverride(project.objectDocuments_[0].get()).value_or(-1), 470);
}

void ObjectDirectEditTests::GeneratedDocumentReplacementPreservesSelectionAndRebindsUndo() {
    War3Map::QtProject::EditorProject project;
    QVERIFY(InstallSyntheticDefinitions(project));
    const ObjectKey first{ObjectType::Unit, "hfoo"};
    const ObjectKey selected{ObjectType::Unit, "hrif"};
    const ModificationKey health{"uhpm"};
    project.objectDocuments_[0] = EmptyUnitDocument();
    QVERIFY(project.objectDocuments_[0]);
    QVERIFY(project.objectDocuments_[0]->AddOriginalObject(first.rawcode).Succeeded());
    QVERIFY(project.objectDocuments_[0]->AddOriginalObject(selected.rawcode).Succeeded());
    QVERIFY(project.objectDocuments_[0]->SetModification(first, health, std::int32_t{350}).Succeeded());
    QVERIFY(project.objectDocuments_[0]->SetModification(selected, health, std::int32_t{450}).Succeeded());
    project.objectUndoStacks_[0] = new QUndoStack(&project);

    War3Map::Modules::ObjectModule module(project);
    module.Widget();
    module.objectFieldModel_->SetContext(project.standardCatalog_.get(), project.objectFieldSchema_.get());
    module.UpdateObjectReferenceEntries();
    module.ShowObjectType(0);
    module.SelectMapObject(0, QStringLiteral("hrif"));
    QCOMPARE(module.objectListView_->currentIndex().row(), 1);
    QCOMPARE(module.objectListView_->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("hrif"));
    QCOMPARE(module.objectFieldModel_->rowCount(), 1);
    QVERIFY(module.objectFieldModel_->setData(module.objectFieldModel_->index(0, 3), QStringLiteral("600"), Qt::EditRole));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);

    auto replacement = EmptyUnitDocument();
    QVERIFY(replacement);
    // 改变底层次序并插入排序靠前的生成对象，确保恢复依赖 rawcode 而非原行号或旧模型。
    QVERIFY(replacement->AddOriginalObject(selected.rawcode).Succeeded());
    QVERIFY(replacement->AddOriginalObject(first.rawcode).Succeeded());
    QVERIFY(replacement->AddObject("h001", first.rawcode).Succeeded());
    QVERIFY(replacement->SetModification(selected, health, std::int32_t{725}).Succeeded());
    QVERIFY(replacement->SetModification(first, health, std::int32_t{375}).Succeeded());
    emit project.ObjectDocumentsAboutToReplace();
    QCOMPARE(module.objectListModel_->rowCount(), 0);
    QCOMPARE(module.objectFieldModel_->rowCount(), 0);
    QVERIFY(!module.objectListView_->currentIndex().isValid());
    // 与保存提交相同：旧命令先清理，再析构旧文档；模型必须已解除引用。
    project.objectUndoStacks_[0]->clear();
    project.objectDocuments_[0] = std::move(replacement);
    emit project.ObjectDocumentsReplaced();
    QCOMPARE(module.objectListView_->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("hrif"));
    QCOMPARE(module.objectListView_->currentIndex().row(), 2);
    const auto treeSelection = module.objectTree_->Current();
    QVERIFY(treeSelection.has_value());
    QCOMPARE(treeSelection->first, QStringLiteral("hrif"));
    QCOMPARE(module.objectFieldModel_->data(module.objectFieldModel_->index(0, 3), Qt::EditRole).toString(), QStringLiteral("725"));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 0);
    QCOMPARE(module.UndoStack(), project.objectUndoStacks_[0]);
    QVERIFY(module.objectFieldModel_->setData(module.objectFieldModel_->index(0, 3), QStringLiteral("850"), Qt::EditRole));
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);
    QCOMPARE(std::get<std::int32_t>(project.objectDocuments_[0]->FindObject(selected)->modifications.front().value), 850);
    QCOMPARE(HealthOverride(project.objectDocuments_[0].get()).value_or(-1), 375);
    project.objectUndoStacks_[0]->undo();
    QCOMPARE(module.objectFieldModel_->data(module.objectFieldModel_->index(0, 3), Qt::EditRole).toString(), QStringLiteral("725"));
    project.objectUndoStacks_[0]->redo();
    QCOMPARE(module.objectFieldModel_->data(module.objectFieldModel_->index(0, 3), Qt::EditRole).toString(), QStringLiteral("850"));

    module.ShowObjectType(1);
    module.ShowObjectType(0);
    QCOMPARE(module.objectListView_->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("hrif"));
    // 新基线删除已选对象时沿用现有首行回退，不保留无效模型索引。
    replacement = EmptyUnitDocument();
    QVERIFY(replacement);
    QVERIFY(replacement->AddOriginalObject(first.rawcode).Succeeded());
    emit project.ObjectDocumentsAboutToReplace();
    project.objectUndoStacks_[0]->clear();
    project.objectDocuments_[0] = std::move(replacement);
    emit project.ObjectDocumentsReplaced();
    QCOMPARE(module.objectListView_->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("hfoo"));
    QVERIFY(!project.objectUndoStacks_[0]->canUndo());
}

QTEST_MAIN(ObjectDirectEditTests)
#include "ObjectDirectEditTests.moc"
