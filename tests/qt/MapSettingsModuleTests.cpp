#include "MapSettingsModule.h"
#include "MapPlayerEnumDelegate.h"
#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Widgets/WarcraftTextPreview.h"
#include "MapDataFixtures.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTest>
#include <QTextEdit>
#include <QUndoStack>

#include <algorithm>
#include <memory>

using namespace War3Map;

namespace {

void OpenSyntheticMap(QtProject::EditorProject& project,
    std::vector<std::byte> bytes = MapDataFixtures::FullMapSettingsV25().bytes) {
    const std::vector<MapArchiveMember> members{{"war3map.w3i", std::move(bytes)}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    emit project.MapDocumentChanged();
}

std::vector<std::byte> SettingsBytesWithDataSet(std::int32_t dataSet) {
    auto parsed = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    if (!parsed.Succeeded() || !parsed.value) return {};
    parsed.value->gameDataSet = dataSet;
    auto serialized = parsed.value->Serialize();
    return serialized.Succeeded() && serialized.value ? std::move(*serialized.value) : std::vector<std::byte>{};
}

std::vector<MapArchiveMember> ReferencedSettingsMembers(bool includeStart = false) {
    auto parsed = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    if (!parsed.Succeeded() || !parsed.value) return {};
    parsed.value->author = "TRIGSTR_00042"; parsed.value->recommendedPlayers = "TRIGSTR_7";
    const auto settings = parsed.value->Serialize();
    if (!settings.Succeeded() || !settings.value) return {};
    const std::string strings = "// keep\r\nSTRING 42\r\n{\r\n原作者与描述\r\n}\r\nSTRING 7\r\n{\r\n2–4\r\n}\r\n";
    std::vector<MapArchiveMember> members{{"war3map.w3i", *settings.value}, {"war3map.wts",
        {reinterpret_cast<const std::byte*>(strings.data()), reinterpret_cast<const std::byte*>(strings.data() + strings.size())}}};
    if (includeStart) {
        MapData::UnitPlacements units;
        MapData::UnitPlacement start; start.rawcode = "sloc"; start.creationId = 77; start.scale = {1, 1, 1};
        start.position = {parsed.value->players[0].startX, parsed.value->players[0].startY, 16};
        units.units.push_back(start);
        const auto bytes = units.Serialize(); if (!bytes.Succeeded() || !bytes.value) return {};
        members.push_back({"war3mapUnits.doo", *bytes.value});
    }
    return members;
}

void OpenMembers(QtProject::EditorProject& project, std::span<const MapArchiveMember> members) {
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    emit project.MapDocumentChanged();
}

} // namespace

class MapSettingsModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void DraftCancelAndInvalidInputDoNotModifyDocument();
    void PlayerControllerAndRaceUseNamedPickers();
    void DraftSurvivesUnrelatedEditsAndBlocksAfterSettingsChange();
    void ApplyUndoRedoPreservesUneditedBytesAndOtherFields();
    void UnknownDataSetIsPreservedUntilExplicitlyChanged();
    void RawLabelsAndDocumentReplacement();
    void ClassicFlagsAreEditableAndUnknownBitsSurviveUndo();
    void PlayerStartEditsUndoTheirLinkedPlacement();
    void NarrowLayoutKeepsDraftNavigationAndActions();
    void NewFormatIsVisibleButCannotEnterClassicSavePath();
    void ReferencedTextPreservesWireAndRefreshesTheCurrentSharedBody();
    void ReferencedTextAndPlayerStartShareOneUndoCommand();
    void ReferencedTextRejectsConflictsUnsafeAndChangedSources();
    void NavigationRestoresSectionAndPreservesUnappliedDraft();
    void WarcraftPreviewUsesCurrentWtsAndExistingApplyUndo();
};

void MapSettingsModuleTests::WarcraftPreviewUsesCurrentWtsAndExistingApplyUndo() {
    using UiKit::WarcraftTextPreview;
    auto members = ReferencedSettingsMembers(); QVERIFY(!members.empty());
    const std::string strings = "STRING 42\r\n{\r\n|cff00ff00原作者|r|n与描述\r\n}\r\nSTRING 7\r\n{\r\n2–4\r\n}\r\n";
    members[1].data.assign(reinterpret_cast<const std::byte*>(strings.data()), reinterpret_cast<const std::byte*>(strings.data() + strings.size()));
    QtProject::EditorProject project; OpenMembers(project, members);
    Modules::MapSettingsModule module(project); auto* page = module.Widget();
    const auto getAuthor = [&] { return page->findChild<QLineEdit*>(QStringLiteral("mapSettings_author")); };
    const auto getPreview = [&]() -> WarcraftTextPreview* {
        const auto* author = getAuthor();
        return author ? dynamic_cast<WarcraftTextPreview*>(author->parentWidget()->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"))) : nullptr;
    };
    auto* author = getAuthor(); auto* preview = getPreview(); QVERIFY(author && preview);
    QCOMPARE(preview->Preview()->toPlainText(), QStringLiteral("原作者\n与描述"));
    // 一页多个字段：含颜色/换行标记时显示渲染框，纯文字时收起，插入标记后再出现。
    QVERIFY(!preview->Preview()->isHidden());
    author->setText(QStringLiteral("纯文字作者")); preview->Refresh();
    QVERIFY(preview->Preview()->isHidden());
    author->setCursorPosition(static_cast<int>(author->text().size())); QVERIFY(preview->InsertNewline());
    QVERIFY(!preview->Preview()->isHidden());
    module.DiscardDraft();
    author = getAuthor(); preview = getPreview(); QVERIFY(author && preview);
    const auto originalSettings = project.MapEdits()->Snapshot().Settings()->Serialize().value;
    const auto originalStrings = project.MapEdits()->Snapshot().Member(MapDataKind::Strings).sources.front().data;
    author->setCursorPosition(static_cast<int>(author->text().size())); QVERIFY(preview->InsertColour(Qt::red));
    QVERIFY(module.HasPendingDraft()); QVERIFY(!project.MapEdits()->IsDirty()); QCOMPARE(module.UndoStack()->count(), 0);
    module.DiscardDraft(); QVERIFY(!project.MapEdits()->IsDirty());
    QCOMPARE(project.MapEdits()->Snapshot().Member(MapDataKind::Strings).sources.front().data, originalStrings);
    author = getAuthor(); preview = getPreview(); QVERIFY(author && preview);
    author->setCursorPosition(static_cast<int>(author->text().size())); QVERIFY(preview->InsertColour(Qt::blue));
    const auto body = author->text().toUtf8().toStdString();
    QVERIFY(module.ApplyDraft()); QCOMPARE(module.UndoStack()->count(), 1);
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->author, std::string("TRIGSTR_00042"));
    QCOMPARE(project.MapEdits()->Snapshot().ResolveString("TRIGSTR_42").value, std::optional<std::string>(body));
    module.UndoStack()->undo();
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->Serialize().value, originalSettings);
    QCOMPARE(project.MapEdits()->Snapshot().Member(MapDataKind::Strings).sources.front().data, originalStrings);
    module.UndoStack()->redo(); QCOMPARE(project.MapEdits()->Snapshot().ResolveString("TRIGSTR_42").value, std::optional<std::string>(body));
    auto* path = page->findChild<QLineEdit*>(QStringLiteral("mapSettings_loadingScreen.path")); QVERIFY(path);
    QVERIFY(path->parentWidget()->findChild<QWidget*>(QStringLiteral("warcraftTextPreview"), Qt::FindDirectChildrenOnly) == nullptr);

    auto missing = members; missing.erase(missing.begin() + 1);
    QtProject::EditorProject unknown; OpenMembers(unknown, missing);
    Modules::MapSettingsModule isolated(unknown); auto* unknownPage = isolated.Widget();
    auto* unknownAuthor = unknownPage->findChild<QLineEdit*>(QStringLiteral("mapSettings_author")); QVERIFY(unknownAuthor);
    auto* unknownPreview = dynamic_cast<WarcraftTextPreview*>(unknownAuthor->parentWidget()->findChild<QWidget*>(QStringLiteral("warcraftTextPreview")));
    QVERIFY(unknownPreview && !unknownPreview->InsertReset());
    QVERIFY(unknownPreview->StatusText().contains(QStringLiteral("未确认")));
    QVERIFY(unknown.MapEdits()->Replacements().empty()); QCOMPARE(isolated.UndoStack()->count(), 0);
}

void MapSettingsModuleTests::NavigationRestoresSectionAndPreservesUnappliedDraft() {
    QtProject::EditorProject project; OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project); auto* page = module.Widget();
    auto* sections = page->findChild<QListWidget*>("mapSettingsSections");
    auto* author = page->findChild<QLineEdit*>("mapSettings_author"); QVERIFY(sections && author);
    QVERIFY(module.Reveal({"map-settings", "forces", {}, {}}));
    const auto bookmark = module.CaptureNavigationBookmark(); QVERIFY(bookmark.has_value());
    QCOMPARE(bookmark->value("section").toString(), QStringLiteral("forces"));
    const auto before = project.MapEdits()->Snapshot().Settings()->Serialize().value;
    const auto revision = project.MapEdits()->Revision(); const auto undoCount = project.mapDataUndoStack_->count();
    QVERIFY(module.Reveal({"map-settings", "general", {}, {}}));
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(sections->currentRow(), 2);
    auto invalid = *bookmark; invalid["section"] = "unknown";
    QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(sections->currentRow(), 2);
    invalid = *bookmark; invalid["scroll_v"] = 0.5;
    QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(sections->currentRow(), 2);
    author->setText("Unapplied Author"); QVERIFY(module.HasPendingDraft());
    QVERIFY(!module.PrepareNavigationLeave());
    QCOMPARE(module.RestoreNavigationBookmark(*bookmark), Shell::NavigationRestoreResult::Blocked);
    QVERIFY(module.HasPendingDraft()); QCOMPARE(author->text(), QStringLiteral("Unapplied Author"));
    QCOMPARE(sections->currentRow(), 2); QCOMPARE(project.MapEdits()->Revision(), revision);
    QCOMPARE(project.mapDataUndoStack_->count(), undoCount); QCOMPARE(project.MapEdits()->Snapshot().Settings()->Serialize().value, before);
}

void MapSettingsModuleTests::NewFormatIsVisibleButCannotEnterClassicSavePath() {
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded() && settings.value);
    settings.value->formatVersion = 31;
    settings.value->gameVersion = {1, 32, 0, 0};
    settings.value->scriptLanguage = 1;
    settings.value->graphicsModes = 3;
    settings.value->gameDataVersion = 1;
    const auto bytes = settings.value->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    QtProject::EditorProject project;
    OpenSyntheticMap(project, *bytes.value);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    QVERIFY(page);
    QVERIFY(page->findChild<QLineEdit*>("mapSettings_mapName") == nullptr);
    QVERIFY(!page->findChild<QPushButton*>("mapSettingsApply")->isEnabled());
    QVERIFY(!module.ApplyDraft());
    QVERIFY(!module.HasPendingDraft());
    QVERIFY(project.MapEdits()->Replacements().empty());
}

void MapSettingsModuleTests::NarrowLayoutKeepsDraftNavigationAndActions() {
    UiKit::ThemeManager theme;
    theme.ApplyTheme(qEnvironmentVariable("WAR3_TEST_THEME", "graphite"));
    QtProject::EditorProject project;
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded() && settings.value);
    settings.value->flags |= 0x80000000u;
    OpenSyntheticMap(project, *settings.value->Serialize().value);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* sections = page->findChild<QListWidget*>("mapSettingsSections");
    auto* compact = page->findChild<QComboBox*>("mapSettingsCompactSections");
    auto* pages = page->findChild<QStackedWidget*>("mapSettingsPages");
    auto* author = page->findChild<QLineEdit*>("mapSettings_author");
    QVERIFY(sections && compact && pages && author);
    author->setText(QStringLiteral("布局切换保留这份草稿"));
    for (const int width : {1080, 640, 480, 1080}) {
        page->findChild<QCheckBox*>("mapSettingsRawFields")->setChecked(width == 480);
        page->resize(width, 520);
        page->show();
        QApplication::processEvents();
        QCOMPARE(page->width(), width);
        QCOMPARE(compact->isVisible(), width < 760);
        QCOMPARE(sections->isVisible(), width >= 760);
        for (int section = 0; section < pages->count(); ++section) {
            if (compact->isVisible()) compact->setCurrentIndex(section);
            else sections->setCurrentRow(section);
            QApplication::processEvents();
            QCOMPARE(pages->currentIndex(), section);
            QCOMPARE(module.SaveViewState().value("section").toInt(), section);
            QCOMPARE(qobject_cast<QScrollArea*>(pages->currentWidget())->horizontalScrollBar()->maximum(), 0);
        }
        QVERIFY(module.Reveal({"map-settings", "general", "", {}}));
        QApplication::processEvents();
        for (const auto* name : {"mapSettingsApply", "mapSettingsDiscard"}) {
            auto* button = page->findChild<QPushButton*>(name);
            QVERIFY(button->isVisible() && button->isEnabled());
            QVERIFY(page->rect().contains(QRect(button->mapTo(page, QPoint()), button->size())));
        }
        QCOMPARE(author->text(), QStringLiteral("布局切换保留这份草稿"));
        const auto shots = qEnvironmentVariable("WAR3_TEST_SCREENSHOT_DIR");
        if (!shots.isEmpty()) QVERIFY(page->grab().save(QDir(shots).filePath(QStringLiteral("map-settings-%1.png").arg(width))));
    }
    QVERIFY(module.HasPendingDraft());
    QVERIFY(project.MapEdits()->Replacements().empty());
    QCOMPARE(module.UndoStack()->count(), 0);
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->author, std::string("布局切换保留这份草稿"));
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->flags, settings.value->flags);
}

void MapSettingsModuleTests::DraftCancelAndInvalidInputDoNotModifyDocument() {
    QtProject::EditorProject project;
    OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* author = page->findChild<QLineEdit*>("mapSettings_author");
    auto* flag = page->findChild<QCheckBox*>("mapSettingsFlag_disable_preview");
    auto* dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    QVERIFY(author);
    QVERIFY(flag);
    QVERIFY(dataSet);
    author->setText(QStringLiteral("Changed"));
    flag->setChecked(false);
    dataSet->setCurrentIndex(dataSet->findData(2));
    module.DiscardDraft();
    QCOMPARE(page->findChild<QLineEdit*>("mapSettings_author")->text(), QStringLiteral("Creator"));
    QVERIFY(page->findChild<QCheckBox*>("mapSettingsFlag_disable_preview")->isChecked());
    QCOMPARE(page->findChild<QComboBox*>("mapSettings_gameDataSet")->currentData().toInt(), 1);
    QVERIFY(project.MapEdits()->Replacements().empty());

    dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    const int meleeIndex = dataSet->findData(2);
    QVERIFY(meleeIndex >= 0);
    dataSet->setItemData(meleeIndex, QStringLiteral("invalid"));
    dataSet->setCurrentIndex(meleeIndex);
    QVERIFY(!module.ApplyDraft());
    QVERIFY(project.MapEdits()->Replacements().empty());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

// 同一会话的其他地图修改（如 Agent 写区域）不能抹掉未应用的设置草稿；
// 设置本身被改动后草稿保留但标为过期，应用被拒绝，放弃后载入当前设置。
void MapSettingsModuleTests::DraftSurvivesUnrelatedEditsAndBlocksAfterSettingsChange() {
    QtProject::EditorProject project;
    OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    const auto author = [&] { return page->findChild<QLineEdit*>("mapSettings_author"); };
    QVERIFY(author());
    author()->setText(QStringLiteral("Draft author"));
    QVERIFY(module.HasPendingDraft());

    MapData::RegionsDocument regions;
    QVERIFY(project.MapEdits()->EditRegions(regions).Succeeded());
    emit project.MapDocumentChanged();
    QVERIFY(module.HasPendingDraft() && !module.DraftIsStale());
    QCOMPARE(author()->text(), QStringLiteral("Draft author"));
    QVERIFY(module.ApplyDraft());
    QVERIFY(!module.HasPendingDraft());
    const auto applied = project.MapEdits()->Snapshot();
    QCOMPARE(applied.Settings()->author, std::string("Draft author"));
    QVERIFY(applied.Regions().has_value());

    author()->setText(QStringLiteral("Second draft"));
    auto external = *applied.Settings();
    external.mapName = "Changed elsewhere";
    QVERIFY(project.MapEdits()->EditSettings(external).Succeeded());
    emit project.MapDocumentChanged();
    QVERIFY(module.HasPendingDraft() && module.DraftIsStale());
    QCOMPARE(author()->text(), QStringLiteral("Second draft"));
    const auto revision = project.MapEdits()->Revision();
    const auto undoCount = project.mapDataUndoStack_->count();
    QVERIFY(!module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Revision(), revision);
    QCOMPARE(project.mapDataUndoStack_->count(), undoCount);
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->author, std::string("Draft author"));
    QVERIFY(module.HasPendingDraft());

    module.DiscardDraft();
    QVERIFY(!module.HasPendingDraft() && !module.DraftIsStale());
    QCOMPARE(page->findChild<QLineEdit*>("mapSettings_mapName")->text(), QStringLiteral("Changed elsewhere"));
    QCOMPARE(author()->text(), QStringLiteral("Draft author"));
}

// 控制类型/种族显示中文名称、用下拉框写回整数；手工写入未知值不能应用。
void MapSettingsModuleTests::PlayerControllerAndRaceUseNamedPickers() {
    using Kind = Modules::MapPlayerEnumDelegate::Kind;
    QtProject::EditorProject project;
    OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* players = page->findChild<QTableWidget*>("mapSettingsPlayers");
    auto* raw = page->findChild<QCheckBox*>("mapSettingsRawFields");
    QVERIFY(players && raw && players->rowCount() > 0);
    auto* controller = dynamic_cast<Modules::MapPlayerEnumDelegate*>(players->itemDelegateForColumn(2));
    auto* race = dynamic_cast<Modules::MapPlayerEnumDelegate*>(players->itemDelegateForColumn(3));
    QVERIFY(controller && race);
    const auto source = *project.mapDocument_->Settings();
    const QVariant stored = players->item(0, 2)->data(Qt::DisplayRole);
    QCOMPARE(controller->displayText(stored, QLocale()), Modules::MapPlayerEnumDelegate::Label(Kind::Controller, source.players[0].controller));
    QCOMPARE(Modules::MapPlayerEnumDelegate::Label(Kind::Race, 4), QStringLiteral("暗夜精灵族"));
    QCOMPARE(Modules::MapPlayerEnumDelegate::Label(Kind::Controller, 9), QStringLiteral("未知（9）"));
    raw->setChecked(true);
    QCOMPARE(controller->displayText(stored, QLocale()), QString::number(source.players[0].controller));
    raw->setChecked(false);

    const auto index = players->model()->index(0, 2);
    QStyleOptionViewItem option;
    std::unique_ptr<QWidget> editor(controller->createEditor(players->viewport(), option, index));
    auto* combo = qobject_cast<QComboBox*>(editor.get());
    QVERIFY(combo);
    controller->setEditorData(combo, index);
    QCOMPARE(combo->count(), 4);
    const int target = source.players[0].controller == 2 ? 3 : 2;
    const int choice = combo->findData(target);
    QVERIFY(choice >= 0);
    combo->setCurrentIndex(choice);
    controller->setModelData(combo, players->model(), index);
    // 编辑器挂在表格视口下；应用会重建表格，先释放，避免随旧视口删除后再次删除。
    editor.reset();
    QCOMPARE(players->item(0, 2)->text(), QString::number(target));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(MapData::MapSettings::Parse(project.MapEdits()->Replacements().front().data).value->players[0].controller, target);

    players = page->findChild<QTableWidget*>("mapSettingsPlayers");
    players->item(0, 3)->setText(QStringLiteral("9"));
    const auto before = project.MapEdits()->Revision();
    QVERIFY(!module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Revision(), before);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
}

void MapSettingsModuleTests::ApplyUndoRedoPreservesUneditedBytesAndOtherFields() {
    QtProject::EditorProject project;
    OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* author = page->findChild<QLineEdit*>("mapSettings_author");
    auto* players = page->findChild<QTableWidget*>("mapSettingsPlayers");
    auto* forces = page->findChild<QTableWidget*>("mapSettingsForces");
    auto* flag = page->findChild<QCheckBox*>("mapSettingsFlag_disable_preview");
    auto* dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    QVERIFY(author);
    QVERIFY(players);
    QVERIFY(forces);
    QVERIFY(flag);
    QVERIFY(dataSet);
    author->setText(QStringLiteral("New Creator"));
    flag->setChecked(false);
    dataSet->setCurrentIndex(dataSet->findData(2));
    players->item(0, 1)->setText(QStringLiteral("Player One"));
    forces->item(0, 0)->setText(QStringLiteral("Blue Team"));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(module.UndoStack(), project.mapDataUndoStack_);
    const auto replacements = project.MapEdits()->Replacements();
    QCOMPARE(replacements.size(), std::size_t(1));
    const auto changed = MapData::MapSettings::Parse(replacements.front().data);
    QVERIFY(changed.Succeeded());
    QCOMPARE(changed.value->author, std::string("New Creator"));
    QCOMPARE(changed.value->players[0].name, std::string("Player One"));
    QCOMPARE(changed.value->forces[0].name, std::string("Blue Team"));
    QCOMPARE(changed.value->flags, project.mapDocument_->Settings()->flags & ~std::uint32_t{1});
    QCOMPARE(changed.value->gameDataSet, 2);
    // 夹具中的非 UTF-8 名称必须保留原始字节，不被 Qt 文本转换改写。
    QCOMPARE(changed.value->mapName, project.mapDocument_->Settings()->mapName);
    QVERIFY(changed.value->UnknownTail() == project.mapDocument_->Settings()->UnknownTail());

    project.mapDataUndoStack_->undo();
    QVERIFY(project.MapEdits()->Replacements().empty());
    QCOMPARE(page->findChild<QLineEdit*>("mapSettings_author")->text(), QStringLiteral("Creator"));
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->author, std::string("New Creator"));
}

void MapSettingsModuleTests::UnknownDataSetIsPreservedUntilExplicitlyChanged() {
    QtProject::EditorProject project;
    const auto bytes = SettingsBytesWithDataSet(73);
    QVERIFY(!bytes.empty());
    OpenSyntheticMap(project, bytes);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    QVERIFY(dataSet);
    QCOMPARE(dataSet->currentData().toInt(), 73);
    QVERIFY(dataSet->currentText().contains(QStringLiteral("未知")));
    page->findChild<QLineEdit*>("mapSettings_author")->setText(QStringLiteral("Edited"));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->gameDataSet, 73);

    dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    const int customIndex = dataSet->findData(1);
    QVERIFY(customIndex >= 0);
    dataSet->setCurrentIndex(customIndex);
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->gameDataSet, 1);

    const auto defaultBytes = SettingsBytesWithDataSet(-1);
    QVERIFY(!defaultBytes.empty());
    OpenSyntheticMap(project, defaultBytes);
    dataSet = page->findChild<QComboBox*>("mapSettings_gameDataSet");
    QVERIFY(dataSet);
    QCOMPARE(dataSet->currentData().toInt(), -1);
    page->findChild<QLineEdit*>("mapSettings_author")->setText(QStringLiteral("Edited"));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->gameDataSet, -1);
}

void MapSettingsModuleTests::RawLabelsAndDocumentReplacement() {
    QtProject::EditorProject project;
    OpenSyntheticMap(project);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* raw = page->findChild<QCheckBox*>("mapSettingsRawFields");
    auto* authorLabel = page->findChild<QLabel*>("mapSettingsLabel_author");
    auto* previewFlag = page->findChild<QCheckBox*>("mapSettingsFlag_disable_preview");
    QVERIFY(raw);
    QVERIFY(authorLabel);
    QVERIFY(previewFlag);
    QCOMPARE(authorLabel->text(), QStringLiteral("作者"));
    QCOMPARE(previewFlag->text(), QStringLiteral("关闭预览图"));
    raw->setChecked(true);
    QCOMPARE(authorLabel->text(), QStringLiteral("author"));
    QCOMPARE(previewFlag->text(), QStringLiteral("disable_preview"));
    QCOMPARE(module.SaveViewState().value("rawFields").toBool(), true);
    OpenSyntheticMap(project);
    QCOMPARE(module.Widget(), page);
    QVERIFY(page->findChild<QLineEdit*>("mapSettings_author"));
    QCOMPARE(module.SaveViewState().value("rawFields").toBool(), true);
    QVERIFY(!module.Reveal({"objects", "", "", {}}));
    QVERIFY(module.Reveal({"map-settings", "players", "", {}}));
}

void MapSettingsModuleTests::ClassicFlagsAreEditableAndUnknownBitsSurviveUndo() {
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded() && settings.value);
    settings.value->flags = 0x8001e000u;
    const auto bytes = settings.value->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    QtProject::EditorProject project;
    OpenSyntheticMap(project, *bytes.value);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    const QStringList flags{"use_terrain_fog", "requires_expansion", "use_item_classification", "use_water_tint"};
    for (const auto& flag : flags) {
        auto* check = page->findChild<QCheckBox*>(QStringLiteral("mapSettingsFlag_%1").arg(flag));
        QVERIFY(check);
        QVERIFY(check->isChecked());
    }
    const auto* note = page->findChild<QLabel*>("mapSettingsUnknownFlags");
    QVERIFY(note);
    QVERIFY(note->text().contains(QStringLiteral("0x80000000")));
    QVERIFY(!note->text().contains(QStringLiteral("0x8001e000")));
    auto* fog = page->findChild<QCheckBox*>("mapSettingsFlag_use_terrain_fog");
    fog->setChecked(false);
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->flags, 0x8001c000u);
    module.UndoStack()->undo();
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->flags, 0x8001e000u);
    QVERIFY(project.MapEdits()->Replacements().empty());
    module.UndoStack()->redo();
    QCOMPARE(project.MapEdits()->Snapshot().Settings()->flags, 0x8001c000u);
}

void MapSettingsModuleTests::PlayerStartEditsUndoTheirLinkedPlacement() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    QVERIFY(units.Succeeded() && units.value);
    MapData::UnitPlacement start;
    start.rawcode = "sloc";
    start.scale = {1, 1, 1};
    start.position = {100, 200, 0};
    units.value->units.push_back(start);
    const auto bytes = units.value->Serialize();
    QVERIFY(bytes.Succeeded() && bytes.value);
    const std::vector<MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3mapUnits.doo", *bytes.value}};
    QtProject::EditorProject project;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    Modules::MapSettingsModule module(project);
    auto* page = module.Widget();
    auto* players = page->findChild<QTableWidget*>("mapSettingsPlayers");
    QVERIFY(players);
    players->item(0, 5)->setText(QStringLiteral("750"));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.mapEdits_->Replacements().size(), std::size_t{2});
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 750.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[1],
        project.mapDocument_->Settings()->players[0].startY);
    QCOMPARE(module.UndoStack()->count(), 1);
    module.UndoStack()->undo();
    QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 100.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[1], 200.0f);
    module.UndoStack()->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 750.0f);
}

void MapSettingsModuleTests::ReferencedTextPreservesWireAndRefreshesTheCurrentSharedBody() {
    const auto members = ReferencedSettingsMembers(); QVERIFY(!members.empty());
    QtProject::EditorProject project; OpenMembers(project, members);
    Modules::MapSettingsModule module(project); auto* page = module.Widget();
    auto* author = page->findChild<QLineEdit*>("mapSettings_author");
    auto* description = page->findChild<QPlainTextEdit*>("mapSettings_description");
    auto* recommended = page->findChild<QLineEdit*>("mapSettings_recommendedPlayers");
    QVERIFY(author && description && recommended);
    QCOMPARE(author->text(), QStringLiteral("原作者与描述")); QCOMPARE(description->toPlainText(), QStringLiteral("原作者与描述"));
    QVERIFY(author->toolTip().contains(QStringLiteral("其它地图引用")));
    author->setText(QStringLiteral("共享新正文")); recommended->setText(QStringLiteral("3–5"));
    QVERIFY(module.ApplyDraft());
    QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{1}); QCOMPARE(module.UndoStack()->count(), 1);
    const auto current = project.MapEdits()->Snapshot();
    QVERIFY(current.Member(MapDataKind::Settings).sources.front().data == members.front().data);
    QCOMPARE(current.Settings()->author, std::string("TRIGSTR_00042")); QCOMPARE(current.Settings()->description, std::string("TRIGSTR_0042"));
    QCOMPARE(current.Settings()->recommendedPlayers, std::string("TRIGSTR_7"));
    QVERIFY(current.ResolveString(current.Settings()->description).value == std::optional<std::string>("共享新正文"));
    auto expected = std::string(reinterpret_cast<const char*>(members[1].data.data()), members[1].data.size());
    expected.replace(expected.find("原作者与描述"), std::string("原作者与描述").size(), "共享新正文");
    expected.replace(expected.find("2–4"), std::string("2–4").size(), "3–5");
    const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(expected.data()), reinterpret_cast<const std::byte*>(expected.data() + expected.size()));
    QVERIFY(current.Member(MapDataKind::Strings).sources.front().data == bytes);
    QCOMPARE(page->findChild<QLineEdit*>("mapSettings_author")->text(), QStringLiteral("共享新正文"));
    QCOMPARE(page->findChild<QPlainTextEdit*>("mapSettings_description")->toPlainText(), QStringLiteral("共享新正文"));
    QCOMPARE(page->findChild<QLineEdit*>("mapSettings_recommendedPlayers")->text(), QStringLiteral("3–5"));
    // 页面回显必须使用会话 WTS，不能回退到 project_.mapDocument_ 的旧正文。
    module.Refresh(); QCOMPARE(page->findChild<QLineEdit*>("mapSettings_author")->text(), QStringLiteral("共享新正文"));
    module.UndoStack()->undo(); QVERIFY(!project.MapEdits()->IsDirty());
    QVERIFY(project.MapEdits()->Snapshot().Member(MapDataKind::Strings).sources.front().data == members[1].data);
    QCOMPARE(page->findChild<QPlainTextEdit*>("mapSettings_description")->toPlainText(), QStringLiteral("原作者与描述"));
    module.UndoStack()->redo(); QVERIFY(project.MapEdits()->Snapshot().Member(MapDataKind::Strings).sources.front().data == bytes);
    QCOMPARE(page->findChild<QPlainTextEdit*>("mapSettings_description")->toPlainText(), QStringLiteral("共享新正文"));
}

void MapSettingsModuleTests::ReferencedTextAndPlayerStartShareOneUndoCommand() {
    const auto members = ReferencedSettingsMembers(true); QVERIFY(members.size() == 3);
    QtProject::EditorProject project; OpenMembers(project, members);
    Modules::MapSettingsModule module(project); auto* page = module.Widget();
    auto* author = page->findChild<QLineEdit*>("mapSettings_author");
    auto* players = page->findChild<QTableWidget*>("mapSettingsPlayers");
    auto* flag = page->findChild<QCheckBox*>("mapSettingsFlag_disable_preview");
    QVERIFY(author && players && flag);
    author->setText(QStringLiteral("联动共享正文")); players->item(0, 5)->setText(QStringLiteral("750")); flag->setChecked(false);
    QVERIFY(module.ApplyDraft()); QCOMPARE(module.UndoStack()->count(), 1);
    QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{1}); QCOMPARE(project.MapEdits()->Replacements().size(), std::size_t{3});
    const auto current = project.MapEdits()->Snapshot();
    QCOMPARE(current.Settings()->author, std::string("TRIGSTR_00042")); QCOMPARE(current.Settings()->description, std::string("TRIGSTR_0042"));
    QCOMPARE(current.Settings()->players[0].startX, 750.0f); QCOMPARE(current.Units()->units[0].position[0], 750.0f);
    QCOMPARE(current.Units()->units[0].position[1], project.mapDocument_->Settings()->players[0].startY);
    QCOMPARE(current.Units()->units[0].position[2], 16.0f);
    QCOMPARE(current.Settings()->flags, project.mapDocument_->Settings()->flags & ~std::uint32_t{1});
    QVERIFY(current.ResolveString(current.Settings()->description).value == std::optional<std::string>("联动共享正文"));
    module.UndoStack()->undo(); QVERIFY(!project.MapEdits()->IsDirty());
    const auto original = project.MapEdits()->Snapshot();
    for (const auto& member : members) {
        const auto found = std::find_if(original.Members().begin(), original.Members().end(), [&](const auto& state) { return state.memberName == member.name; });
        QVERIFY(found != original.Members().end()); QVERIFY(found->sources.front().data == member.data);
    }
    module.UndoStack()->redo(); QCOMPARE(project.MapEdits()->Snapshot().Units()->units[0].position[0], 750.0f);
    QVERIFY(project.MapEdits()->Snapshot().ResolveString("TRIGSTR_42").value == std::optional<std::string>("联动共享正文"));
}

void MapSettingsModuleTests::ReferencedTextRejectsConflictsUnsafeAndChangedSources() {
    const auto members = ReferencedSettingsMembers(); QVERIFY(!members.empty());
    QtProject::EditorProject project; OpenMembers(project, members);
    Modules::MapSettingsModule module(project); auto* page = module.Widget();
    page->findChild<QLineEdit*>("mapSettings_author")->setText(QStringLiteral("作者正文"));
    page->findChild<QPlainTextEdit*>("mapSettings_description")->setPlainText(QStringLiteral("冲突描述"));
    QVERIFY(!module.ApplyDraft()); QCOMPARE(module.UndoStack()->count(), 0); QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{0});
    QVERIFY(project.MapEdits()->Replacements().empty());
    module.DiscardDraft();
    page->findChild<QLineEdit*>("mapSettings_author")->setText(QStringLiteral("待提交正文"));
    auto strings = *project.MapEdits()->Snapshot().Strings(); strings.entries[1].value = "外部建议玩家";
    QVERIFY(project.MapEdits()->EditStrings(strings).Succeeded());
    QVERIFY(!module.ApplyDraft()); QCOMPARE(project.MapEdits()->Revision(), std::uint64_t{1}); QCOMPARE(module.UndoStack()->count(), 0);
    QCOMPARE(project.MapEdits()->Snapshot().Strings()->entries[0].value, std::string("原作者与描述"));
    QCOMPARE(project.MapEdits()->Snapshot().Strings()->entries[1].value, std::string("外部建议玩家"));
    QVERIFY(project.MapEdits()->Undo()); QVERIFY(!project.MapEdits()->IsDirty());
    for (bool duplicate : {false, true}) {
        auto unsafe = members;
        if (duplicate) {
            const std::string text = "STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\nSTRING 7\n{\n2–4\n}\n";
            unsafe[1].data.assign(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size()));
        } else unsafe.erase(unsafe.begin() + 1);
        QtProject::EditorProject isolated; OpenMembers(isolated, unsafe);
        Modules::MapSettingsModule editor(isolated); auto* isolatedPage = editor.Widget();
        isolatedPage->findChild<QLineEdit*>("mapSettings_author")->setText(QStringLiteral("new"));
        QVERIFY(!editor.ApplyDraft()); QVERIFY(isolated.MapEdits()->Replacements().empty());
        QCOMPARE(editor.UndoStack()->count(), 0); QCOMPARE(isolated.MapEdits()->Revision(), std::uint64_t{0});
        QVERIFY(isolated.MapEdits()->Snapshot().Member(MapDataKind::Settings).sources.front().data == unsafe.front().data);
    }
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    MapSettingsModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MapSettingsModuleTests.moc"
