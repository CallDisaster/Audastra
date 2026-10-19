#include "ObjectModels.h"
#include "ObjectColourPreviewSource.h"
#include "EditorProject.h"
#include "Widgets/WarcraftTextPreview.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QSpinBox>
#include <QTest>
#include <QTextEdit>
#include <QTextCursor>
#include <QUndoStack>
#include <algorithm>
#include <map>
#include <memory>

namespace {
using namespace War3Map::ObjectData;
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}
std::string Table(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows) {
    std::string result = "ID;PWXL;N;E\n";
    auto append = [&result](std::size_t x, std::size_t y, const std::string& text) {
        result += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 1) + ";K\"" + text + "\"\n";
    };
    for (std::size_t x = 0; x < headers.size(); ++x) append(x, 0, headers[x]);
    for (std::size_t y = 0; y < rows.size(); ++y)
        for (std::size_t x = 0; x < rows[y].size(); ++x) append(x, y + 1, rows[y][x]);
    return result + "E\n";
}
struct Fixture {
    ObjectFieldSchema schema;
    StandardObjectCatalog catalog;
    bool valid = false;
    Fixture() {
        const std::vector<std::string> fields{"ID", "field", "slk", "type", "displayName", "category",
            "repeat", "data", "index", "useSpecific", "useUnit", "useHero", "useBuilding", "minVal", "maxVal"};
        const std::map<std::string, std::string> files{
            {"unit-meta.slk", Table(fields, {
                {"uhpm", "HP", "UnitBalance", "int", "生命值", "stats", "0", "0", "-1", "", "1", "1", "1", "0", "100000"},
                {"umvs", "spd", "UnitBalance", "real", "移动速度", "movement", "0", "0", "-1", "", "1", "1", "1", "0", "100000"},
                {"ubdg", "isbldg", "UnitData", "bool", "建筑", "stats", "0", "0", "-1", "", "1", "1", "1", "0", "1"},
                {"unam", "Name", "Profile", "string", "名称", "text", "0", "0", "0", "", "1", "1", "1"},
                {"utub", "Ubertip", "Profile", "string", "扩展说明", "text", "0", "0", "0", "", "1", "1", "1"},
                {"uabi", "abilList", "UnitAbilities", "abilityList", "普通技能", "abilities", "0", "0", "-1", "", "1", "1", "1"}
            })},
            {"ability-meta.slk", Table(fields, {
                {"Hbz1", "Data", "AbilityData", "unreal", "暴风雪伤害", "stats", "1", "1", "-1", "AHbz", "1", "1", "1"},
                {"acdn", "Cool", "AbilityData", "real", "冷却时间", "stats", "1", "0", "-1", "", "1", "1", "1"},
                {"atp1", "Tip", "Profile", "string", "提示", "text", "1", "0", "0", "", "1", "1", "1"}
            })},
            {"unit.slk", Table({"ID", "HP", "spd", "isbldg", "abilList", "code"}, {
                {"hfoo", "420", "270.125", "0", "A001,A002", "hfoo"}})},
            {"ability.slk", Table({"ID", "code", "DataA1", "DataA2", "Cool1", "Cool2"}, {
                {"AHbz", "AHbz", "75.5", "100.25", "8", "6"}})},
            {"unit.txt", "[hfoo]\nName=\"步兵\"\nUbertip=\"原版说明\"\n"},
            {"ability.txt", "[AHbz]\nTip=\"第一等级,含逗号\",\"第二等级\"\n"},
            {"world.txt", "[ObjectEditorCategories]\nstats=属性\nmovement=移动\ntext=文本\nabilities=技能\n"}
        };
        const auto reader = [&files](const std::string& path) {
            const auto found = files.find(path);
            return found == files.end()
                ? War3Map::Result<std::vector<std::byte>>::Failure("test", "absent", "合成资源未提供")
                : War3Map::Result<std::vector<std::byte>>::Success(Bytes(found->second));
        };
        ObjectFieldSchemaOptions schemaOptions;
        schemaOptions.sources = {{ObjectType::Unit, "unit-meta.slk"}, {ObjectType::Ability, "ability-meta.slk"}};
        schemaOptions.stringFiles.clear();
        schemaOptions.worldEditDataFile = "world.txt";
        schemaOptions.unitEditorDataFile.clear();
        auto loadedSchema = ObjectFieldSchema::Load(reader, schemaOptions);
        CatalogLoadOptions catalogOptions;
        catalogOptions.sources = {{ObjectType::Unit, {"unit.slk"}, {"unit.txt"}}, {ObjectType::Ability, {"ability.slk"}, {"ability.txt"}}};
        catalogOptions.stringFiles.clear();
        auto loadedCatalog = StandardObjectCatalog::Load(reader, catalogOptions);
        valid = loadedSchema.Succeeded() && loadedCatalog.Succeeded();
        if (valid) {
            schema = std::move(*loadedSchema.value);
            catalog = std::move(*loadedCatalog.value);
        }
    }
};
std::unique_ptr<LniObjectDocument> Parse(ObjectType type, const std::string& source) {
    auto parsed = LniObjectCodec(type).Parse(Bytes(source));
    if (!parsed.Succeeded()) return {};
    return std::unique_ptr<LniObjectDocument>(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
}
int FindRow(const ObjectFieldModel& model, const std::string& id, std::optional<std::uint32_t> level = {}) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto* modification = model.ModificationAt(row);
        if (modification && modification->key.fieldId == id && (!level || modification->key.level == level)) return row;
    }
    return -1;
}
std::string Serialize(const LniObjectDocument& document) {
    auto result = LniObjectCodec(document.Type()).Serialize(document);
    return result.Succeeded() ? std::string(reinterpret_cast<const char*>(result.value->data()), result.value->size()) : std::string{};
}
}

class ObjectFieldModelTests final : public QObject {
    Q_OBJECT
private slots:
    void EffectiveInheritedFieldsCreateOverlaysAndUndo();
    void TypedValidationPreservesUnknownFieldsAndText();
    void MultilineDescriptionCancelApplyAndUndo();
    void WarcraftPreviewDraftApplyCancelAndUnresolvedReference();
    void ColourPreviewUsesOwnedUtf8AndGbkWtsWithoutRawWrites();
    void ColourPreviewOverlayAndStaleSourcesRejectPreviousBody();
    void ColourPreviewPreparationAndResolverMayDeleteDelegateOwner();
    void LevelAndDataAddressesRemainDistinct();
    void OrderedReferenceEditorCommitsOneUndoableChange();
    void NumericEditingStartsAtValueAndSelectsText();
    void StringReferenceEditorsShowReferencedText();
    void RemovingOneOverlayPreservesSurroundingLines();
    void CustomParentChainMergesNearestValuesAndSources();
    void CustomParentResetAndEditsOnlyChangeSelectedObject();
    void InvalidParentChainsStayReadOnly();
    void InvalidInheritedValuesStayReadOnly();
    void NearParentValueShadowsFarParentAmbiguity();
};

void ObjectFieldModelTests::EffectiveInheritedFieldsCreateOverlaysAndUndo() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nunam=\"自定义步兵\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &object);
    const auto original = Serialize(*document);
    QVERIFY(model.Categories().contains(QStringLiteral("属性")));
    int health = FindRow(model, "uhpm");
    QVERIFY(health >= 0);
    QCOMPARE(model.index(health, 3).data().toString(), QStringLiteral("420"));
    QVERIFY(model.index(health, 0).data().toString().contains(QStringLiteral("生命值")));
    QVERIFY(!model.CanReset(health));
    QVERIFY(model.setData(model.index(health, 3), 560, Qt::EditRole));
    QCOMPARE(undo.count(), 1);
    QVERIFY(model.CanReset(FindRow(model, "uhpm")));
    QCOMPARE(fixture.catalog.Find(ObjectType::Unit, "hfoo")->Field("HP"), std::string_view("420"));
    undo.undo();
    QCOMPARE(Serialize(*document), original);
    undo.redo();
    QVERIFY(model.ResetField(FindRow(model, "uhpm")));
    QCOMPARE(model.index(FindRow(model, "uhpm"), 3).data().toString(), QStringLiteral("420"));
    undo.undo();
    QCOMPARE(model.index(FindRow(model, "uhpm"), 3).data().toString(), QStringLiteral("560"));
    model.SetFieldFilter(QStringLiteral("生命值"));
    QCOMPARE(model.rowCount(), 1);
    model.SetShowRawFields(true);
    QCOMPARE(model.index(0, 0).data().toString(), QStringLiteral("uhpm"));
    model.SetFieldFilter(QStringLiteral("uhpm"));
    QCOMPARE(model.rowCount(), 1);
    model.SetCategoryFilter(QStringLiteral("文本"));
    QCOMPARE(model.rowCount(), 0);
    model.SetCategoryFilter({});
    undo.clear();
}

void ObjectFieldModelTests::TypedValidationPreservesUnknownFieldsAndText() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nunam=\"步兵\"\nZZZZ=\"untouched\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &object);
    const auto original = Serialize(*document);
    QVERIFY(!model.index(FindRow(model, "ZZZZ"), 3).flags().testFlag(Qt::ItemIsEditable));
    int health = FindRow(model, "uhpm");
    QVERIFY(!model.setData(model.index(health, 3), QStringLiteral("4294967296"), Qt::EditRole));
    QVERIFY(!model.setData(model.index(health, 3), -1, Qt::EditRole));
    QVERIFY(!model.setData(model.index(FindRow(model, "umvs"), 3), QStringLiteral("nan"), Qt::EditRole));
    QCOMPARE(Serialize(*document), original);
    QCOMPARE(undo.count(), 0);
    QVERIFY(model.setData(model.index(FindRow(model, "ubdg"), 3), QStringLiteral("true"), Qt::EditRole));
    const auto* building = model.ModificationAt(FindRow(model, "ubdg"));
    QVERIFY(building && std::holds_alternative<std::int32_t>(building->value));
    QCOMPARE(std::get<std::int32_t>(building->value), 1);
    QVERIFY(std::get<bool>(*model.EditorValueAt(FindRow(model, "ubdg"))));
    QVERIFY(model.setData(model.index(FindRow(model, "unam"), 3), QStringLiteral("  保留空格  "), Qt::EditRole));
    QVERIFY(Serialize(*document).find("ZZZZ=\"untouched\"") != std::string::npos);
    QCOMPARE(model.index(FindRow(model, "unam"), 3).data().toString(), QStringLiteral("  保留空格  "));
    undo.clear();
}

void ObjectFieldModelTests::WarcraftPreviewDraftApplyCancelAndUnresolvedReference() {
    using War3Map::UiKit::WarcraftTextPreview;
    Fixture fixture; QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nUbertip=\"|cff00ff00原文|r|n第二行\"\nZZZZ=\"untouched\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    QUndoStack undo; ObjectFieldModel model;
    model.SetUndoStack(&undo); model.SetContext(&fixture.catalog, &fixture.schema); model.SetDocument(document.get(), &object);
    const int textRow = FindRow(model, "Ubertip");
    QVERIFY(textRow >= 0);
    const auto* textDefinition = model.DefinitionAt(textRow);
    QVERIFY(textDefinition);
    QCOMPARE(textDefinition->fieldId, std::string("utub"));
    QVERIFY(model.index(textRow, 3).flags().testFlag(Qt::ItemIsEditable));
    QCOMPARE(model.index(textRow, 3).data(Qt::EditRole).toString(), QStringLiteral("|cff00ff00原文|r|n第二行"));
    const auto original = Serialize(*document);
    QTableView view; view.setModel(&model); ObjectFieldDelegate delegate(&view); view.setItemDelegate(&delegate);
    view.resize(800, 500); view.show();
    const auto open = [&]() -> QFrame* {
        const auto index = model.index(FindRow(model, "Ubertip"), 3); if (!index.isValid()) return nullptr;
        QStyleOptionViewItem option; option.widget = view.viewport(); option.rect = view.visualRect(index);
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        if (!delegate.editorEvent(&event, &model, option, index)) return nullptr;
        return view.findChild<QFrame*>(QStringLiteral("objectMultilineEditor"));
    };
    auto* panel = open(); QVERIFY(panel);
    auto* text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText")); QVERIFY(text);
    auto* preview = dynamic_cast<WarcraftTextPreview*>(panel->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"))); QVERIFY(preview);
    QCOMPARE(preview->Preview()->toPlainText(), QStringLiteral("原文\n第二行"));
    auto cursor = text->textCursor(); cursor.movePosition(QTextCursor::End); text->setTextCursor(cursor);
    QVERIFY(preview->InsertColour(Qt::blue));
    QCOMPARE(undo.count(), 0); QCOMPARE(Serialize(*document), original);
    text->undo(); QCOMPARE(text->toPlainText(), QStringLiteral("|cff00ff00原文|r|n第二行"));
    QVERIFY(preview->InsertReset());
    QTest::mouseClick(panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(undo.count(), 0); QCOMPARE(Serialize(*document), original);
    panel = open(); QVERIFY(panel);
    text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText")); QVERIFY(text);
    preview = dynamic_cast<WarcraftTextPreview*>(panel->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"))); QVERIFY(preview);
    cursor = text->textCursor(); cursor.movePosition(QTextCursor::End); text->setTextCursor(cursor);
    QVERIFY(preview->InsertColour(Qt::red)); const auto expected = text->toPlainText();
    QTest::mouseClick(panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(undo.count(), 1); QCOMPARE(model.index(FindRow(model, "Ubertip"), 3).data(Qt::EditRole).toString(), expected);
    undo.undo(); QCOMPARE(Serialize(*document), original);
    undo.redo(); QCOMPARE(model.index(FindRow(model, "Ubertip"), 3).data(Qt::EditRole).toString(), expected);
    QVERIFY(Serialize(*document).find("ZZZZ=\"untouched\"") != std::string::npos);
    QVERIFY(model.setData(model.index(FindRow(model, "Ubertip"), 3), QStringLiteral("TRIGSTR_42"), Qt::EditRole));
    const auto referenced = Serialize(*document); const int count = undo.count();
    panel = open(); QVERIFY(panel);
    preview = dynamic_cast<WarcraftTextPreview*>(panel->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"))); QVERIFY(preview);
    QVERIFY(preview->StatusText().contains(QStringLiteral("未解析"))); QVERIFY(!preview->InsertColour(Qt::red));
    QCOMPARE(Serialize(*document), referenced); QCOMPARE(undo.count(), count);
}

void ObjectFieldModelTests::ColourPreviewUsesOwnedUtf8AndGbkWtsWithoutRawWrites() {
    using namespace War3Map;
    using namespace War3Map::UiKit;
    Fixture fixture; QVERIFY(fixture.valid);
    std::string wts = "STRING 42\n{\n|cff00ff00UTF中文|r|n第二行\n}\nSTRING 43\n{\n|cffff0000";
    wts.append("\xd6\xd0\xce\xc4", 4); wts += "|r\n}\n";
    const auto originalWts = Bytes(wts);
    const std::vector<MapArchiveMember> members{{"war3map.wts", originalWts}};
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    QVERIFY(project.mapDocument_->Strings());
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nUbertip=\"TRIGSTR_00042\"\nZZZZ=\"untouched\"\n");
    QVERIFY(document); const auto original = Serialize(*document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    QUndoStack undo; ObjectFieldModel model; int preparations = 0;
    model.SetUndoStack(&undo); model.SetContext(&fixture.catalog, &fixture.schema); model.SetDocument(document.get(), &object);
    const int textRow = FindRow(model, "Ubertip");
    QVERIFY(textRow >= 0);
    const auto* textDefinition = model.DefinitionAt(textRow);
    QVERIFY(textDefinition);
    QCOMPARE(textDefinition->fieldId, std::string("utub"));
    QVERIFY(model.index(textRow, 3).flags().testFlag(Qt::ItemIsEditable));
    QCOMPARE(model.index(textRow, 3).data(Qt::EditRole).toString(), QStringLiteral("TRIGSTR_00042"));
    model.SetColourPreviewFactory([&] { ++preparations; return Modules::CaptureObjectColourPreviewSource(project); });
    QTableView view; view.setModel(&model); ObjectFieldDelegate delegate(&view); view.setItemDelegate(&delegate);
    view.resize(800, 500); view.show();
    const auto open = [&]() -> QFrame* {
        const auto index = model.index(FindRow(model, "Ubertip"), 3); if (!index.isValid()) return nullptr;
        QStyleOptionViewItem option; option.widget = view.viewport(); option.rect = view.visualRect(index);
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        if (!delegate.editorEvent(&event, &model, option, index)) return nullptr;
        return view.findChild<QFrame*>(QStringLiteral("objectMultilineEditor"));
    };
    auto* panel = open(); QVERIFY(panel);
    auto* text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText")); QVERIFY(text);
    auto* preview = dynamic_cast<WarcraftTextPreview*>(panel->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"))); QVERIFY(preview);
    QCOMPARE(preparations, 1); QCOMPARE(text->toPlainText(), QStringLiteral("TRIGSTR_00042"));
    QCOMPARE(preview->Preview()->toPlainText(), QStringLiteral("UTF中文\n第二行"));
    QTextCursor green(preview->Preview()->document()); green.setPosition(0); green.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(green.charFormat().foreground().color(), QColor(0, 255, 0));
    text->setPlainText(QStringLiteral("TRIGSTR_43")); preview->Refresh();
    QCOMPARE(preview->Preview()->toPlainText(), QStringLiteral("中文"));
    QTextCursor red(preview->Preview()->document()); red.setPosition(0); red.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(red.charFormat().foreground().color(), QColor(255, 0, 0));
    text->setPlainText(QStringLiteral("\"TRIGSTR_00043\"")); preview->Refresh();
    QCOMPARE(preview->Preview()->toPlainText(), QStringLiteral("中文"));
    text->setPlainText(QStringLiteral("TRIGSTR_999")); preview->Refresh();
    QVERIFY(preview->StatusText().contains(QStringLiteral("没有可确认")));
    QCOMPARE(preparations, 1); QCOMPARE(Serialize(*document), original); QCOMPARE(undo.count(), 0);
    QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(project.mapDocument_->Member(MapDataKind::Strings).sources.front().data, originalWts);
    QTest::mouseClick(panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(Serialize(*document), original); QCOMPARE(undo.count(), 0);
    panel = open(); QVERIFY(panel); QCOMPARE(preparations, 2);
    text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText")); QVERIFY(text);
    text->setPlainText(QStringLiteral("TRIGSTR_43"));
    QTest::mouseClick(panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(undo.count(), 1); QCOMPARE(model.index(FindRow(model, "Ubertip"), 3).data(Qt::EditRole).toString(), QStringLiteral("TRIGSTR_43"));
    QVERIFY(!project.mapEdits_->IsDirty());
    undo.undo(); QCOMPARE(Serialize(*document), original);
    undo.redo(); QCOMPARE(model.index(FindRow(model, "Ubertip"), 3).data(Qt::EditRole).toString(), QStringLiteral("TRIGSTR_43"));
    QCOMPARE(project.mapDocument_->Member(MapDataKind::Strings).sources.front().data, originalWts);
    QVERIFY(Serialize(*document).find("ZZZZ=\"untouched\"") != std::string::npos);
}

void ObjectFieldModelTests::ColourPreviewOverlayAndStaleSourcesRejectPreviousBody() {
    using namespace War3Map;
    using namespace War3Map::UiKit;
    const std::vector<MapArchiveMember> members{{"war3map.wts", Bytes("STRING 42\n{\nbaseline\n}\n")}};
    auto project = std::make_unique<QtProject::EditorProject>();
    project->mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project->mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project->mapDocument_);
    auto overlay = *project->mapDocument_->Strings(); overlay.entries[0].value = "|cff00ff00当前覆盖|r|n第二行";
    QVERIFY(project->mapEdits_->EditStrings(overlay).Succeeded());
    auto resolver = Modules::CaptureObjectColourPreviewSource(*project);
    const auto current = resolver(QStringLiteral("TRIGSTR_42"));
    QCOMPARE(current.state, WarcraftTextSourceState::Available);
    QCOMPARE(current.text, QStringLiteral("|cff00ff00当前覆盖|r|n第二行"));
    QCOMPARE(project->mapDocument_->Strings()->entries[0].value, std::string("baseline"));
    QPlainTextEdit draft; draft.setPlainText(QStringLiteral("TRIGSTR_42")); WarcraftTextPreview widget;
    widget.Bind(&draft, {}, true, resolver);
    QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("当前覆盖\n第二行"));
    overlay.entries[0].value = "newer"; QVERIFY(project->mapEdits_->EditStrings(overlay).Succeeded());
    widget.Refresh(); QVERIFY(widget.StatusText().contains(QStringLiteral("已失效")));
    QCOMPARE(resolver(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Stale);
    QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("当前覆盖\n第二行"));
    QVERIFY(widget.StatusText().contains(QStringLiteral("未代表当前")));
    auto fresh = Modules::CaptureObjectColourPreviewSource(*project);
    QCOMPARE(fresh(QStringLiteral("TRIGSTR_42")).text, QStringLiteral("newer"));
    project->sourceChangeSuspected_ = true;
    QCOMPARE(fresh(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Stale);
    project->sourceChangeSuspected_ = false;
    QCOMPARE(fresh(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Stale);
    Modules::ObjectColourPreviewLimits limits; limits.maxSourceBytes = 1;
    auto bounded = Modules::CaptureObjectColourPreviewSource(*project, limits);
    QCOMPARE(bounded(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Unknown);
    QCOMPARE(bounded(QStringLiteral("literal")).state, WarcraftTextSourceState::Available);
    const std::vector<MapArchiveMember> duplicateMembers{{"war3map.wts", Bytes("STRING 42\n{\none\n}\nSTRING 42\n{\ntwo\n}\n")}};
    auto duplicate = std::make_shared<const MapDocument>(MapDocument::FromMembers(duplicateMembers));
    auto duplicateResolver = Modules::PrepareObjectColourPreviewSource(duplicate, [] { return true; });
    const auto ambiguous = duplicateResolver(QStringLiteral("TRIGSTR_00042"));
    QCOMPARE(ambiguous.state, WarcraftTextSourceState::Unknown); QVERIFY(ambiguous.explanation.contains(QStringLiteral("重复")));
    QCOMPARE(duplicateResolver(QStringLiteral("TRIGSTR_" )).state, WarcraftTextSourceState::Unknown);
    limits = {}; limits.maxRecords = 1;
    auto recordBounded = Modules::PrepareObjectColourPreviewSource(duplicate, [] { return true; }, limits);
    QCOMPARE(recordBounded(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Unknown);
    const std::vector<MapArchiveMember> emptyMembers{{"war3map.wts", Bytes("STRING 42\n{\n\n}\n")}};
    auto emptySource = std::make_shared<const MapDocument>(MapDocument::FromMembers(emptyMembers));
    auto emptyResolver = Modules::PrepareObjectColourPreviewSource(emptySource, [] { return true; });
    const auto empty = emptyResolver(QStringLiteral("TRIGSTR_42"));
    QCOMPARE(empty.state, WarcraftTextSourceState::Available); QVERIFY(empty.text.isEmpty());
    limits = {}; limits.maxBodyBytes = 1;
    auto bodySource = std::make_shared<const MapDocument>(MapDocument::FromMembers(members));
    auto bodyBounded = Modules::PrepareObjectColourPreviewSource(bodySource, [] { return true; }, limits);
    const auto tooLong = bodyBounded(QStringLiteral("TRIGSTR_42"));
    QCOMPARE(tooLong.state, WarcraftTextSourceState::Unknown);
    QVERIFY(tooLong.explanation.contains(QStringLiteral("单项")));
    project.reset(); QCOMPARE(fresh(QStringLiteral("TRIGSTR_42")).state, WarcraftTextSourceState::Stale);
    QCOMPARE(draft.toPlainText(), QStringLiteral("TRIGSTR_42"));
}

void ObjectFieldModelTests::ColourPreviewPreparationAndResolverMayDeleteDelegateOwner() {
    using namespace War3Map::UiKit;
    for (const bool deleteInResolver : {false, true}) {
        Fixture fixture; QVERIFY(fixture.valid);
        auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nUbertip=\"TRIGSTR_42\"\nZZZZ=\"untouched\"\n");
        QVERIFY(document); const auto original = Serialize(*document);
        const ObjectKey object{ObjectType::Unit, "u000"};
        QUndoStack undo; ObjectFieldModel model;
        model.SetUndoStack(&undo); model.SetContext(&fixture.catalog, &fixture.schema); model.SetDocument(document.get(), &object);
        const int textRow = FindRow(model, "Ubertip");
        QVERIFY(textRow >= 0);
        const auto* textDefinition = model.DefinitionAt(textRow);
        QVERIFY(textDefinition);
        QCOMPARE(textDefinition->fieldId, std::string("utub"));
        QVERIFY(model.index(textRow, 3).flags().testFlag(Qt::ItemIsEditable));
        QCOMPARE(model.index(textRow, 3).data(Qt::EditRole).toString(), QStringLiteral("TRIGSTR_42"));
        QTableView view; view.setModel(&model); view.resize(800, 500); view.show();
        QPointer<ObjectFieldDelegate> delegate = new ObjectFieldDelegate(&view);
        int preparations = 0, resolutions = 0;
        model.SetColourPreviewFactory([&]() -> WarcraftTextSourceResolver {
            ++preparations;
            if (!deleteInResolver) { delete delegate.data(); return {}; }
            return [&](const QString&) {
                ++resolutions; delete delegate.data();
                return WarcraftTextSource{QStringLiteral("不会发布的旧正文"), {}, WarcraftTextSourceState::Available};
            };
        });
        const auto index = model.index(FindRow(model, "Ubertip"), 3); QVERIFY(index.isValid());
        QStyleOptionViewItem option; option.widget = view.viewport(); option.rect = view.visualRect(index);
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        const bool published = delegate->editorEvent(&event, &model, option, index);
        QVERIFY(!published); QVERIFY(delegate.isNull()); QCOMPARE(preparations, 1);
        QCOMPARE(resolutions, deleteInResolver ? 1 : 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!view.findChild<QFrame*>(QStringLiteral("objectMultilineEditor")));
        QCOMPARE(Serialize(*document), original); QCOMPARE(undo.count(), 0);
        QCOMPARE(model.index(FindRow(model, "Ubertip"), 3).data(Qt::EditRole).toString(), QStringLiteral("TRIGSTR_42"));
        QVERIFY(view.isVisible());
    }
}

void ObjectFieldModelTests::MultilineDescriptionCancelApplyAndUndo() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nZZZZ=\"untouched\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &object);
    const int row = FindRow(model, "utub");
    QVERIFY(row >= 0);
    QCOMPARE(model.index(row, 3).data().toString(), QStringLiteral("原版说明"));
    const std::string original = Serialize(*document);

    QTableView view;
    view.setModel(&model);
    ObjectFieldDelegate delegate(&view);
    view.setItemDelegate(&delegate);
    view.resize(800, 500);
    view.show();
    const auto openEditor = [&]() -> QFrame* {
        const QModelIndex index = model.index(FindRow(model, "utub"), 3);
        QStyleOptionViewItem option;
        option.widget = view.viewport();
        option.rect = view.visualRect(index);
        QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        if (!delegate.editorEvent(&open, &model, option, index)) return nullptr;
        return view.findChild<QFrame*>(QStringLiteral("objectMultilineEditor"));
    };

    auto* panel = openEditor();
    QVERIFY(panel);
    QCOMPARE(panel->windowModality(), Qt::NonModal);
    auto* text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText"));
    QVERIFY(text);
    QCOMPARE(text->toPlainText(), QStringLiteral("原版说明"));
    text->setPlainText(QStringLiteral("取消时不应写入\n第二行"));
    auto* actions = panel->findChild<QDialogButtonBox*>();
    QVERIFY(actions);
    QTest::mouseClick(actions->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCOMPARE(undo.count(), 0);
    QCOMPARE(Serialize(*document), original);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    panel = openEditor();
    QVERIFY(panel);
    text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText"));
    QVERIFY(text);
    const QString edited = QStringLiteral("第一行\n第二行\n");
    actions = panel->findChild<QDialogButtonBox*>();
    QVERIFY(actions);
    text->setPlainText(edited);
    QTest::mouseClick(actions->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCOMPARE(undo.count(), 1);
    QCOMPARE(model.index(FindRow(model, "utub"), 3).data(Qt::EditRole).toString(), edited);
    QVERIFY(model.index(FindRow(model, "utub"), 3).data(Qt::DisplayRole).toString().contains(QChar(0x21b5)));
    const auto* modification = model.ModificationAt(FindRow(model, "utub"));
    QVERIFY(modification && std::holds_alternative<std::string>(modification->value));
    QCOMPARE(std::get<std::string>(modification->value), edited.toStdString());
    const std::string serialized = Serialize(*document);
    QVERIFY(serialized.find("ZZZZ=\"untouched\"") != std::string::npos);
    QVERIFY(serialized.find("第一行") != std::string::npos);
    QVERIFY(serialized.find("第二行") != std::string::npos);
    auto reparsed = Parse(ObjectType::Unit, serialized);
    QVERIFY(reparsed && reparsed->IsStructurallySafe());
    QCOMPARE(Serialize(*reparsed), serialized);
    ObjectFieldModel reopened;
    reopened.SetContext(&fixture.catalog, &fixture.schema);
    reopened.SetDocument(reparsed.get(), &object);
    const int reopenedRow = FindRow(reopened, "utub");
    QVERIFY(reopenedRow >= 0);
    QVERIFY(reopened.index(reopenedRow, 3).flags().testFlag(Qt::ItemIsEditable));
    QCOMPARE(reopened.index(reopenedRow, 3).data(Qt::EditRole).toString(), edited);
    QVERIFY(reopened.setData(reopened.index(reopenedRow, 3), QStringLiteral("再次修改\n仍可编辑\n"), Qt::EditRole));
    undo.undo();
    QCOMPARE(Serialize(*document), original);
    undo.redo();
    QCOMPARE(model.index(FindRow(model, "utub"), 3).data(Qt::EditRole).toString(), edited);
    undo.clear();
}

void ObjectFieldModelTests::LevelAndDataAddressesRemainDistinct() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Ability, "[A000]\n_parent=\"AHbz\"\nHbz1[1,1]=99.5\nZZZZ[2,9]=\"keep\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Ability, "A000"};
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &object);
    const auto original = Serialize(*document);
    int level2 = FindRow(model, "Hbz1", 2);
    QVERIFY(level2 >= 0);
    QCOMPARE(model.ModificationAt(level2)->key.dataIndex, std::optional<std::uint32_t>{1});
    QVERIFY(model.setData(model.index(level2, 3), QStringLiteral("125.25"), Qt::EditRole));
    QCOMPARE(model.index(FindRow(model, "Hbz1", 1), 3).data().toString(), QStringLiteral("99.5"));
    QVERIFY(Serialize(*document).find("Hbz1[2,1] = 125.25") != std::string::npos);
    QCOMPARE(model.index(FindRow(model, "atp1", 1), 3).data().toString(), QStringLiteral("第一等级,含逗号"));
    QCOMPARE(model.index(FindRow(model, "atp1", 2), 3).data().toString(), QStringLiteral("第二等级"));
    QVERIFY(model.ResetField(FindRow(model, "Hbz1", 1)));
    QVERIFY(Serialize(*document).find("ZZZZ[2,9]=\"keep\"") != std::string::npos);
    undo.undo();
    undo.undo();
    QCOMPARE(Serialize(*document), original);
    undo.clear();
}

void ObjectFieldModelTests::OrderedReferenceEditorCommitsOneUndoableChange() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\nuabi=\"A001,LOST,A001\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &object);
    QTableView view;
    view.setModel(&model);
    ObjectFieldDelegate delegate(&view);
    delegate.SetReferenceContext(nullptr, {
        {QStringLiteral("ability"), QStringLiteral("A001"), QStringLiteral("技能甲")},
        {QStringLiteral("ability"), QStringLiteral("A002"), QStringLiteral("技能乙")},
        {QStringLiteral("unit"), QStringLiteral("hfoo"), QStringLiteral("步兵")}
    });
    view.setItemDelegate(&delegate);
    view.resize(900, 600);
    view.show();
    const auto index = model.index(FindRow(model, "uabi"), 3);
    QStyleOptionViewItem option;
    option.widget = view.viewport();
    option.rect = view.visualRect(index);
    QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QVERIFY(delegate.editorEvent(&open, &model, option, index));
    auto* panel = view.findChild<QFrame*>(QStringLiteral("objectReferenceEditor"));
    QVERIFY(panel);
    QCOMPARE(panel->windowModality(), Qt::NonModal);
    auto* ordered = panel->findChild<War3Map::QtApp::OrderedObjectListWidget*>();
    QVERIFY(ordered);
    QCOMPARE(ordered->Ids(), QStringList({"A001", "LOST", "A001"}));
    ordered->SetIds({"A001", "A002", "LOST", "A001"});
    auto* actions = panel->findChild<QDialogButtonBox*>();
    QVERIFY(actions);
    QTest::mouseClick(actions->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCOMPARE(undo.count(), 1);
    QCOMPARE(model.index(FindRow(model, "uabi"), 3).data().toString(), QStringLiteral("A001,A002,LOST,A001"));
    undo.undo();
    QCOMPARE(model.index(FindRow(model, "uabi"), 3).data().toString(), QStringLiteral("A001,LOST,A001"));
    undo.clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    option.rect = view.visualRect(model.index(FindRow(model, "uabi"), 3));
    const auto reopen = [&]() {
        const auto current = model.index(FindRow(model, "uabi"), 3); view.setCurrentIndex(current);
        delegate.editorEvent(&open, &model, option, current);
        return view.findChild<QFrame*>(QStringLiteral("objectReferenceEditor"));
    };
    panel = reopen(); QVERIFY(panel); QTest::keyClick(panel, Qt::Key_Escape); QVERIFY(!panel->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel = reopen(); QVERIFY(panel);
    view.setCurrentIndex(model.index(FindRow(model, "uhpm"), 3)); QVERIFY(!panel->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel = reopen(); QVERIFY(panel);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(2, 2)); QVERIFY(!panel->isVisible());
}

void ObjectFieldModelTests::NumericEditingStartsAtValueAndSelectsText() {
    Fixture fixture; QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit, "[u000]\n_parent=\"hfoo\"\n"); QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model; model.SetContext(&fixture.catalog, &fixture.schema); model.SetDocument(document.get(), &object);
    QTableView view; view.setModel(&model); ObjectFieldDelegate delegate(&view); view.setItemDelegate(&delegate);
    view.resize(900, 600); view.show(); view.setFocus();
    const auto row = FindRow(model, "uhpm"); QVERIFY(row >= 0);
    view.setCurrentIndex(model.index(row, 0)); QTest::keyClick(&view, Qt::Key_F2);
    auto* spin = view.findChild<QSpinBox*>(); QVERIFY(spin); QCOMPARE(spin->value(), 420);
    QTest::keyClick(spin, Qt::Key_4); QCOMPARE(spin->value(), 4);
    QTest::keyClick(spin, Qt::Key_Return);
    // Qt 的 Return 提交通过排队回调发出，既核对输入替换，也等待实际模型提交。
    QTRY_COMPARE(std::get<std::int32_t>(*model.EditorValueAt(FindRow(model, "uhpm"))), 4);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    view.setCurrentIndex(model.index(FindRow(model, "uhpm"), 0)); QTest::keyClick(&view, Qt::Key_7);
    spin = view.findChild<QSpinBox*>(); QVERIFY(spin); QCOMPARE(spin->value(), 7);
    QTest::keyClick(spin, Qt::Key_Escape);
    QCOMPARE(std::get<std::int32_t>(*model.EditorValueAt(FindRow(model, "uhpm"))), 4);
    model.SetValuePresentationResolver([](const ObjectFieldDefinition*, const QString& raw) {
        return raw == QStringLiteral("A001,A002") ? QStringLiteral("技能甲、技能乙") : raw;
    });
    auto abilities = model.index(FindRow(model, "uabi"), 3);
    QCOMPARE(abilities.data().toString(), QStringLiteral("技能甲、技能乙"));
    QCOMPARE(abilities.data(Qt::EditRole).toString(), QStringLiteral("A001,A002"));
    model.SetShowRawFields(true);
    QCOMPARE(model.index(FindRow(model, "uabi"), 3).data().toString(), QStringLiteral("A001,A002"));
}

void ObjectFieldModelTests::StringReferenceEditorsShowReferencedText() {
    Fixture fixture; QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit,
        "[u000]\n_parent=\"hfoo\"\nunam=\"TRIGSTR_007\"\nUbertip=\"TRIGSTR_008\"\nZZZZ=\"untouched\"\n");
    QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u000"};
    ObjectFieldModel model; QUndoStack undo; model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema); model.SetDocument(document.get(), &object);
    model.SetStringReferenceResolver([](std::string_view raw) -> std::optional<std::string> {
        if (raw == "TRIGSTR_007") return std::string("圣光突袭(Q)");
        if (raw == "TRIGSTR_008") return std::string("|cffffcc00第一行|r|n第二行");
        return std::nullopt;
    });
    QTableView view; view.setModel(&model); ObjectFieldDelegate delegate(&view); view.setItemDelegate(&delegate);
    view.resize(900, 600); view.show(); view.setFocus();
    const auto value = [&](const char* id) { return model.index(FindRow(model, id), 3).data(Qt::EditRole).toString(); };

    // 单行：编辑框是引用的正文；原样提交保留引用，改动后写入新文字。
    view.setCurrentIndex(model.index(FindRow(model, "unam"), 3)); QTest::keyClick(&view, Qt::Key_F2);
    auto* edit = view.findChild<QLineEdit*>(); QVERIFY(edit);
    QCOMPARE(edit->text(), QStringLiteral("圣光突袭(Q)"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(value("unam"), QStringLiteral("TRIGSTR_007"));
    QCOMPARE(undo.count(), 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    view.setCurrentIndex(model.index(FindRow(model, "unam"), 3)); QTest::keyClick(&view, Qt::Key_F2);
    edit = view.findChild<QLineEdit*>(); QVERIFY(edit);
    edit->setText(QStringLiteral("圣光突袭(W)")); QTest::keyClick(edit, Qt::Key_Return);
    QTRY_COMPARE(value("unam"), QStringLiteral("圣光突袭(W)"));
    QCOMPARE(undo.count(), 1);

    // 多行：面板里是引用的原始正文（保留颜色码）；未改动直接应用不写入。
    const QModelIndex tip = model.index(FindRow(model, "Ubertip"), 3);
    QStyleOptionViewItem option; option.widget = view.viewport(); option.rect = view.visualRect(tip);
    QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QVERIFY(delegate.editorEvent(&open, &model, option, tip));
    auto* panel = view.findChild<QFrame*>(QStringLiteral("objectMultilineEditor")); QVERIFY(panel);
    auto* text = panel->findChild<QPlainTextEdit*>(QStringLiteral("objectMultilineText")); QVERIFY(text);
    QCOMPARE(text->toPlainText(), QStringLiteral("|cffffcc00第一行|r|n第二行"));
    QTest::mouseClick(panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCOMPARE(value("Ubertip"), QStringLiteral("TRIGSTR_008"));
    QCOMPARE(undo.count(), 1);
}

void ObjectFieldModelTests::RemovingOneOverlayPreservesSurroundingLines() {
    auto document = Parse(ObjectType::Unit, "[u000]\r\n_parent=\"hfoo\"\r\n; comment\r\nuhpm=900 ; hp\r\nZZZZ=\"opaque-id\"\r\n[u001]\r\n_parent=\"hfoo\"\r\nunam=\"Other\"\r\n");
    QVERIFY(document);
    QVERIFY(document->RemoveModification({ObjectType::Unit, "u000"}, {"uhpm"}).Succeeded());
    QVERIFY(document->SetModification({ObjectType::Unit, "u001"}, {"unam"}, std::string("Next")).Succeeded());
    const auto result = Serialize(*document);
    QVERIFY(result.find("; comment\r\nZZZZ=\"opaque-id\"\r\n") != std::string::npos);
    QVERIFY(result.find("uhpm") == std::string::npos);
    QVERIFY(result.find("unam=\"Next\"\r\n") != std::string::npos);
    auto reparsed = Parse(ObjectType::Unit, result);
    QVERIFY(reparsed);
    QCOMPARE(reparsed->Objects().size(), std::size_t{2});
}

void ObjectFieldModelTests::CustomParentChainMergesNearestValuesAndSources() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit,
        "[hfoo]\n_parent=\"hfoo\"\nuhpm=500\nunam=\"Root\"\n"
        "[H001]\n_parent=\"hfoo\"\nHP=600\nunam=\"Parent\"\nuabi=\"A002,LOST,A002\"\nZZZZ=\"unknown-parent\"\n"
        "[H002]\n_parent=\"H001\"\nuhpm=700\n");
    QVERIFY(document);
    const ObjectKey child{ObjectType::Unit, "H002"};
    const auto original = Serialize(*document);
    const auto chain = ResolveObjectInheritance(*document, child, &fixture.catalog);
    QVERIFY(chain.IsComplete());
    QCOMPARE(chain.records.size(), std::size_t{3});
    QCOMPARE(chain.records[0]->key.rawcode, std::string("H002"));
    QCOMPARE(chain.records[1]->key.rawcode, std::string("H001"));
    QCOMPARE(chain.records[2]->key.rawcode, std::string("hfoo"));
    QVERIFY(chain.standardBase);
    QCOMPARE(chain.standardBase->key.rawcode, std::string("hfoo"));
    ObjectFieldModel model;
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &child);
    QVERIFY(model.InheritanceIssue().isEmpty());
    const int health = FindRow(model, "uhpm");
    QVERIFY(health >= 0);
    QCOMPARE(model.index(health, 3).data().toString(), QStringLiteral("700"));
    QCOMPARE(model.index(health, 2).data().toString(), QStringLiteral("600"));
    const QString tooltip = model.index(health, 3).data(Qt::ToolTipRole).toString();
    QVERIFY(tooltip.contains(QStringLiteral("当前来源: 地图对象 H002")));
    QVERIFY(tooltip.contains(QStringLiteral("继承来源: 地图对象 H001")));
    QCOMPARE(model.index(FindRow(model, "unam"), 3).data().toString(), QStringLiteral("Parent"));
    QCOMPARE(model.index(FindRow(model, "uabi"), 3).data().toString(), QStringLiteral("A002,LOST,A002"));
    QCOMPARE(model.index(FindRow(model, "umvs"), 3).data().toString(), QStringLiteral("270.125"));
    QVERIFY(!model.CanReset(FindRow(model, "unam")));
    QVERIFY(!model.index(FindRow(model, "ZZZZ"), 3).flags().testFlag(Qt::ItemIsEditable));
    QCOMPARE(Serialize(*document), original);
    QCOMPARE(fixture.catalog.Find(ObjectType::Unit, "hfoo")->Field("HP"), std::string_view("420"));
}

void ObjectFieldModelTests::CustomParentResetAndEditsOnlyChangeSelectedObject() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit,
        "[H001]\n_parent=\"hfoo\"\nHP=610\nspd=290.25\nunam=\"Parent\"\n"
        "[H002]\n_parent=\"H001\"\nuhpm=720\nZZZZ=\"child-unknown\"\n");
    QVERIFY(document);
    const ObjectKey child{ObjectType::Unit, "H002"};
    const auto original = Serialize(*document);
    const auto parentBytes = original.substr(0, original.find("[H002]"));
    ObjectFieldModel model;
    QUndoStack undo;
    model.SetUndoStack(&undo);
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &child);
    QVERIFY(model.ResetField(FindRow(model, "uhpm")));
    QCOMPARE(model.index(FindRow(model, "uhpm"), 3).data().toString(), QStringLiteral("610"));
    QVERIFY(!model.CanReset(FindRow(model, "uhpm")));
    QCOMPARE(document->FindObject(child)->modifications.size(), std::size_t{1});
    QVERIFY(model.setData(model.index(FindRow(model, "umvs"), 3), QStringLiteral("305.5"), Qt::EditRole));
    const auto* changedChild = document->FindObject(child);
    QVERIFY(changedChild);
    const auto speed = std::find_if(changedChild->modifications.begin(), changedChild->modifications.end(), [](const ObjectModification& field) {
        return field.key.fieldId == "umvs";
    });
    QVERIFY(speed != changedChild->modifications.end());
    QVERIFY(std::holds_alternative<float>(speed->value));
    QCOMPARE(std::get<float>(speed->value), 305.5F);
    QCOMPARE(Serialize(*document).substr(0, Serialize(*document).find("[H002]")), parentBytes);
    QVERIFY(model.ResetField(FindRow(model, "umvs")));
    QCOMPARE(model.index(FindRow(model, "umvs"), 3).data().toString(), QStringLiteral("290.25"));
    QCOMPARE(undo.count(), 3);
    undo.undo();
    undo.undo();
    undo.undo();
    QCOMPARE(Serialize(*document), original);
    undo.clear();
}

void ObjectFieldModelTests::InvalidParentChainsStayReadOnly() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    const std::vector<std::pair<std::string, QString>> cases{
        {"[H001]\n_parent=\"H002\"\nuhpm=600\n[H002]\n_parent=\"H001\"\nuhpm=700\n", QStringLiteral("循环")},
        {"[H002]\n_parent=\"H002\"\nuhpm=700\n", QStringLiteral("循环")},
        {"[H001]\n_parent=\"hfoo\"\nuhpm=600\n[H002]\n_parent=\"h001\"\nuhpm=700\n", QStringLiteral("缺少父对象：h001")},
        // AHbz 仅存在于技能目录，不允许用相同 rawcode 跨类型完成单位继承。
        {"[H002]\n_parent=\"AHbz\"\nuhpm=700\n", QStringLiteral("缺少父对象：AHbz")}
    };
    for (const auto& [source, issue] : cases) {
        auto document = Parse(ObjectType::Unit, source);
        QVERIFY(document);
        const ObjectKey child{ObjectType::Unit, "H002"};
        const auto original = Serialize(*document);
        ObjectFieldModel model;
        QUndoStack undo;
        model.SetUndoStack(&undo);
        model.SetContext(&fixture.catalog, &fixture.schema);
        model.SetDocument(document.get(), &child);
        QVERIFY2(model.InheritanceIssue().contains(issue), qPrintable(model.InheritanceIssue()));
        const int health = FindRow(model, "uhpm");
        QVERIFY(health >= 0);
        QVERIFY(!model.index(health, 3).flags().testFlag(Qt::ItemIsEditable));
        QVERIFY(model.index(health, 3).data(Qt::ToolTipRole).toString().contains(issue));
        QVERIFY(!model.setData(model.index(health, 3), 900, Qt::EditRole));
        QVERIFY(!model.ResetField(health));
        QCOMPARE(undo.count(), 0);
        QCOMPARE(Serialize(*document), original);
        const auto mismatchedType = ResolveObjectInheritance(*document, {ObjectType::Ability, "H002"}, &fixture.catalog);
        QVERIFY(!mismatchedType.IsComplete());
        QVERIFY(mismatchedType.records.empty());
    }
}

void ObjectFieldModelTests::InvalidInheritedValuesStayReadOnly() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    for (const std::string childFields : {std::string{}, std::string{"uhpm=700\n"}}) {
        auto document = Parse(ObjectType::Unit,
            "[H001]\n_parent=\"hfoo\"\nuhpm=\"invalid-number\"\n"
            "[H002]\n_parent=\"H001\"\n" + childFields);
        QVERIFY(document);
        const ObjectKey child{ObjectType::Unit, "H002"};
        const auto original = Serialize(*document);
        ObjectFieldModel model;
        model.SetContext(&fixture.catalog, &fixture.schema);
        model.SetDocument(document.get(), &child);
        const int health = FindRow(model, "uhpm");
        QVERIFY(health >= 0);
        QVERIFY(model.InheritanceIssue().isEmpty());
        QVERIFY(!model.index(health, 3).flags().testFlag(Qt::ItemIsEditable));
        QVERIFY(model.index(health, 3).data(Qt::ToolTipRole).toString().contains(QStringLiteral("类型解析")));
        QVERIFY(!model.setData(model.index(health, 3), 900, Qt::EditRole));
        QVERIFY(!model.ResetField(health));
        QCOMPARE(Serialize(*document), original);
    }
}

void ObjectFieldModelTests::NearParentValueShadowsFarParentAmbiguity() {
    Fixture fixture;
    QVERIFY(fixture.valid);
    auto document = Parse(ObjectType::Unit,
        "[H000]\n_parent=\"hfoo\"\nHP=500\nuhpm=550\n"
        "[H001]\n_parent=\"H000\"\nuhpm=600\n"
        "[H002]\n_parent=\"H001\"\nuhpm=700\n");
    QVERIFY(document);
    const ObjectKey child{ObjectType::Unit, "H002"};
    ObjectFieldModel model;
    model.SetContext(&fixture.catalog, &fixture.schema);
    model.SetDocument(document.get(), &child);
    const int health = FindRow(model, "uhpm");
    QVERIFY(health >= 0);
    QVERIFY(model.index(health, 3).flags().testFlag(Qt::ItemIsEditable));
    QCOMPARE(model.index(health, 2).data().toString(), QStringLiteral("600"));
    QVERIFY(model.ResetField(health));
    QCOMPARE(model.index(FindRow(model, "uhpm"), 3).data().toString(), QStringLiteral("600"));
    const auto source = Serialize(*document);
    QVERIFY(source.find("HP=500\nuhpm=550\n") != std::string::npos);
    QVERIFY(source.find("[H001]\n_parent=\"H000\"\nuhpm=600\n") != std::string::npos);
}

QTEST_MAIN(ObjectFieldModelTests)
#include "ObjectFieldModelTests.moc"
