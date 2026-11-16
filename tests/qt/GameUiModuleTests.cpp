#include "AudastraUiCatalog.h"
#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiKindPicker.h"
#include "GameUiModule.h"
#include "GameUiToolService.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>

#include <algorithm>

using namespace War3Map;
using namespace War3Map::GameUi;

namespace {

// 原创合成界面定义；不含任何游戏文件。
Modules::GameUiDefinitions SyntheticDefinitions() {
    const std::string text =
        "StringList { KEY_HELLO \"Hello\", }\n"
        "Frame \"SIMPLEFRAME\" \"ConsoleUI\" {\n"
        "    DecorateFileNames,\n"
        "    Layer \"BACKGROUND\" { Texture \"ConsoleTile\" { File \"Tile\", } }\n"
        "    String \"ConsoleText\" { Anchor CENTER, 0.0, 0.0, Text \"KEY_HELLO\", }\n"
        "}\n"
        "Frame \"BACKDROP\" \"Box\" {\n"
        "    Width 0.2, Height 0.1,\n"
        "    BackdropBackground \"Box.blp\",\n"
        "}\n"
        "Texture \"NotAFrame\" { File \"X.blp\", }\n";
    auto parsed = ParseFdf(text, "UI\\Synthetic.fdf");
    auto library = std::make_shared<FrameLibrary>();
    library->AddParsed(std::move(*parsed.value), "game");
    Modules::GameUiDefinitions definitions;
    definitions.library = library;
    definitions.gameSkins = "[Default]\nTile=UI\\Default\\Tile\n[Orc]\nTile=UI\\Orc\\Tile\n";
    return definitions;
}

QTreeWidgetItem* FindItem(QTreeWidget* tree, const QString& name) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->text(0) == name) return *it;
    }
    return nullptr;
}

bool HasProblem(const QStandardItemModel* model, const QString& code) {
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 2).data().toString() == code) return true;
    }
    return false;
}

} // namespace

class GameUiModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void BuildsSceneFromDefinitions();
    void RootsRaceAndVisibility();
    void ViewStateRoundTrip();
    void CanvasDrawsAndHitsTextures();
    void SimulationDrivesScriptFrames();
    void DescriptionPreviewDrivesFrames();
    void AuthoringStagesDescriptionImport();
    void DescriptionDataTablesEdit();
    void GeneratesDescriptionTriggers();
    void ImportsDescriptionFile();
    void FramePropertiesEditLayoutAndText();
    void KindPickerCreatesTypedFrames();
    void FrameSettingsCardAndTexturePicker();
    void TriggerSimulationRunsAuthorTriggers();
    void OriginAndClipboardTransactions();
    void OriginalScalePinsPosition();
    void VisibleSelectableAndPicking();
    void NewUiFlowAndLayerOrder();
    void GroupedPropertiesUnitsAndEvents();
};

void GameUiModuleTests::BuildsSceneFromDefinitions() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    QVERIFY(page);
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(module.Tree());
    QVERIFY(module.AvailableTemplates().contains(QStringLiteral("ConsoleUI")));
    QVERIFY(!module.AvailableTemplates().contains(QStringLiteral("NotAFrame")));

    auto* nodes = page->findChild<QTreeWidget*>(QStringLiteral("gameUiNodes"));
    QVERIFY(nodes);
    QVERIFY(FindItem(nodes, QStringLiteral("ConsoleUI")));
    QVERIFY(FindItem(nodes, QStringLiteral("ConsoleTile")));
    QCOMPARE(FindItem(nodes, QStringLiteral("ConsoleText"))->text(2), QStringLiteral("随文字"));
    // 默认带出游戏代码区域：树里显示中文名，按标识也能选中。
    QVERIFY(module.Roots().contains(QStringLiteral("GameUI")));
    QVERIFY(module.AvailableTemplates().contains(QStringLiteral("GameUI")));
    QVERIFY(FindItem(nodes, QStringLiteral("小地图")));
    QVERIFY(module.SelectFrame(QStringLiteral("CommandBar")));
    QCOMPARE(module.Tree()->Nodes()[module.SelectedNode()].frameType, std::string("CCommandBar"));

    // 默认根中合成定义里没有的两个界面如实报告缺失。
    auto* problems = page->findChild<QStandardItemModel*>(QStringLiteral("gameUiProblemModel"));
    QVERIFY(problems);
    QVERIFY(HasProblem(problems, QStringLiteral("GAMEUI_TEMPLATE_MISSING")));

    QVERIFY(module.SelectFrame(QStringLiteral("ConsoleTile")));
    auto* properties = page->findChild<QTreeWidget*>(QStringLiteral("gameUiProperties"));
    QVERIFY(properties);
    QTreeWidgetItem* path = nullptr;
    for (QTreeWidgetItemIterator it(properties); *it; ++it) {
        if ((*it)->text(0) == QStringLiteral("路径") && !path) path = *it;
    }
    QVERIFY(path);
    QCOMPARE(path->text(1), QStringLiteral("UI\\Default\\Tile.blp"));

    auto* raw = page->findChild<QCheckBox*>(QStringLiteral("showRawFields"));
    QVERIFY(raw);
    raw->setChecked(true);
    bool rawGroup = false;
    for (int i = 0; i < properties->topLevelItemCount(); ++i) rawGroup = rawGroup || properties->topLevelItem(i)->text(0) == QStringLiteral("原始字段");
    QVERIFY(rawGroup);
}

void GameUiModuleTests::OriginAndClipboardTransactions() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY(module.SelectFrame(QStringLiteral("Minimap")));
    const auto oldRect = module.Tree()->Nodes()[module.SelectedNode()].rect;
    QVERIFY2(module.EditOriginNode(QStringLiteral("Minimap"), {{"visible", false}, {"scale", 1.2}}, &error), qPrintable(error));
    QVERIFY(module.Description(QStringLiteral("OriginalUi")));
    QCOMPARE(module.Description(QStringLiteral("OriginalUi"))->origins.size(), std::size_t{1});
    QVERIFY(module.Tree()->Nodes()[module.SelectedNode()].hidden);
    QVERIFY2(module.MoveOriginNode(module.SelectedNode(), .02, .03, &error), qPrintable(error));
    const auto rect = module.Tree()->Nodes()[module.SelectedNode()].rect;
    // 画布拖动按0.0001屏幕单位吸附，原版初始坐标可能有五位小数。
    QVERIFY(std::abs(rect.left - oldRect.left - .02) < .000051);
    QVERIFY2(module.RestoreOriginNode(QStringLiteral("Minimap"), &error), qPrintable(error));
    QVERIFY(module.Description(QStringLiteral("OriginalUi"))->origins.empty());
    QVERIFY(!module.Tree()->Nodes()[module.SelectedNode()].hidden);
    QVERIFY(std::abs(module.Tree()->Nodes()[module.SelectedNode()].rect.left - oldRect.left) < .00001);
    const auto originPayload = [&]() {
        for (const auto& asset : project.AssetEntries()) if (asset.path.compare(QStringLiteral("AudastraUI\\OriginalUi.json"), Qt::CaseInsensitive) == 0) return asset.ownedPayload;
        return std::shared_ptr<const std::vector<std::byte>>{};
    };
    QVERIFY(originPayload()); const auto restored = *originPayload();
    QVERIFY2(module.CreateDescription(QStringLiteral("Own"), {}, &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("button"), &error), QStringLiteral("button1"));
    QVERIFY2(module.CopySelectedDescriptionFrame(&error), qPrintable(error));
    QVERIFY(module.SelectFrame(QStringLiteral("Own")));
    QVERIFY2(module.PasteDescriptionFrame(&error), qPrintable(error));
    QVERIFY(FindUiFrame(*module.Description(QStringLiteral("Own")), "button1_copy"));
    QVERIFY2(module.CreateDescription(QStringLiteral("Other"), {}, &error), qPrintable(error));
    QVERIFY2(module.PasteDescriptionFrame(&error), qPrintable(error));
    QVERIFY(FindUiFrame(*module.Description(QStringLiteral("Other")), "button1"));
    QVERIFY(!module.CopyOriginTemplate(*module.Tree()->FindByName("Minimap"), &error));
    QVERIFY2(module.CopyOriginTemplate(*module.Tree()->FindByName("ConsoleUI"), &error), qPrintable(error));
    QVERIFY(module.Description(QStringLiteral("CopiedUi")));
    QVERIFY2(module.EditOriginNode(QStringLiteral("Minimap"), {{"visible", false}}, &error), qPrintable(error));
    const auto hidden = *originPayload();
    project.AssetUndoStack()->undo(); QCOMPARE(*originPayload(), restored);
    project.AssetUndoStack()->redo(); QCOMPARE(*originPayload(), hidden);
    Q_UNUSED(page);
}

void GameUiModuleTests::OriginalScalePinsPosition() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(module.SelectFrame(QStringLiteral("Portrait")));
    const auto rect = module.Tree()->Nodes()[module.SelectedNode()].rect;
    QString error;
    QVERIFY2(module.EditOriginNode(QStringLiteral("Portrait"), {{"scale", 1.2}}, &error), qPrintable(error));
    const auto& change = module.Description(QStringLiteral("OriginalUi"))->origins.front();
    QCOMPARE(change.name, std::string("Portrait")); QVERIFY(change.position);
    QCOMPARE(change.position->targetPoint, FramePoint::BottomLeft);
    QVERIFY(std::abs(change.position->x - (rect.left + rect.right) / 2) < .00001);
    project.AssetUndoStack()->undo();
    QTRY_VERIFY(!module.Description(QStringLiteral("OriginalUi")) || module.Description(QStringLiteral("OriginalUi"))->origins.empty());
}

void GameUiModuleTests::RootsRaceAndVisibility() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());

    QVERIFY(!module.AddRoot(QStringLiteral("NotAFrame")));
    QVERIFY(module.AddRoot(QStringLiteral("box")));
    QVERIFY(module.Roots().contains(QStringLiteral("Box")));
    QCOMPARE(module.Tree()->Nodes()[module.SelectedNode()].name, std::string("Box"));
    QVERIFY(module.RemoveRoot(QStringLiteral("BOX")));
    QVERIFY(!module.Tree()->FindByName("Box"));

    const auto tilePath = [&] {
        const auto id = module.Tree()->FindByName("ConsoleTile");
        return id ? module.Tree()->Nodes()[*id].texture->path : std::string();
    };
    QCOMPARE(tilePath(), std::string("UI\\Default\\Tile.blp"));
    module.SetRace(QStringLiteral("Orc"));
    QCOMPARE(tilePath(), std::string("UI\\Orc\\Tile.blp"));

    const auto before = module.DrawList().commands.size();
    QVERIFY(before >= 2);
    auto* nodes = page->findChild<QTreeWidget*>(QStringLiteral("gameUiNodes"));
    const auto nodeOf = [nodes](const QString& name) {
        QTreeWidgetItem* item = FindItem(nodes, name);
        return item ? item->data(0, Qt::UserRole + 1).toUInt() : kNoNode;
    };
    QVERIFY(nodeOf(QStringLiteral("ConsoleTile")) != kNoNode);
    module.SetNodeVisible(nodeOf(QStringLiteral("ConsoleTile")), false);
    QCOMPARE(module.DrawList().commands.size(), before - 1);
    // 隐藏按节点身份保存，种族切换重建后仍然隐藏。
    module.SetRace(QStringLiteral("Human"));
    QCOMPARE(module.DrawList().commands.size(), before - 1);
    QVERIFY(!module.NodeVisible(nodeOf(QStringLiteral("ConsoleTile"))));

    // 只显示一项：同级隐藏，自身与父节点显示。
    module.SetNodeVisible(nodeOf(QStringLiteral("ConsoleText")), false);
    module.ShowOnly(nodeOf(QStringLiteral("ConsoleText")));
    QVERIFY(module.NodeVisible(nodeOf(QStringLiteral("ConsoleText"))));
    QVERIFY(!module.NodeVisible(nodeOf(QStringLiteral("层 BACKGROUND"))));
    std::vector<FrameDrawCommand> content;
    for (const auto& command : module.DrawList().commands) {
        if (command.kind != DrawKind::CodeArea) content.push_back(command);
    }
    QCOMPARE(content.size(), std::size_t{1});
    QVERIFY(content[0].kind == DrawKind::Text);
    QCOMPARE(module.Tree()->Nodes()[module.SelectedNode()].name, std::string("ConsoleText"));
}

void GameUiModuleTests::ViewStateRoundTrip() {
    QtProject::EditorProject project;
    QJsonObject saved;
    {
        Modules::GameUiModule module(project);
        (void)module.Widget();
        module.LoadDefinitionsForTesting(SyntheticDefinitions());
        QVERIFY(module.AddRoot(QStringLiteral("Box")));
        module.SetRace(QStringLiteral("Undead"));
        module.SetExpansion(false);
        saved = module.SaveViewState();
    }
    Modules::GameUiModule restored(project);
    restored.RestoreViewState(saved);  // 页面创建前恢复，创建时套用
    (void)restored.Widget();
    restored.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(restored.Roots().contains(QStringLiteral("Box")));
    QCOMPARE(restored.Tree()->Options().race, std::string("Undead"));
    QVERIFY(!restored.Tree()->Options().expansion);
}

void GameUiModuleTests::CanvasDrawsAndHitsTextures() {
    const std::string text =
        "Frame \"SIMPLEFRAME\" \"Root\" {\n"
        "    Texture \"Left\" { File \"Red.blp\", Width 0.4, Height 0.6, Anchor BOTTOMLEFT, 0, 0, }\n"
        "    Texture \"Right\" { File \"Clear.blp\", Width 0.4, Height 0.6, Anchor BOTTOMRIGHT, 0, 0, }\n"
        "}\n";
    auto parsed = ParseFdf(text, "UI\\Canvas.fdf");
    QVERIFY(parsed.Succeeded());
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    FrameTree tree;
    QVERIFY(tree.Instantiate(library, nullptr, "Root", RootPlacement{true, {}}).Succeeded());
    (void)tree.Layout();

    QHash<QString, QImage> textures;
    QImage red(4, 4, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    QImage clear(4, 4, QImage::Format_ARGB32_Premultiplied);
    clear.fill(Qt::transparent);
    textures.insert(Modules::GameUiAssetKey(QStringLiteral("Red.blp")), red);
    textures.insert(Modules::GameUiAssetKey(QStringLiteral("Clear.blp")), clear);

    Modules::GameUiCanvas canvas;
    canvas.resize(424, 324);
    canvas.SetAspect(4.0 / 3.0);
    canvas.SetTextures(&textures);
    canvas.SetScene(&tree, BuildDrawList(tree));
    const QImage image = canvas.RenderImage(QSize(400, 300));
    QCOMPARE(image.pixelColor(100, 150), QColor(255, 0, 0));
    QCOMPARE(image.pixelColor(300, 150), QColor(10, 12, 13));

    (void)canvas.grab();  // 绘制一次以建立点击区域
    const QRectF screen = canvas.ScreenRect();
    const auto left = canvas.NodeAt(screen.center() - QPointF(screen.width() / 4.0, 0));
    QCOMPARE(tree.Nodes()[left].name, std::string("Left"));
    // 透明贴图处退回到包含该点的最小框架。
    const auto right = canvas.NodeAt(screen.center() + QPointF(screen.width() / 4.0, 0));
    QCOMPARE(tree.Nodes()[right].name, std::string("Root"));

    // 不能点中的节点照常画，点击穿过它：先落到能点中的父框架，再都不能点中时落到空白处。
    std::vector<bool> pickable(tree.Nodes().size(), true);
    pickable[left] = false;
    canvas.SetPickable(pickable);
    (void)canvas.grab();
    QCOMPARE(tree.Nodes()[canvas.NodeAt(screen.center() - QPointF(screen.width() / 4.0, 0))].name, std::string("Root"));
    pickable[right] = false;
    canvas.SetPickable(pickable);
    QCOMPARE(canvas.NodeAt(screen.center() - QPointF(screen.width() / 4.0, 0)), kNoNode);
    QCOMPARE(canvas.RenderImage(QSize(400, 300)).pixelColor(100, 150), QColor(255, 0, 0));
}

void GameUiModuleTests::SimulationDrivesScriptFrames() {
    // 原创脚本：初始化时创建血条，更新函数按单位生命百分比改宽度。
    const std::string script = R"(globals
    framehandle udg_Bar = null
    unit udg_Unit = null
endglobals
function InitBar takes nothing returns nothing
    set udg_Bar = BlzCreateFrameByType("BACKDROP", "Bar", BlzGetOriginFrame(ORIGIN_FRAME_GAME_UI, 0), "", 0)
    call BlzFrameSetAbsPoint(udg_Bar, FRAMEPOINT_BOTTOMLEFT, 0.3, 0.2)
    call BlzFrameSetSize(udg_Bar, 0.2, 0.02)
endfunction
function UpdateBar takes nothing returns nothing
    call BlzFrameSetSize(udg_Bar, 0.2 * GetUnitLifePercent(udg_Unit) / 100.0, 0.02)
endfunction
)";
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    auto definitions = SyntheticDefinitions();
    definitions.scripts.push_back({"war3map.j", ScriptLanguage::Jass, script});
    definitions.scriptFrames = FindScriptFrames(definitions.scripts);
    module.LoadDefinitionsForTesting(std::move(definitions));
    QCOMPARE(module.SimulationFunctions(), QStringList{QStringLiteral("UpdateBar")});
    const auto barWidth = [&module] {
        if (!module.SelectFrame(QStringLiteral("Bar"))) return -1.0f;
        return module.Tree()->Nodes()[module.SelectedNode()].rect.Width();
    };
    QVERIFY(qAbs(barWidth() - 0.2f) < 1e-4f);

    auto* box = page->findChild<QComboBox*>(QStringLiteral("gameUiSimulationFunction"));
    QVERIFY(box);
    QCOMPARE(box->count(), 2);
    box->setCurrentIndex(box->findData(QStringLiteral("UpdateBar")));
    QCOMPARE(module.SimulationFunction(), QStringLiteral("UpdateBar"));
    auto* inputs = page->findChild<QTableWidget*>(QStringLiteral("gameUiSimulationInputs"));
    QVERIFY(inputs);
    QCOMPARE(inputs->rowCount(), 1);
    QVERIFY(module.Simulation());
    QCOMPARE(module.Simulation()->inputs.front().key, std::string("GetUnitLifePercent(udg_Unit)"));

    module.SetSimulationValues({{"GetUnitLifePercent(udg_Unit)", "50"}});
    QVERIFY(qAbs(barWidth() - 0.1f) < 1e-4f);
    // 输入集合不变：编辑器保留，不重建。
    QWidget* editor = inputs->cellWidget(0, 1);
    module.SetSimulationValues({{"GetUnitLifePercent(udg_Unit)", "25"}});
    QCOMPARE(inputs->cellWidget(0, 1), editor);
    QVERIFY(qAbs(barWidth() - 0.05f) < 1e-4f);

    QString error;
    QVERIFY(!module.SetSimulation(QStringLiteral("NoSuchFunction"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(module.SimulationFunction().isEmpty());
    QCOMPARE(box->currentIndex(), 0);
    QVERIFY(qAbs(barWidth() - 0.2f) < 1e-4f);
}

void GameUiModuleTests::DescriptionPreviewDrivesFrames() {
    // 原创界面描述：底框随“显示”开关，填充随进度；“充满”状态换贴图并填满。
    auto parsed = ParseUiDescription(R"({"format": "audastra.ui", "version": 1, "id": "Bar",
  "values": [{"id": "progress", "type": "real", "min": 0, "max": 1, "default": 0.5}, {"id": "shown", "type": "boolean", "default": true}],
  "frames": [{"id": "back", "type": "backdrop", "anchors": [{"point": "CENTER", "to": "screen"}], "width": 0.2, "height": 0.02,
    "texture": "Back", "visible": "shown",
    "children": [{"id": "fill", "type": "bar", "anchors": [{"point": "TOPLEFT"}, {"point": "BOTTOMRIGHT"}], "texture": "Fill", "value": "progress"}]}],
  "states": [{"id": "full", "label": "充满", "set": [{"frame": "fill", "texture": "Full", "value": 1}]}]})");
    QVERIFY(parsed.value);
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    auto definitions = SyntheticDefinitions();
    definitions.descriptions.push_back({"AudastraUI\\Bar.json", std::move(*parsed.value)});
    module.LoadDefinitionsForTesting(std::move(definitions));
    QCOMPARE(module.Descriptions().size(), std::size_t{1});
    const auto node = [&module](const char* name) -> const FrameNode& {
        return module.Tree()->Nodes()[*module.Tree()->FindByName(name)];
    };
    const auto commands = [&module, &node](const char* name) {
        const std::uint32_t id = node(name).id;
        return std::count_if(module.DrawList().commands.begin(), module.DrawList().commands.end(),
            [id](const FrameDrawCommand& command) { return command.node == id; });
    };
    QVERIFY(qAbs(*node("fill").fill - 0.5f) < 1e-6f);

    auto* panel = page->findChild<QWidget*>(QStringLiteral("gameUiDescription"));
    QVERIFY(panel);
    QVERIFY(module.SelectFrame(QStringLiteral("fill")));
    QVERIFY(!panel->isHidden());
    auto* inputs = page->findChild<QTableWidget*>(QStringLiteral("gameUiDescriptionInputs"));
    QVERIFY(inputs);
    QCOMPARE(inputs->rowCount(), 2);
    QWidget* editor = inputs->cellWidget(0, 1);

    QString error;
    QVERIFY(module.SetDescriptionValues(QStringLiteral("Bar"), {{"progress", "0.25"}}, &error));
    QVERIFY(qAbs(*node("fill").fill - 0.25f) < 1e-6f);
    QCOMPARE(inputs->cellWidget(0, 1), editor);
    // 不合法的输入整批拒绝，预览不变。
    QVERIFY(!module.SetDescriptionValues(QStringLiteral("Bar"), {{"progress", "0.9"}, {"shown", "maybe"}}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!module.SetDescriptionValues(QStringLiteral("Bar"), {{"nope", "1"}}, &error));
    QVERIFY(qAbs(*node("fill").fill - 0.25f) < 1e-6f);

    auto* state = page->findChild<QComboBox*>(QStringLiteral("gameUiDescriptionState"));
    QVERIFY(state);
    QCOMPARE(state->count(), 2);
    state->setCurrentIndex(1);
    QCOMPARE(module.DescriptionPreview(QStringLiteral("Bar")).state, std::string("full"));
    QCOMPARE(node("fill").texture->path, std::string("Full.blp"));
    QVERIFY(qAbs(*node("fill").fill - 1.0f) < 1e-6f);
    QVERIFY(!module.SetDescriptionState(QStringLiteral("Bar"), QStringLiteral("missing"), &error));

    // 隐藏：游戏中不显示，打开“显示隐藏”后照画。
    QVERIFY(module.SetDescriptionValues(QStringLiteral("Bar"), {{"shown", "false"}}, &error));
    QVERIFY(node("back").hidden);
    QCOMPARE(commands("back"), 0);
    auto* showHidden = page->findChild<QPushButton*>(QStringLiteral("gameUiShowHidden"));
    QVERIFY(showHidden);
    showHidden->setChecked(true);
    QVERIFY(module.ShowHidden());
    QVERIFY(commands("back") > 0);

    QVERIFY(module.SelectFrame(QStringLiteral("ConsoleUI")));
    QVERIFY(panel->isHidden());
}

void GameUiModuleTests::AuthoringStagesDescriptionImport() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    const auto staged = [&project]() -> std::shared_ptr<const std::vector<std::byte>> {
        for (const auto& entry : project.AssetEntries()) {
            if (entry.path.compare(QStringLiteral("AudastraUI\\Hud.json"), Qt::CaseInsensitive) == 0) return entry.ownedPayload;
        }
        return nullptr;
    };
    const auto frame = [&module](const char* id) { return FindUiFrame(*module.Description(QStringLiteral("Hud")), id); };
    const auto selectedName = [&module] { return module.Tree()->Nodes()[module.SelectedNode()].name; };

    QString error;
    QVERIFY(!module.CreateDescription(QStringLiteral("9bad"), {}, &error));
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), QStringLiteral("状态栏"), &error), qPrintable(error));
    QVERIFY(staged());
    QCOMPARE(selectedName(), std::string("Hud"));
    QVERIFY(!module.CreateDescription(QStringLiteral("Hud"), {}, &error));

    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("bar"), &error), QStringLiteral("bar1"));
    QCOMPARE(selectedName(), std::string("bar1"));
    QVERIFY(module.Tree()->Nodes()[module.SelectedNode()].layout == LayoutState::Placed);

    // 属性表里改宽度：下一轮事件生效。
    auto* properties = page->findChild<QTreeWidget*>(QStringLiteral("gameUiProperties"));
    QVERIFY(properties);
    const auto field = [properties](const QString& label) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(properties); *it; ++it) {
            if ((*it)->text(0) == label && ((*it)->flags() & Qt::ItemIsEditable)) return *it;
        }
        return nullptr;
    };
    QVERIFY(field(QStringLiteral("宽")));
    field(QStringLiteral("宽"))->setText(1, QStringLiteral("0.2"));
    QCoreApplication::processEvents();
    QCOMPARE(frame("bar1")->width, std::optional<double>(0.2));
    field(QStringLiteral("宽"))->setText(1, QStringLiteral("wide"));
    QCoreApplication::processEvents();
    QCOMPARE(frame("bar1")->width, std::optional<double>(0.2));
    auto* problems = page->findChild<QStandardItemModel*>(QStringLiteral("gameUiProblemModel"));
    QVERIFY(HasProblem(problems, QStringLiteral("AUI_AUTHORING")));

    const float left = module.Tree()->Nodes()[module.SelectedNode()].rect.left;
    QVERIFY2(module.MoveDescriptionFrame(module.SelectedNode(), 0.05, -0.02, &error), qPrintable(error));
    QCOMPARE(frame("bar1")->anchors[0].x, 0.05);
    QCOMPARE(frame("bar1")->anchors[0].y, -0.02);
    QVERIFY(qAbs(module.Tree()->Nodes()[module.SelectedNode()].rect.left - left - 0.05f) < 1e-5f);

    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("text"), &error), QStringLiteral("text1"));
    QCOMPARE(frame("bar1")->children.size(), std::size_t{1});
    QVERIFY(module.RemoveSelectedDescriptionFrame(&error));
    QVERIFY(!frame("text1"));
    QCOMPARE(selectedName(), std::string("Hud"));

    // 不合法的编辑什么都不改。
    const auto before = *staged();
    QVERIFY(!module.EditDescription(QStringLiteral("Hud"),
        QStringLiteral(R"([{"op":"set_frame","frame":"bar1","fields":{"value":"missing"}}])"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("missing")));
    QCOMPARE(*staged(), before);

    // 放进地图的是规范文本；撤销栈里每次修改一步。
    const std::string text = SerializeUiDescription(*module.Description(QStringLiteral("Hud")));
    QCOMPARE(std::string(reinterpret_cast<const char*>(before.data()), before.size()), text);
    QVERIFY(project.AssetUndoStack());
    QCOMPARE(project.AssetUndoStack()->count(), 6);
    project.AssetUndoStack()->undo();
    QVERIFY(staged());
    QVERIFY(*staged() != before);
}

void GameUiModuleTests::DescriptionDataTablesEdit() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Cast"), {}, &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("bar"), &error), QStringLiteral("bar1"));
    const auto description = [&module] { return module.Description(QStringLiteral("Cast")); };
    const auto click = [page](const char* name) {
        auto* button = page->findChild<QPushButton*>(QLatin1String(name));
        QVERIFY(button);
        button->click();
        QCoreApplication::processEvents();
    };
    // 选择列：打开单元的编辑器，像作者一样从列表里选。
    const auto choose = [](QTableWidget* table, int row, int column, const QString& value) {
        table->setCurrentCell(row, column);
        table->editItem(table->item(row, column));
        auto* box = table->viewport()->findChild<QComboBox*>();
        QVERIFY(box);
        const int index = box->findData(value);
        QVERIFY(index >= 0);
        box->setCurrentIndex(index);
        emit box->activated(index);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // 关掉的编辑器
    };
    auto* values = page->findChild<QTableWidget*>(QStringLiteral("gameUiValueTable"));
    auto* states = page->findChild<QTableWidget*>(QStringLiteral("gameUiStateTable"));
    auto* overrides = page->findChild<QTableWidget*>(QStringLiteral("gameUiOverrideTable"));
    auto* events = page->findChild<QTableWidget*>(QStringLiteral("gameUiEventTable"));
    QVERIFY(values && states && overrides && events);

    // 数值：添加、改名（框架取值跟着改）、改默认值、改类型。
    click("gameUiAddValue");
    QCOMPARE(description()->values.size(), std::size_t{1});
    QCOMPARE(values->item(0, 0)->text(), QStringLiteral("value1"));
    QVERIFY2(module.EditDescription(QStringLiteral("Cast"),
                 QStringLiteral(R"([{"op":"set_frame","frame":"bar1","fields":{"value":"value1"}}])"), {}, &error),
        qPrintable(error));
    values->item(0, 0)->setText(QStringLiteral("progress"));
    QCoreApplication::processEvents();
    QCOMPARE(description()->values[0].id, std::string("progress"));
    QCOMPARE(FindUiFrame(*description(), "bar1")->value.binding->value, std::string("progress"));
    values->item(0, 4)->setText(QStringLiteral("2.6"));
    QCoreApplication::processEvents();
    QCOMPARE(description()->values[0].defaultValue, std::string("2.6"));
    choose(values, 0, 2, QStringLiteral("integer"));
    QVERIFY(description()->values[0].type == UiValueType::Integer);
    QCOMPARE(description()->values[0].defaultValue, std::string("3"));
    QCOMPARE(values->item(0, 2)->text(), QStringLiteral("整数"));
    // 布尔默认值是单元自带的勾选。
    click("gameUiAddValue");
    choose(values, 1, 2, QStringLiteral("boolean"));
    QVERIFY(description()->values[1].type == UiValueType::Boolean);
    values->item(1, 4)->setCheckState(Qt::Checked);
    QCoreApplication::processEvents();
    QCOMPARE(description()->values[1].defaultValue, std::string("true"));
    // 不是数值：报告并恢复表格。
    values->item(0, 5)->setText(QStringLiteral("low"));
    QCoreApplication::processEvents();
    QVERIFY(!description()->values[0].min);
    QCOMPARE(values->item(0, 5)->text(), QString());
    QVERIFY(HasProblem(page->findChild<QStandardItemModel*>(QStringLiteral("gameUiProblemModel")), QStringLiteral("AUI_AUTHORING")));

    // 状态：新状态直接预览；覆盖跟着所选框架；改名后预览跟着走。
    click("gameUiAddState");
    QCOMPARE(description()->states.size(), std::size_t{1});
    QCOMPARE(module.DescriptionPreview(QStringLiteral("Cast")).state, std::string("state1"));
    QVERIFY(module.SelectFrame(QStringLiteral("bar1")));
    click("gameUiAddOverride");
    QCOMPARE(description()->states[0].overrides.size(), std::size_t{1});
    QCOMPARE(description()->states[0].overrides[0].frame, std::string("bar1"));
    overrides->item(0, 4)->setText(QStringLiteral("0.5"));
    QCoreApplication::processEvents();
    QCOMPARE(description()->states[0].overrides[0].value, std::optional<double>(0.5));
    const auto bar = module.Tree()->FindByName("bar1");
    QVERIFY(bar && module.Tree()->Nodes()[*bar].fill);
    QVERIFY(qAbs(*module.Tree()->Nodes()[*bar].fill - 0.5f) < 1e-5f);
    states->item(0, 0)->setText(QStringLiteral("interrupted"));
    QCoreApplication::processEvents();
    QCOMPARE(description()->states[0].id, std::string("interrupted"));
    QCOMPARE(module.DescriptionPreview(QStringLiteral("Cast")).state, std::string("interrupted"));

    // 事件：按钮默认点击，可改触发与删除。
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("button"), &error), QStringLiteral("button1"));
    click("gameUiAddEvent");
    QCOMPARE(description()->events.size(), std::size_t{1});
    QVERIFY(description()->events[0].frame == "button1" && description()->events[0].kind == UiEventKind::Click);
    choose(events, 0, 2, QStringLiteral("leave"));
    QVERIFY(description()->events[0].kind == UiEventKind::Leave);
    choose(events, 0, 1, QStringLiteral("bar1"));
    QCOMPARE(description()->events[0].frame, std::string("bar1"));
    events->setCurrentCell(0, 0);
    click("gameUiRemoveEvent");
    QVERIFY(description()->events.empty());

    // 删除状态：预览回到基础状态。
    states->setCurrentCell(0, 0);
    click("gameUiRemoveState");
    QVERIFY(description()->states.empty());
    QVERIFY(module.DescriptionPreview(QStringLiteral("Cast")).state.empty());

    // 原始字段：表头与取值换成文件里的写法。
    page->findChild<QCheckBox*>(QStringLiteral("showRawFields"))->setChecked(true);
    QCOMPARE(values->horizontalHeaderItem(0)->text(), QStringLiteral("id"));
    QCOMPARE(values->item(0, 2)->text(), QStringLiteral("integer"));
}

// 一键生成：经共用触发器事务写进 AudastraUI 类别，基础模板放进待保存修改；预览与重复生成不改地图。
void GameUiModuleTests::GeneratesDescriptionTriggers() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    auto definitions = SyntheticDefinitions();
    definitions.gameSkins = "[Default]\nMasterFont=Fonts\\dfst-m3u.ttf\n";
    module.LoadDefinitionsForTesting(std::move(definitions));
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Cast"), QStringLiteral("读条"), &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("bar"), &error), QStringLiteral("bar1"));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("text"), &error), QStringLiteral("text1"));
    QVERIFY2(module.EditDescription(QStringLiteral("Cast"),
                 QStringLiteral(R"([{"op":"set_frame","frame":"text1","fields":{"fontHeight":0.013}}])"), {}, &error),
        qPrintable(error));
    auto* button = page->findChild<QPushButton*>(QStringLiteral("gameUiGenerateTriggers"));
    QVERIFY(button && button->isEnabled());

    // 没有触发器文档时明确拒绝。
    QVERIFY(!module.GenerateDescriptionTriggers(QStringLiteral("Cast"), true, &error));
    QVERIFY(error.contains(QStringLiteral("触发器")));

    AudastraUiTest::Catalog files;
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    const auto assetSteps = project.AssetUndoStack()->count();
    const auto category = [&project]() -> const Trigger::CategoryRecord* {
        const auto& rows = project.triggerDocument_->categories;
        const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.name.text == kUiTriggerCategory; });
        return found == rows.end() ? nullptr : &*found;
    };
    const auto staged = [&project](std::string_view path) {
        const auto entries = project.AssetEntries();
        return std::any_of(entries.begin(), entries.end(),
            [&](const auto& entry) { return entry.path.compare(QString::fromUtf8(path.data(), qsizetype(path.size())), Qt::CaseInsensitive) == 0; });
    };

    const auto preview = module.GenerateDescriptionTriggers(QStringLiteral("Cast"), true, &error);
    QVERIFY2(preview, qPrintable(error));
    QVERIFY(preview->featureText.contains(QStringLiteral("AudastraUISetFont(udg_AUI_Cast_text1, \"Fonts\\\\dfst-m3u.ttf\", 0.013, 0)")));
    QCOMPARE(preview->triggers, (QStringList{QStringLiteral("读条 创建 Cast_Create"), QStringLiteral("读条 更新 Cast_Update")}));
    QVERIFY(!category() && project.triggerUndoStack_->count() == 0 && project.AssetUndoStack()->count() == assetSteps);

    const auto applied = module.GenerateDescriptionTriggers(QStringLiteral("Cast"), false, &error);
    QVERIFY2(applied, qPrintable(error));
    QVERIFY(category());
    const auto categoryId = category()->categoryId;
    QCOMPARE(std::count_if(project.triggerDocument_->triggers.begin(), project.triggerDocument_->triggers.end(),
                 [&](const auto& row) { return row.categoryId == categoryId; }), 2);
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QCOMPARE(applied->templateFiles.size(), qsizetype{2});
    QVERIFY(staged(kUiTemplateTocMember) && staged(kUiTemplateFdfMember));
    QCOMPARE(project.AssetUndoStack()->count(), assetSteps + 1);

    // 描述没变时再生成：触发器与模板都已相同，不再加撤销步骤。
    const auto again = module.GenerateDescriptionTriggers(QStringLiteral("Cast"), false, &error);
    QVERIFY2(again, qPrintable(error));
    QVERIFY(again->templateFiles.isEmpty());
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QCOMPARE(project.AssetUndoStack()->count(), assetSteps + 1);

    // 触发器那一步可以撤销。
    project.triggerUndoStack_->undo();
    QVERIFY(!category());

    // 锚在游戏代码区域：按固定位置换成屏幕坐标，结果带警告；预览什么都不改。
    QVERIFY2(module.EditDescription(QStringLiteral("Cast"),
                 QStringLiteral(R"([{"op":"set_frame","frame":"bar1","fields":{"anchors":[{"point":"center","to":"game:Minimap","toPoint":"center"}]}}])"),
                 {}, &error),
        qPrintable(error));
    const auto fixed = module.GenerateDescriptionTriggers(QStringLiteral("Cast"), true, &error);
    QVERIFY2(fixed, qPrintable(error));
    QVERIFY(std::any_of(fixed->warnings.begin(), fixed->warnings.end(), [](const auto& warning) {
        return warning.code == "AUI_TRIGGER_AREA_FIXED" && warning.message.find("Minimap") != std::string::npos;
    }));
    QVERIFY(fixed->featureText.contains(QStringLiteral("AudastraUISetAbsPoint(udg_AUI_Cast_bar1, AudastraAnchorCenter, ")));
    QVERIFY(!fixed->featureText.contains(QStringLiteral("Minimap")));
    QVERIFY(!category() && project.triggerUndoStack_->index() == 0);
}

// 从文件导入示例：描述放进待保存修改；选了一并写入时先生成界面自己的变量和触发器，同目录 triggers.txt
// 随后写进以显示名命名的类别（可以引用生成的变量），一步撤销；触发器写不进时描述也不放入。
void GameUiModuleTests::ImportsDescriptionFile() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(folder.filePath(name)).path());
        QFile file(folder.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto description = [](const char* id) {
        auto created = NewUiDescription(id, "状态栏");
        return created.value ? QByteArray::fromStdString(SerializeUiDescription(*created.value)) : QByteArray();
    };
    QVERIFY(write(QStringLiteral("hud/hud.json"), description("Hud")));
    QVERIFY(write(QStringLiteral("hud/triggers.txt"), QByteArrayLiteral(
        "变量:\n    AUH_Count: integer = 0\n触发器 状态栏 计数 Hud_Count:\n    events:\n        TriggerRegisterTimerEventPeriodic(1)\n"
        "    actions:\n        SetVariable(udg_AUH_Count, 1)\n        SetVariable(udg_AUIC_Hud, false)\n")));
    QVERIFY(write(QStringLiteral("bad/bad.json"), QByteArrayLiteral("{}")));
    QVERIFY(write(QStringLiteral("broken/other.json"), description("Other")));
    QVERIFY(write(QStringLiteral("broken/triggers.txt"), QByteArrayLiteral(
        "触发器 状态栏 坏 Other_Bad:\n    events:\n    actions:\n        NoSuchAction()\n")));

    QtProject::EditorProject plain;
    Modules::GameUiModule only(plain);
    only.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY(!only.ImportDescriptionFile(folder.filePath(QStringLiteral("bad/bad.json")), false, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY2(only.ImportDescriptionFile(folder.filePath(QStringLiteral("hud/hud.json")), false, &error), qPrintable(error));
    QVERIFY(only.Description(QStringLiteral("Hud")));
    QVERIFY(!only.ImportDescriptionFile(folder.filePath(QStringLiteral("hud/hud.json")), false, &error));
    // 没有触发器文档时不能一并写入。
    QVERIFY(!only.ImportDescriptionFile(folder.filePath(QStringLiteral("broken/other.json")), true, &error));
    QVERIFY(!only.Description(QStringLiteral("Other")));

    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    AudastraUiTest::Catalog files;
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    QVERIFY2(module.ImportDescriptionFile(folder.filePath(QStringLiteral("hud/hud.json")), true, &error), qPrintable(error));
    QVERIFY(module.Description(QStringLiteral("Hud")));
    const auto& categories = project.triggerDocument_->categories;
    const auto category = std::find_if(categories.begin(), categories.end(), [](const auto& row) { return row.name.text == "状态栏"; });
    QVERIFY(category != categories.end());
    const auto categoryId = category->categoryId;
    const auto& imported = project.triggerDocument_->triggers;
    QVERIFY(std::any_of(imported.begin(), imported.end(), [&](const auto& row) {
        return row.categoryId == categoryId && row.name.text == "状态栏 计数 Hud_Count";
    }));
    const auto& variables = project.triggerDocument_->variables;
    QVERIFY(std::any_of(variables.begin(), variables.end(), [](const auto& row) { return row.name.text == "AUH_Count"; }));
    QVERIFY(std::any_of(variables.begin(), variables.end(), [](const auto& row) { return row.name.text == "AUIC_Hud"; }));
    QVERIFY(std::any_of(imported.begin(), imported.end(), [](const auto& row) { return row.name.text == "状态栏 创建 Hud_Create"; }));
    QCOMPARE(project.triggerUndoStack_->count(), 1);

    QVERIFY(!module.ImportDescriptionFile(folder.filePath(QStringLiteral("broken/other.json")), true, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!module.Description(QStringLiteral("Other")));
    QCOMPARE(project.triggerUndoStack_->count(), 1);

    // 事件的响应触发器：事件名已填好；再建一次直接用已有的，不覆盖。
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("button"), &error), QStringLiteral("button1"));
    QVERIFY2(module.EditDescription(QStringLiteral("Hud"),
                 QStringLiteral(R"([{"op":"set_events","events":[{"id":"open","frame":"button1","on":"click"}]}])"), {}, &error),
        qPrintable(error));
    QVERIFY(!module.CreateEventTrigger(QStringLiteral("Hud"), QStringLiteral("missing"), &error));
    const auto handler = module.CreateEventTrigger(QStringLiteral("Hud"), QStringLiteral("open"), &error);
    QVERIFY2(handler, qPrintable(error));
    const auto& rows = project.triggerDocument_->triggers;
    const auto trigger = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == *handler; });
    QVERIFY(trigger != rows.end());
    QCOMPARE(trigger->name.text, std::string("状态栏 事件 open Hud_On_open"));
    QVERIFY(trigger->categoryId == categoryId);
    const auto exported = Trigger::RenderTriggerText(*project.triggerDocument_, *project.uiCatalog_, *handler, 1 << 20, nullptr, nullptr);
    QVERIFY(exported.Succeeded());
    QVERIFY2(exported.value->find("AudastraUITriggerRegisterEvent(\"Hud.open\")") != std::string::npos, exported.value->c_str());
    const auto steps = project.triggerUndoStack_->count();
    QCOMPARE(module.CreateEventTrigger(QStringLiteral("Hud"), QStringLiteral("open"), &error), handler);
    QCOMPARE(project.triggerUndoStack_->count(), steps);

    // 模拟计时器的触发器同样填好界面与计时器编号。
    QVERIFY2(module.EditDescription(QStringLiteral("Hud"),
                 QStringLiteral(R"([{"op":"set_simulation","simulation":{"timers":[{"id":"tick","period":1}]}}])"), {}, &error),
        qPrintable(error));
    QVERIFY(!module.CreateSimulationTrigger(QStringLiteral("Hud"), QStringLiteral("missing"), &error));
    const auto ticker = module.CreateSimulationTrigger(QStringLiteral("Hud"), QStringLiteral("tick"), &error);
    QVERIFY2(ticker, qPrintable(error));
    const auto simulated = Trigger::RenderTriggerText(*project.triggerDocument_, *project.uiCatalog_, *ticker, 1 << 20, nullptr, nullptr);
    QVERIFY(simulated.Succeeded());
    QVERIFY2(simulated.value->find("AudastraUIRegisterSimulationTimer(\"Hud\", \"tick\")") != std::string::npos, simulated.value->c_str());
    QCOMPARE(project.triggerUndoStack_->count(), steps + 1);
}

void GameUiModuleTests::FramePropertiesEditLayoutAndText() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), {}, &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("backdrop"), &error), QStringLiteral("back1"));
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("text"), &error), QStringLiteral("text1"));
    const auto frame = [&module](const char* id) { return FindUiFrame(*module.Description(QStringLiteral("Hud")), id); };
    auto* properties = page->findChild<QTreeWidget*>(QStringLiteral("gameUiProperties"));
    QVERIFY(properties);
    const auto field = [properties](const QString& label, const QString& group = {}) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(properties); *it; ++it) {
            if ((*it)->text(0) != label || !((*it)->flags() & Qt::ItemIsEditable)) continue;
            if (group.isEmpty() || ((*it)->parent() && (*it)->parent()->text(0) == group)) return *it;
        }
        return nullptr;
    };
    const auto edit = [](QTreeWidgetItem* item, const QString& text) {
        QVERIFY(item);
        item->setText(1, text);
        QCoreApplication::processEvents();
    };
    // 选择类字段：像作者一样打开编辑器、从列表里选。
    const auto open = [properties](QTreeWidgetItem* item) -> QComboBox* {
        if (!item) return nullptr;
        properties->setCurrentItem(item, 1);
        properties->editItem(item, 1);
        return properties->viewport()->findChild<QComboBox*>();
    };
    const auto choose = [&](QTreeWidgetItem* item, const QString& value) {
        QComboBox* box = open(item);
        QVERIFY(box);
        const int index = box->findData(value);
        QVERIFY2(index >= 0, qPrintable(value));
        box->setCurrentIndex(index);
        emit box->activated(index);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    const auto steps = [&project] { return project.AssetUndoStack()->count(); };

    // 文字：对齐、颜色、字体。
    QVERIFY(field(QStringLiteral("水平对齐")));
    QCOMPARE(field(QStringLiteral("水平对齐"))->text(1), QStringLiteral("居中"));
    choose(field(QStringLiteral("水平对齐")), QStringLiteral("left"));
    QVERIFY(frame("text1")->justifyH == JustifyH::Left);
    QCOMPARE(field(QStringLiteral("水平对齐"))->text(1), QStringLiteral("左"));
    const auto before = steps();
    choose(field(QStringLiteral("水平对齐")), QStringLiteral("left"));  // 选回同一项不产生修改
    QCOMPARE(steps(), before);
    choose(field(QStringLiteral("竖直对齐")), QStringLiteral("bottom"));
    QVERIFY(frame("text1")->justifyV == JustifyV::Bottom);
    // 颜色用取色器选（双击或右键），选定的值与手写走同一检查；颜色行不可直接改，按与“字体”同组查找。
    const auto color = [&field]() -> QTreeWidgetItem* {
        QTreeWidgetItem* font = field(QStringLiteral("字体"));
        if (!font || !font->parent()) return nullptr;
        for (int i = 0; i < font->parent()->childCount(); ++i) {
            if (font->parent()->child(i)->text(0) == QStringLiteral("颜色")) return font->parent()->child(i);
        }
        return nullptr;
    };
    QVERIFY(color() && !(color()->flags() & Qt::ItemIsEditable));
    edit(color(), QStringLiteral("#80FF0000"));
    QCOMPARE(frame("text1")->color, (std::array<double, 4>{1.0, 0.0, 0.0, 0.502}));
    QCOMPARE(color()->text(1), QStringLiteral("#80FF0000"));
    edit(color(), QStringLiteral("red"));
    QCOMPARE(frame("text1")->color[3], 0.502);
    edit(color(), QString());
    QCOMPARE(frame("text1")->color, (std::array<double, 4>{1.0, 1.0, 1.0, 1.0}));
    edit(field(QStringLiteral("字体")), QStringLiteral("Fonts\\FRIZQT__.TTF"));
    QCOMPARE(frame("text1")->font, std::string("Fonts\\FRIZQT__.TTF"));

    // 锚点：第一个锚点直接列在“位置与大小”里；目标只列自身以外的控件；改对齐点时目标点保持不变。
    const QString place = QStringLiteral("位置与大小");
    QComboBox* targets = open(field(QStringLiteral("对齐到"), place));
    QVERIFY(targets);
    QVERIFY(targets->findData(QStringLiteral("back1")) >= 0);
    QVERIFY(targets->findData(QStringLiteral("text1")) < 0);
    QVERIFY(targets->findData(QStringLiteral("game:Minimap")) >= 0);
    emit properties->itemDelegateForColumn(1)->closeEditor(targets);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    choose(field(QStringLiteral("对齐到"), place), QStringLiteral("back1"));
    choose(field(QStringLiteral("对齐点"), place), QStringLiteral("TOPLEFT"));
    QCOMPARE(frame("text1")->anchors.size(), std::size_t{1});
    QVERIFY(frame("text1")->anchors[0].target == UiAnchorTarget::Frame);
    QCOMPARE(frame("text1")->anchors[0].targetId, std::string("back1"));
    QVERIFY(frame("text1")->anchors[0].point == FramePoint::TopLeft);
    QVERIFY(frame("text1")->anchors[0].targetPoint == FramePoint::Center);
    choose(field(QStringLiteral("目标点"), place), QStringLiteral("BOTTOMLEFT"));
    QVERIFY(frame("text1")->anchors[0].targetPoint == FramePoint::BottomLeft);
    edit(field(QStringLiteral("X"), place), QStringLiteral("0.01"));
    QCOMPARE(frame("text1")->anchors[0].x, 0.01);
    choose(field(QStringLiteral("对齐到"), place), QStringLiteral("parent"));
    QVERIFY(frame("text1")->anchors[0].target == UiAnchorTarget::Parent);

    QVERIFY2(module.AddDescriptionAnchor(&error), qPrintable(error));
    QCOMPARE(frame("text1")->anchors.size(), std::size_t{2});
    QVERIFY(frame("text1")->anchors[1].point == FramePoint::Center);
    QVERIFY(field(QStringLiteral("对齐点"), QStringLiteral("锚点 2")));
    QVERIFY2(module.RemoveDescriptionAnchor(0, &error), qPrintable(error));
    QCOMPARE(frame("text1")->anchors.size(), std::size_t{1});
    QVERIFY(frame("text1")->anchors[0].point == FramePoint::Center);
    QVERIFY2(module.RemoveDescriptionAnchor(0, &error), qPrintable(error));
    QVERIFY(frame("text1")->anchors.empty());
    QVERIFY(!module.RemoveDescriptionAnchor(0, &error));
    // 没有锚点时“对齐点”一行新建第一个锚点。
    QCOMPARE(field(QStringLiteral("对齐点"), place)->text(1), QStringLiteral("（未定位）"));
    choose(field(QStringLiteral("对齐点"), place), QStringLiteral("CENTER"));
    QCOMPARE(frame("text1")->anchors.size(), std::size_t{1});

    // 阵列：填数量才有间距，清空数量即只有一个。
    QVERIFY(field(QStringLiteral("数量"), QStringLiteral("阵列")));
    QVERIFY(!field(QStringLiteral("横向间距")));
    edit(field(QStringLiteral("数量")), QStringLiteral("3"));
    QVERIFY(frame("text1")->repeat && frame("text1")->repeat->count == 3);
    edit(field(QStringLiteral("横向间距")), QStringLiteral("0.02"));
    QCOMPARE(frame("text1")->repeat->dx, 0.02);
    edit(field(QStringLiteral("数量")), QStringLiteral("0"));
    QCOMPARE(frame("text1")->repeat->count, 3u);
    edit(field(QStringLiteral("数量")), QString());
    QVERIFY(!frame("text1")->repeat);

    // 模板框架：从游戏界面文件的模板添加，模板名可从列表改。
    const QStringList templates = module.FrameTemplates();
    QCOMPARE(templates.size(), qsizetype{2});
    QVERIFY(templates.contains(QStringLiteral("ConsoleUI")) && templates.contains(QStringLiteral("Box")));
    QVERIFY(module.AddDescriptionTemplate({}, &error).isEmpty());
    QCOMPARE(module.AddDescriptionTemplate(QStringLiteral("Box"), &error), QStringLiteral("template1"));
    QCOMPARE(frame("template1")->templateName, std::string("Box"));
    QVERIFY(field(QStringLiteral("模板")));
    choose(field(QStringLiteral("模板")), QStringLiteral("ConsoleUI"));
    QCOMPARE(frame("template1")->templateName, std::string("ConsoleUI"));
}

void GameUiModuleTests::KindPickerCreatesTypedFrames() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    QVERIFY(module.Widget());
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    // 还没有界面时也能打开，放进“新界面 ui1”。
    {
        QPointer<Modules::GameUiKindPicker> first =
            qobject_cast<Modules::GameUiKindPicker*>(module.OpenKindPicker(kNoNode, std::nullopt, QPoint(10, 10)).data());
        QVERIFY(first);
        auto* target = first->findChild<QComboBox*>(QStringLiteral("gameUiKindTarget"));
        QVERIFY(target && target->currentText().contains(QStringLiteral("ui1")));
        first->close();
    }
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), {}, &error), qPrintable(error));
    const auto description = [&module] { return module.Description(QStringLiteral("Hud")); };
    const auto text = [](std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); };

    // 每种类型的默认框架都能放进描述并排得出位置。
    for (const auto& kind : UiFrameKinds()) {
        const QString templateName = kind.type == UiFrameType::Template ? QStringLiteral("Box") : QString();
        const QString created = module.CreateDescriptionFrame(text(kind.id), QStringLiteral("Hud"), {}, std::nullopt, templateName, &error);
        QVERIFY2(!created.isEmpty(), qPrintable(text(kind.id) + QStringLiteral("：") + error));
        const LayoutState layout = module.Tree()->Nodes()[module.SelectedNode()].layout;
        QVERIFY2(layout == (kind.type == UiFrameType::Text ? LayoutState::SizeFromContent : LayoutState::Placed), qPrintable(text(kind.id)));
    }
    QCOMPARE(description()->frames.size(), UiFrameKinds().size());
    QVERIFY(module.CreateDescriptionFrame(QStringLiteral("nope"), QStringLiteral("Hud"), {}, std::nullopt, {}, &error).isEmpty());
    QVERIFY(module.CreateDescriptionFrame(QStringLiteral("template"), QStringLiteral("Hud"), {}, std::nullopt, {}, &error).isEmpty());

    // 右键位置：新框架中心落在所点处（顶层相对屏幕中心，嵌套相对父框架中心）。
    QCOMPARE(module.CreateDescriptionFrame(QStringLiteral("icon"), QStringLiteral("Hud"), {}, QPointF(0.1, 0.5), {}, &error),
        QStringLiteral("icon2"));
    const auto& iconNode = module.Tree()->Nodes()[module.SelectedNode()];
    QVERIFY(qAbs((iconNode.rect.left + iconNode.rect.right) / 2.0 - 0.1) < 1e-4);
    QVERIFY(qAbs((iconNode.rect.top + iconNode.rect.bottom) / 2.0 - 0.5) < 1e-4);
    QCOMPARE(module.CreateDescriptionFrame(QStringLiteral("text"), QStringLiteral("Hud"), QStringLiteral("panel1"), QPointF(0.45, 0.32), {},
                 &error),
        QStringLiteral("text2"));
    QCOMPARE(FindUiFrame(*description(), "panel1")->children.size(), std::size_t{1});
    QCOMPARE(FindUiFrame(*description(), "text2")->anchors[0].x, 0.05);
    QCOMPARE(FindUiFrame(*description(), "text2")->anchors[0].y, 0.02);

    // 类型选取：默认放进所点节点；作用与用法取自类型表；预览放大到所选类型；创建后放进目标框架。
    QVERIFY(module.SelectFrame(QStringLiteral("bar1")));
    QPointer<Modules::GameUiKindPicker> picker =
        qobject_cast<Modules::GameUiKindPicker*>(module.OpenKindPicker(module.SelectedNode(), std::nullopt, QPoint(10, 10)).data());
    QVERIFY(picker);
    auto* target = picker->findChild<QComboBox*>(QStringLiteral("gameUiKindTarget"));
    QVERIFY(target && target->currentText().contains(QStringLiteral("bar1")));
    QVERIFY(picker->SelectKind(QStringLiteral("button")));
    QCOMPARE(picker->findChild<QLabel*>(QStringLiteral("gameUiKindPurpose"))->text(), text(FindUiFrameKind("button")->purpose));
    QCOMPARE(picker->findChild<QLabel*>(QStringLiteral("gameUiKindUsage"))->text(), text(FindUiFrameKind("button")->usage));
    auto* preview = picker->findChild<Modules::GameUiCanvas*>(QStringLiteral("gameUiKindPreview"));
    QVERIFY(preview && preview->ScreenRect().width() > preview->width());
    picker->Accept();
    QCOMPARE(FindUiFrame(*description(), "bar1")->children.size(), std::size_t{1});
    QCOMPARE(FindUiFrame(*description(), "bar1")->children[0].type, UiFrameType::Button);

    // 模板类型改为搜索模板；没选中模板时不能创建。
    picker = qobject_cast<Modules::GameUiKindPicker*>(module.OpenKindPicker(kNoNode, std::nullopt, QPoint(10, 10)).data());
    QVERIFY(picker && picker->SelectKind(QStringLiteral("template")));
    auto* create = picker->findChild<QPushButton*>(QStringLiteral("gameUiKindCreate"));
    auto* search = picker->findChild<QLineEdit*>(QStringLiteral("gameUiKindTemplateSearch"));
    QVERIFY(create && search && search->isVisibleTo(picker));
    search->setText(QStringLiteral("nothing"));
    QVERIFY(!create->isEnabled());
    search->setText(QStringLiteral("Box"));
    QVERIFY(create->isEnabled());
    picker->Accept();
    QVERIFY(FindUiFrame(*description(), "template2"));
    QCOMPARE(FindUiFrame(*description(), "template2")->templateName, std::string("Box"));

    // 助手读到同一份类型表；默认框架可直接作为 add_frame 写入。
    Modules::GameUiToolService service([&module] { return &module; });
    const QJsonArray kinds = service.Invoke(QStringLiteral("game_ui_view_read"), {}).value("frame_kinds").toArray();
    QCOMPARE(kinds.size(), static_cast<qsizetype>(UiFrameKinds().size()));
    QCOMPARE(kinds[0].toObject().value("kind").toString(), QStringLiteral("panel"));
    QJsonObject bar;
    for (const auto& kind : kinds) {
        if (kind.toObject().value("kind").toString() == QLatin1String("bar")) bar = kind.toObject().value("frame").toObject();
    }
    QVERIFY(bar.contains(QStringLiteral("background")));
    bar.insert(QStringLiteral("id"), QStringLiteral("hp"));
    const QJsonArray edits{QJsonObject{{"op", "add_frame"}, {"frame", bar}}};
    QVERIFY2(module.EditDescription(QStringLiteral("Hud"), QString::fromUtf8(QJsonDocument(edits).toJson(QJsonDocument::Compact)), {}, &error),
        qPrintable(error));
    QVERIFY(FindUiFrame(*description(), "hp"));
}

void GameUiModuleTests::FrameSettingsCardAndTexturePicker() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), {}, &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("bar"), &error), QStringLiteral("bar1"));
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("text"), &error), QStringLiteral("text1"));
    const auto frame = [&module](const char* id) { return FindUiFrame(*module.Description(QStringLiteral("Hud")), id); };
    const auto field = [](QTreeWidget* tree, const QString& label) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->text(0) == label && (*it)->data(1, Qt::UserRole + 40).isValid()) return *it;
        }
        return nullptr;
    };

    // 设置卡片：只对描述框架打开；与属性表同一套字段，改动走同一修改。
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QVERIFY(!module.OpenFrameSettings(module.SelectedNode(), QPoint(10, 10)));
    QVERIFY(module.SelectFrame(QStringLiteral("bar1")));
    QPointer<QWidget> card = module.OpenFrameSettings(module.SelectedNode(), QPoint(10, 10));
    QVERIFY(card && card->isVisible());
    auto* fields = card->findChild<QTreeWidget*>(QStringLiteral("gameUiFrameSettingsFields"));
    QVERIFY(fields && field(fields, QStringLiteral("底图")));
    QVERIFY(card->findChild<QLabel*>(QStringLiteral("gameUiFrameSettingsTitle"))->text().contains(QStringLiteral("进度条")));
    field(fields, QStringLiteral("宽"))->setText(1, QStringLiteral("0.25"));
    QCoreApplication::processEvents();
    QCOMPARE(frame("bar1")->width, std::optional<double>(0.25));
    // 没有资源选择器时贴图照旧手写。
    QVERIFY(field(fields, QStringLiteral("贴图"))->flags() & Qt::ItemIsEditable);
    // 卡片跟随选择：换选另一个描述框架时显示它，选中描述根节点时关闭。
    QVERIFY(module.SelectFrame(QStringLiteral("text1")));
    QVERIFY(card && card->findChild<QLabel*>(QStringLiteral("gameUiFrameSettingsTitle"))->text().contains(QStringLiteral("text1")));
    QVERIFY(!field(fields, QStringLiteral("贴图")));
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!card);

    // 有资源选择器时：贴图双击打开选择器，应用后作为一次修改写入。
    module.SetResourcePickerFactory([] { return Modules::ObjectResourcePickerServices{}; });
    QVERIFY(module.SelectFrame(QStringLiteral("bar1")));
    auto* properties = page->findChild<QTreeWidget*>(QStringLiteral("gameUiProperties"));
    QTreeWidgetItem* texture = field(properties, QStringLiteral("贴图"));
    QVERIFY(texture && !(texture->flags() & Qt::ItemIsEditable));
    page->resize(1200, 800);
    page->show();
    properties->scrollToItem(texture);
    emit properties->itemDoubleClicked(texture, 1);
    auto* picker = page->findChild<Modules::ObjectResourcePicker*>(QStringLiteral("gameUiTexturePicker"));
    QVERIFY(picker && picker->isVisible());
    // 与物编字段同一套表格浮层：放在屏幕工作区内，Esc 关闭，可以再打开。
    QCoreApplication::processEvents();
    QVERIFY(properties->screen()->availableGeometry().contains(picker->geometry()));
    // 显示后按内容长高（如模型预览）也重新放进工作区，底部按钮不越出屏幕。
    picker->setMinimumHeight(std::min(picker->height() + 160, properties->screen()->availableGeometry().height()));
    QTRY_VERIFY(properties->screen()->availableGeometry().contains(picker->geometry()));
    QTest::keyClick(properties, Qt::Key_Escape);
    QVERIFY(!picker->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    emit properties->itemDoubleClicked(texture, 1);
    picker = page->findChild<Modules::ObjectResourcePicker*>(QStringLiteral("gameUiTexturePicker"));
    QVERIFY(picker && picker->isVisible());
    auto* path = picker->findChild<QLineEdit*>(QStringLiteral("resourcePath"));
    QVERIFY(path);
    QCOMPARE(path->text(), QStringLiteral("ReplaceableTextures\\TeamColor\\TeamColor06"));
    path->setText(QStringLiteral("ReplaceableTextures\\TeamColor\\TeamColor00"));
    emit path->returnPressed();
    QCOMPARE(frame("bar1")->texture, std::string("ReplaceableTextures\\TeamColor\\TeamColor00"));

    // 模型框架的“模型”一栏打开选模型的资源选择器。
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("model"), &error), QStringLiteral("model1"));
    QVERIFY(module.SelectFrame(QStringLiteral("model1")));
    QTreeWidgetItem* model = field(properties, QStringLiteral("模型"));
    QVERIFY(model && !(model->flags() & Qt::ItemIsEditable) && !field(properties, QStringLiteral("贴图")));
    emit properties->itemDoubleClicked(model, 1);
    auto* modelPicker = page->findChild<Modules::ObjectResourcePicker*>(QStringLiteral("gameUiModelPicker"));
    QVERIFY(modelPicker && modelPicker->isVisible());
    auto* modelPath = modelPicker->findChild<QLineEdit*>(QStringLiteral("resourcePath"));
    QVERIFY(modelPath);
    modelPath->setText(QStringLiteral("Abilities\\Spells\\Human\\Thunderclap\\ThunderClapCaster.mdx"));
    emit modelPath->returnPressed();
    QCOMPARE(frame("model1")->model, std::string("Abilities\\Spells\\Human\\Thunderclap\\ThunderClapCaster.mdx"));
}

// 界面模拟：作者的触发器订阅模拟计时器，每次到期扣血、低于 0.3 时运行“状态 低”；生成的“更新”“状态”触发器被拦截，
// 数值与状态读回预览。脚本用生产整图生成器从同一份触发器文档生成（模拟工程里的后台生成，换成测试给定的基础库）。
void GameUiModuleTests::TriggerSimulationRunsAuthorTriggers() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hp"), QStringLiteral("血条"), &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("bar"), &error), QStringLiteral("bar1"));
    QVERIFY2(module.EditDescription(QStringLiteral("Hp"), QStringLiteral(R"([
        {"op":"set_values","values":[{"id":"hp","type":"real","min":0,"max":1,"default":1}]},
        {"op":"set_states","states":[{"id":"low","label":"低"}]}])"), {}, &error), qPrintable(error));

    // 没有模拟计时器时明确拒绝。
    QVERIFY(!module.StepTriggerSimulation(QStringLiteral("Hp"), &error));
    QVERIFY(error.contains(QStringLiteral("模拟计时器")));
    QVERIFY2(module.EditDescription(QStringLiteral("Hp"), QStringLiteral(R"([
        {"op":"set_simulation","simulation":{"timers":[{"id":"tick","period":0.5}],"values":{"hp":1}}}])"), {}, &error),
        qPrintable(error));

    AudastraUiTest::Catalog files;
    files.extras = {{"simtest", "", "", "", "[ConditionalTriggerExecute]\n[[.args]]\ntype=trigger\n", ""}};
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    const auto generated = module.GenerateDescriptionTriggers(QStringLiteral("Hp"), false, &error);
    QVERIFY2(generated, qPrintable(error));
    const auto low = std::find_if(generated->triggers.begin(), generated->triggers.end(),
        [](const QString& name) { return name.endsWith(QStringLiteral("Hp_State_low")); });
    QVERIFY(low != generated->triggers.end());

    const std::string author = "触发器 模拟 扣血 Sim_Drain:\n"
        "    events:\n"
        "        AudastraUIRegisterSimulationTimer(\"Hp\", \"tick\")\n"
        "    actions:\n"
        "        SetVariable(udg_AUIV_Hp_hp, OperatorReal(udg_AUIV_Hp_hp, OperatorSubtract, 0.25))\n"
        "        IfThenElseMultiple() {\n"
        "            if:\n"
        "                OperatorCompareReal(0.3, OperatorGreater, udg_AUIV_Hp_hp)\n"
        "            then:\n"
        "                ConditionalTriggerExecute(`" + low->toStdString() + "`)\n"
        "        }\n"
        "        ConditionalTriggerExecute(`血条 更新 Hp_Update`)\n";
    const auto binding = Trigger::CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(binding.Succeeded());
    Trigger::TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(Trigger::GraphApplyFeatureText{"模拟", author, {}, std::nullopt});
    const auto applied = Trigger::ApplyTriggerGraphEditBatch(*project.triggerDocument_, *project.uiCatalog_, batch);
    QVERIFY2(applied.Succeeded(), applied.diagnostics.empty() ? "" : applied.diagnostics.front().message.c_str());

    Script::MapScriptInputs inputs;
    inputs.triggers = project.triggerDocument_.get();
    inputs.catalog = project.uiCatalog_.get();
    const auto map = Script::GenerateMapScript(inputs);
    QVERIFY2(map.Succeeded(), map.diagnostics.empty() ? "" : map.diagnostics.front().message.c_str());
    std::string plain;  // 去掉 YDWE 预处理行（行号不变）
    for (std::size_t start = 0; start < map.text.size();) {
        const std::size_t end = std::min(map.text.find('\n', start), map.text.size());
        const std::string_view line(map.text.data() + start, end - start);
        if (!line.starts_with("#")) plain += line;
        plain += '\n';
        start = end + 1;
    }
    const std::string common = "type agent extends handle\ntype event extends agent\ntype trigger extends agent\n"
        "type triggeraction extends handle\ntype triggercondition extends agent\ntype boolexpr extends agent\n"
        "type conditionfunc extends boolexpr\n"
        "native CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction\n"
        "native TriggerAddCondition takes trigger whichTrigger, boolexpr condition returns triggercondition\n"
        "native Condition takes code func returns conditionfunc\n"
        "native TriggerEvaluate takes trigger whichTrigger returns boolean\n"
        "native TriggerExecute takes trigger whichTrigger returns nothing\n"
        "native TriggerRegisterTimerEvent takes trigger whichTrigger, real timeout, boolean periodic returns event\n";
    const std::string blizzard = "function TriggerRegisterTimerEventPeriodic takes trigger trig, real timeout returns event\n"
        "    return TriggerRegisterTimerEvent(trig, timeout, true)\nendfunction\n"
        "function ConditionalTriggerExecute takes trigger trig returns nothing\n"
        "    if TriggerEvaluate(trig) then\n        call TriggerExecute(trig)\n    endif\nendfunction\n";
    std::vector<MapScriptText> scripts{{"common.j", ScriptLanguage::Jass, common}, {"blizzard.j", ScriptLanguage::Jass, blizzard},
        {"war3map.j", ScriptLanguage::Jass, plain}};
    QVERIFY2(module.PrepareTriggerSimulationForTesting(QStringLiteral("Hp"), scripts, &error), qPrintable(error));

    const auto codes = [&module] {
        QStringList list;
        for (const auto& problem : module.TriggerSimulationProblems()) list.push_back(QString::fromStdString(problem.code));
        return list;
    };
    auto status = module.TriggerSimulationState();
    QVERIFY(status.ready && !status.running && status.description == QStringLiteral("Hp"));
    QVERIFY(std::find(status.subscriptions.begin(), status.subscriptions.end(), std::pair<std::string, std::string>{"Hp", "tick"}) !=
        status.subscriptions.end());
    QVERIFY2(!codes().contains(QStringLiteral("AUI_SIM_NO_SUBSCRIBER")) && !codes().contains(QStringLiteral("AUI_SIM_NOT_GENERATED")),
        qPrintable(codes().join(',')));
    const auto preview = [&module] { return module.DescriptionPreview(QStringLiteral("Hp")); };
    QCOMPARE(QString::fromStdString(preview().values.at("hp")), QStringLiteral("1"));

    QVERIFY2(module.StepTriggerSimulation(QStringLiteral("Hp"), &error), qPrintable(error));
    status = module.TriggerSimulationState();
    QCOMPARE(status.fired, 1u);
    QCOMPARE(status.time, 0.5);
    QCOMPARE(QString::fromStdString(preview().values.at("hp")), QStringLiteral("0.75"));
    QVERIFY(preview().state.empty());
    QVERIFY(module.StepTriggerSimulation(QStringLiteral("Hp")) && module.StepTriggerSimulation(QStringLiteral("Hp")));
    QCOMPARE(QString::fromStdString(preview().values.at("hp")), QStringLiteral("0.25"));
    QCOMPARE(QString::fromStdString(preview().state), QStringLiteral("low"));
    QVERIFY2(std::none_of(module.TriggerSimulationProblems().begin(), module.TriggerSimulationProblems().end(),
                 [](const Diagnostic& problem) { return problem.severity == DiagnosticSeverity::Error; }),
        qPrintable(codes().join(',')));

    // “模拟”页：计时器与订阅数、起始值。
    auto* timers = page->findChild<QTableWidget*>(QStringLiteral("gameUiSimulationTimers"));
    auto* starts = page->findChild<QTableWidget*>(QStringLiteral("gameUiSimulationValues"));
    QVERIFY(timers && starts);
    QCOMPARE(timers->rowCount(), 1);
    QCOMPARE(timers->item(0, 0)->text(), QStringLiteral("tick"));
    QCOMPARE(timers->item(0, 4)->text(), QStringLiteral("1"));
    QCOMPARE(starts->rowCount(), 1);
    QCOMPARE(starts->item(0, 1)->text(), QStringLiteral("1"));
    auto* statusLabel = page->findChild<QLabel*>(QStringLiteral("gameUiSimulationStatus"));
    QVERIFY(statusLabel && statusLabel->text().contains(QStringLiteral("3")));

    // 运行：时钟推进，按周期到期。
    QVERIFY2(module.StartTriggerSimulation(QStringLiteral("Hp"), &error), qPrintable(error));
    QVERIFY(module.TriggerSimulationState().running);
    QTRY_VERIFY_WITH_TIMEOUT(module.TriggerSimulationState().fired >= 4, 5000);
    module.PauseTriggerSimulation();
    QVERIFY(!module.TriggerSimulationState().running);

    // 重置：丢掉脚本，预览回到起始值与基础状态。
    module.ResetTriggerSimulation();
    status = module.TriggerSimulationState();
    QVERIFY(!status.ready && status.description.isEmpty() && status.fired == 0);
    QCOMPARE(QString::fromStdString(preview().values.at("hp")), QStringLiteral("1"));
    QVERIFY(preview().state.empty());
}

void GameUiModuleTests::VisibleSelectableAndPicking() {
    QtProject::EditorProject project;
    project.sourceArchivePath_ = std::filesystem::path(L"C:/Synthetic/ui-pick.w3x");  // Synthetic：可见、可选按这张图记住
    const auto nodeIn = [](const Modules::GameUiModule& module, const char* name) { return *module.Tree()->FindByName(name); };
    QJsonObject saved;
    {
        Modules::GameUiModule module(project);
        module.RestoreViewState(QJsonObject{{QStringLiteral("maps"), QJsonArray{}}});
        auto* page = module.Widget();
        page->resize(1400, 900);
        page->show();
        module.LoadDefinitionsForTesting(SyntheticDefinitions());
        const auto node = [&](const char* name) { return nodeIn(module, name); };

        // 原版界面默认可见、不可选：画布上点不中。
        QVERIFY(module.NodeVisible(node("ConsoleUI")));
        QVERIFY(!module.NodeSelectable(node("ConsoleTile")));
        QVERIFY(!module.NodePickable(node("ConsoleTile")));
        auto* canvas = page->findChild<Modules::GameUiCanvas*>(QStringLiteral("gameUiCanvas"));
        QVERIFY(canvas && !canvas->Pickable(node("ConsoleTile")));

        // “原版可选”打开后都能点中；再切换时清掉原版节点各自的设置。
        auto* origin = page->findChild<QPushButton*>(QStringLiteral("gameUiOriginSelectable"));
        QVERIFY(origin && !origin->isChecked());
        origin->click();
        QVERIFY(module.OriginSelectable());
        QVERIFY(module.NodePickable(node("ConsoleTile")) && canvas->Pickable(node("ConsoleTile")));
        module.SetNodeSelectable(node("ConsoleUI"), false);  // 对整棵子树
        QVERIFY(!module.NodeSelectable(node("ConsoleText")));
        module.SetOriginSelectable(false);
        module.SetOriginSelectable(true);
        QVERIFY(module.NodeSelectable(node("ConsoleText")));
        module.SetOriginSelectable(false);
        QVERIFY(!origin->isChecked());
        module.SetNodeSelectable(node("ConsoleUI"), true);
        QVERIFY(module.NodeSelectable(node("ConsoleText")));

        // 不可见：不画，可选也点不中；自身可见但父节点不可见时同样画不出来。
        module.SetNodeVisible(node("ConsoleUI"), false);
        QVERIFY(!module.NodeVisible(node("ConsoleTile")));
        QVERIFY(!module.NodePickable(node("ConsoleTile")));
        module.SetNodeVisible(node("ConsoleTile"), true);
        QVERIFY(module.NodeVisible(node("ConsoleTile")));
        QVERIFY(!module.NodePickable(node("ConsoleTile")));

        // 节点树按下可见列即切换整棵子树，不改选中行。
        QVERIFY(module.SelectFrame(QStringLiteral("ConsoleText")));
        auto* nodes = page->findChild<QTreeWidget*>(QStringLiteral("gameUiNodes"));
        QTreeWidgetItem* consoleUi = FindItem(nodes, QStringLiteral("ConsoleUI"));
        QVERIFY(consoleUi);
        nodes->scrollToItem(consoleUi);
        const QRect eye = nodes->visualRect(nodes->indexFromItem(consoleUi, 3));
        QVERIFY(eye.isValid());
        QTest::mouseClick(nodes->viewport(), Qt::LeftButton, {}, eye.center());
        QVERIFY(module.NodeVisible(node("ConsoleUI")));
        QVERIFY(module.NodePickable(node("ConsoleTile")));
        QCOMPARE(module.Tree()->Nodes()[module.SelectedNode()].name, std::string("ConsoleText"));

        module.SetNodeVisible(node("ConsoleUI"), false);
        saved = module.SaveViewState();
        QVERIFY(!saved.value(QStringLiteral("maps")).toArray().isEmpty());
    }

    // 同一张图：可见、可选取回。
    Modules::GameUiModule restored(project);
    restored.RestoreViewState(saved);
    (void)restored.Widget();
    restored.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(!restored.NodeVisible(nodeIn(restored, "ConsoleUI")));
    QVERIFY(restored.NodeSelectable(nodeIn(restored, "ConsoleText")));

    // 别的图不带这些设置。
    project.sourceArchivePath_ = std::filesystem::path(L"C:/Synthetic/ui-other.w3x");
    Modules::GameUiModule other(project);
    other.RestoreViewState(saved);
    (void)other.Widget();
    other.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(other.NodeVisible(nodeIn(other, "ConsoleUI")));
    QVERIFY(!other.NodeSelectable(nodeIn(other, "ConsoleText")));
}

void GameUiModuleTests::NewUiFlowAndLayerOrder() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    const auto byName = [&module](const char* name) { return *module.Tree()->FindByName(name); };
    QString error;

    // 编号写错当场说明，“创建”不可点；建议编号从 ui1 起。
    QVERIFY(!module.CheckNewDescriptionId(QStringLiteral("2234")).isEmpty());
    QCOMPARE(module.SuggestDescriptionId(), QStringLiteral("ui1"));
    auto* button = page->findChild<QPushButton*>(QStringLiteral("gameUiNewDescription"));
    QVERIFY(button && button->menu());
    QMenu* menu = button->menu();
    emit menu->aboutToShow();
    auto* idEdit = menu->findChild<QLineEdit*>(QStringLiteral("gameUiNewDescriptionId"));
    auto* labelEdit = menu->findChild<QLineEdit*>(QStringLiteral("gameUiNewDescriptionLabel"));
    auto* create = menu->findChild<QPushButton*>(QStringLiteral("gameUiNewDescriptionCreate"));
    auto* problem = menu->findChild<QLabel*>(QStringLiteral("formProblem"));
    QVERIFY(idEdit && labelEdit && create && problem);
    QCOMPARE(idEdit->text(), QStringLiteral("ui1"));
    QVERIFY(create->isEnabled());
    idEdit->setText(QStringLiteral("2234"));
    emit idEdit->textEdited(idEdit->text());
    QVERIFY(!create->isEnabled());
    QVERIFY(idEdit->property("invalid").toBool());
    QVERIFY(!problem->text().isEmpty());
    idEdit->setText(QStringLiteral("ui1"));
    emit idEdit->textEdited(idEdit->text());
    QVERIFY(create->isEnabled());
    QVERIFY(!idEdit->property("invalid").toBool());

    // 新界面自带面板并选中它；之后新控件默认放进这份界面。
    labelEdit->setText(QStringLiteral("背包"));
    create->click();
    const UiDescription* bag = module.Description(QStringLiteral("ui1"));
    QVERIFY(bag);
    QCOMPARE(bag->label, std::string("背包"));
    QCOMPARE(bag->frames.size(), std::size_t{1});
    QCOMPARE(bag->frames[0].id, std::string("panel1"));
    QCOMPARE(module.Tree()->Nodes()[module.SelectedNode()].name, std::string("panel1"));
    QCOMPARE(module.CurrentDescription(), QStringLiteral("ui1"));
    QCOMPARE(module.SuggestDescriptionId(), QStringLiteral("ui2"));
    QVERIFY(!module.CheckNewDescriptionId(QStringLiteral("UI1")).isEmpty());

    // 自己的控件能点中；界面的根铺满画面，不点中。
    QVERIFY(module.NodePickable(byName("panel1")));
    QVERIFY(!module.NodePickable(byName("ui1")));

    // 选中原版节点时添加控件：放进当前界面。
    QVERIFY(module.SelectFrame(QStringLiteral("ConsoleUI")));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("backdrop"), &error), QStringLiteral("back1"));
    QCOMPARE(module.Description(QStringLiteral("ui1"))->frames.size(), std::size_t{2});

    // 置于底层、顶层改同级顺序（后面的画在上面）；已在那一层时不产生修改。
    QVERIFY2(module.RaiseDescriptionFrame(byName("back1"), false, &error), qPrintable(error));
    QCOMPARE(module.Description(QStringLiteral("ui1"))->frames[0].id, std::string("back1"));
    QVERIFY2(module.RaiseDescriptionFrame(byName("back1"), true, &error), qPrintable(error));
    QCOMPARE(module.Description(QStringLiteral("ui1"))->frames[1].id, std::string("back1"));
    QVERIFY(!module.RaiseDescriptionFrame(byName("back1"), true, &error));

    // 不指定界面：新建一份界面放进去，与新建同一步撤销。
    const int undoBefore = project.AssetUndoStack()->count();
    QCOMPARE(module.CreateDescriptionFrame(QStringLiteral("text"), {}, {}, QPointF(0.2, 0.2), {}, &error), QStringLiteral("text1"));
    const UiDescription* fresh = module.Description(QStringLiteral("ui2"));
    QVERIFY(fresh);
    QCOMPARE(fresh->label, std::string("界面2"));
    QCOMPARE(fresh->frames[0].anchors[0].x, -0.2);
    QCOMPARE(project.AssetUndoStack()->count(), undoBefore + 1);
    QCOMPARE(module.CurrentDescription(), QStringLiteral("ui2"));
}

void GameUiModuleTests::GroupedPropertiesUnitsAndEvents() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), {}, &error, true), qPrintable(error));
    const auto frame = [&module](const char* id) { return FindUiFrame(*module.Description(QStringLiteral("Hud")), id); };
    const auto rectOf = [&module](const char* id) { return *module.DescriptionFrameRect(QStringLiteral("Hud"), QString::fromLatin1(id)); };
    auto* properties = page->findChild<QTreeWidget*>(QStringLiteral("gameUiProperties"));
    QVERIFY(properties);
    const auto field = [properties](const QString& label) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(properties); *it; ++it) {
            if ((*it)->text(0) == label && ((*it)->flags() & Qt::ItemIsEditable)) return *it;
        }
        return nullptr;
    };
    const auto edit = [](QTreeWidgetItem* item, const QString& text) {
        QVERIFY(item);
        item->setText(1, text);
        QCoreApplication::processEvents();
    };
    const auto choose = [&](QTreeWidgetItem* item, const QString& value) {
        QVERIFY(item);
        properties->setCurrentItem(item, 1);
        properties->editItem(item, 1);
        auto* box = properties->viewport()->findChild<QComboBox*>();
        QVERIFY(box);
        const int index = box->findData(value);
        QVERIFY2(index >= 0, qPrintable(value));
        box->setCurrentIndex(index);
        emit box->activated(index);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };

    // 分组：可改字段在前，只读的节点信息收进“详细”，默认折叠。
    QStringList groups;
    for (int i = 0; i < properties->topLevelItemCount(); ++i) groups << properties->topLevelItem(i)->text(0);
    QCOMPARE(groups, (QStringList{QStringLiteral("基本"), QStringLiteral("位置与大小"), QStringLiteral("外观"), QStringLiteral("事件"),
                         QStringLiteral("阵列"), QStringLiteral("详细")}));
    QVERIFY(!properties->topLevelItem(5)->isExpanded());
    QCOMPARE(field(QStringLiteral("父控件"))->text(1), QStringLiteral("（界面顶层）"));
    QCOMPARE(field(QStringLiteral("点击"))->text(1), QStringLiteral("（无）"));

    // 像素：参考 1920×1080，面板 0.24 宽即 576 像素；64×64 像素换成 0.026667 × 0.035556。
    choose(field(QStringLiteral("单位")), QStringLiteral("pixel"));
    QVERIFY(module.PixelUnits());
    QCOMPARE(field(QStringLiteral("宽"))->text(1), QStringLiteral("576"));
    edit(field(QStringLiteral("宽")), QStringLiteral("64"));
    edit(field(QStringLiteral("高")), QStringLiteral("64"));
    QCOMPARE(*frame("panel1")->width, RoundUiUnits(64.0 * 0.8 / 1920.0));
    QCOMPARE(*frame("panel1")->height, RoundUiUnits(64.0 * 0.6 / 1080.0));
    QCOMPARE(field(QStringLiteral("宽"))->text(1), QStringLiteral("64"));
    choose(field(QStringLiteral("单位")), QStringLiteral("war3"));
    QVERIFY(!module.PixelUnits());
    edit(field(QStringLiteral("宽")), QStringLiteral("128px"));  // 魔兽单位下带 px 也按像素
    QCOMPARE(*frame("panel1")->width, RoundUiUnits(128.0 * 0.8 / 1920.0));
    edit(field(QStringLiteral("宽")), QStringLiteral("0.24"));
    edit(field(QStringLiteral("高")), QStringLiteral("0.16"));
    module.SetReferenceResolution({2560, 1440});
    QCOMPARE(field(QStringLiteral("参考分辨率"))->text(1), QStringLiteral("2560×1440"));
    module.SetReferenceResolution({0, 1440});  // 不合法的不改
    QCOMPARE(module.ReferenceResolution().width, 2560);
    const QJsonObject view = module.SaveViewState();
    QCOMPARE(view.value(QStringLiteral("resolution")).toArray(), (QJsonArray{2560, 1440}));
    {
        QtProject::EditorProject other;
        Modules::GameUiModule restored(other);
        restored.RestoreViewState(view);
        QCOMPARE(restored.ReferenceResolution().height, 1440);
    }
    module.SetReferenceResolution({1920, 1080});

    // 透明度按百分数写；超出范围不改。
    edit(field(QStringLiteral("透明度")), QStringLiteral("50%"));
    QCOMPARE(frame("panel1")->alpha, 0.5);
    edit(field(QStringLiteral("透明度")), QStringLiteral("150"));
    QCOMPARE(frame("panel1")->alpha, 0.5);
    QCOMPARE(field(QStringLiteral("透明度"))->text(1), QStringLiteral("50%"));

    // 铺满父控件：去掉宽高，与父控件同样大；取消后居中、宽高取当前大小，画面上不动。
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("backdrop"), &error), QStringLiteral("back1"));
    choose(field(QStringLiteral("铺满父控件")), QStringLiteral("true"));
    QVERIFY(Modules::GameUiModule::FillsParent(*frame("back1")));
    QVERIFY(!frame("back1")->width && !field(QStringLiteral("宽")));
    QVERIFY(qAbs(rectOf("back1").left - rectOf("panel1").left) < 1e-5f && qAbs(rectOf("back1").top - rectOf("panel1").top) < 1e-5f);
    choose(field(QStringLiteral("铺满父控件")), QStringLiteral("false"));
    QVERIFY(!Modules::GameUiModule::FillsParent(*frame("back1")));
    QVERIFY(qAbs(*frame("back1")->width - 0.24) < 1e-5);
    QVERIFY(qAbs(rectOf("back1").right - rectOf("panel1").right) < 1e-5f);

    // 换父控件：位置不变；不能放进自己的子控件。
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QCOMPARE(module.AddDescriptionFrame(QStringLiteral("backdrop"), &error), QStringLiteral("back2"));
    QVERIFY2(module.MoveDescriptionFrame(module.SelectedNode(), 0.1, 0.05, &error), qPrintable(error));
    const FrameRect before = rectOf("back2");
    QVERIFY2(module.ReparentDescriptionFrame(QStringLiteral("Hud"), QStringLiteral("back2"), QStringLiteral("back1"), &error), qPrintable(error));
    QCOMPARE(UiFrameParentId(*module.Description(QStringLiteral("Hud")), "back2"), std::string("back1"));
    QVERIFY(qAbs(rectOf("back2").left - before.left) < 1e-5f && qAbs(rectOf("back2").bottom - before.bottom) < 1e-5f);
    QVERIFY(!module.ReparentDescriptionFrame(QStringLiteral("Hud"), QStringLiteral("panel1"), QStringLiteral("back2"), &error));
    QVERIFY(module.SelectFrame(QStringLiteral("back2")));
    choose(field(QStringLiteral("父控件")), QString());
    QCOMPARE(UiFrameParentId(*module.Description(QStringLiteral("Hud")), "back2"), std::string());
    QVERIFY(qAbs(rectOf("back2").left - before.left) < 1e-5f && qAbs(rectOf("back2").bottom - before.bottom) < 1e-5f);

    // 助手的单位换算与属性同一套公式。
    Modules::GameUiToolService service([&module] { return &module; });
    const QJsonObject converted = service.Invoke(QStringLiteral("game_ui_convert_units"), {{"pixels", QJsonArray{64, 64}}});
    QCOMPARE(converted.value("units").toArray(), (QJsonArray{RoundUiUnits(64.0 * 0.8 / 1920.0), RoundUiUnits(64.0 * 0.6 / 1080.0)}));

    // 事件交给触发器：新建、让已有触发器订阅、解除；界面触发器随之重新生成。
    AudastraUiTest::Catalog files;
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    const auto render = [&project](StableId id) {
        const auto text = Trigger::RenderTriggerText(*project.triggerDocument_, *project.uiCatalog_, id, 1 << 20, nullptr, nullptr);
        return text.value ? QString::fromStdString(*text.value) : QString();
    };
    const auto created = module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("panel1"), UiEventKind::Click, std::nullopt, &error);
    QVERIFY2(created && *created != 0, qPrintable(error));
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QVERIFY(render(*created).contains(QStringLiteral("AudastraUITriggerRegisterEvent(\"Hud.panel1_click\")")));
    QCOMPARE(module.Description(QStringLiteral("Hud"))->events.size(), std::size_t{1});
    const auto& rows = project.triggerDocument_->triggers;
    QVERIFY(std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.name.text.ends_with("Hud_Create"); }));
    // 生成的界面触发器不能订阅；新建的响应触发器可以。
    const auto candidates = module.EventTriggerCandidates();
    QVERIFY(std::none_of(candidates.begin(), candidates.end(), [](const auto& row) { return row.second.endsWith(QStringLiteral("Hud_Create")); }));
    QVERIFY(std::any_of(candidates.begin(), candidates.end(), [&](const auto& row) { return row.first == *created; }));
    QVERIFY(module.SelectFrame(QStringLiteral("panel1")));
    QCOMPARE(field(QStringLiteral("点击"))->data(1, Qt::UserRole + 41).toString(), QStringLiteral("trigger:%1").arg(*created));

    const std::string author = "触发器 打开背包:\n    events:\n        TriggerRegisterTimerEventPeriodic(1)\n"
                               "    actions:\n        CommentString(\"open\")\n";
    const auto binding = Trigger::CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(binding.Succeeded());
    Trigger::TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(Trigger::GraphApplyFeatureText{"背包", author, {}, std::nullopt});
    QVERIFY(Trigger::ApplyTriggerGraphEditBatch(*project.triggerDocument_, *project.uiCatalog_, batch).Succeeded());
    const auto bag = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.name.text == "打开背包"; });
    QVERIFY(bag != rows.end());
    const StableId bagId = bag->id;
    const auto linked = module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("back1"), UiEventKind::Enter, bagId, &error);
    QVERIFY2(linked && *linked == bagId, qPrintable(error));
    QVERIFY(render(bagId).contains(QStringLiteral("TriggerRegisterTimerEventPeriodic(1)")));
    QVERIFY(render(bagId).contains(QStringLiteral("AudastraUITriggerRegisterEvent(\"Hud.back1_enter\")")));
    QCOMPARE(module.FrameEventTriggers(QStringLiteral("Hud"), QStringLiteral("back1"), UiEventKind::Enter).triggers.size(), qsizetype{1});
    // 同一个再选一次不产生修改。
    const auto steps = project.triggerUndoStack_->count();
    QVERIFY(module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("back1"), UiEventKind::Enter, bagId, &error));
    QCOMPARE(project.triggerUndoStack_->count(), steps);
    // 解除：事件与订阅行一起去掉，触发器其余内容不动；只有这一个事件的触发器连“events:”一起去掉。
    QVERIFY2(module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("back1"), UiEventKind::Enter, StableId{0}, &error), qPrintable(error));
    QVERIFY(!render(bagId).contains(QStringLiteral("AudastraUITriggerRegisterEvent")));
    QVERIFY(render(bagId).contains(QStringLiteral("TriggerRegisterTimerEventPeriodic(1)")));
    QVERIFY2(module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("panel1"), UiEventKind::Click, StableId{0}, &error), qPrintable(error));
    QVERIFY(module.Description(QStringLiteral("Hud"))->events.empty());
    QVERIFY(!render(*created).contains(QStringLiteral("events:")));
    QVERIFY(render(*created).contains(QStringLiteral("actions:")));
    // 助手读到同一份订阅。
    QVERIFY(module.LinkFrameEvent(QStringLiteral("Hud"), QStringLiteral("panel1"), UiEventKind::Leave, bagId, &error));
    const QJsonObject events = service.Invoke(QStringLiteral("game_ui_frame_event"),
        {{"description", "Hud"}, {"frame", "panel1"}, {"event", "leave"}});
    QCOMPARE(events.value("event_key").toString(), QStringLiteral("Hud.panel1_leave"));
    QCOMPARE(events.value("triggers").toArray().size(), qsizetype{1});
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    GameUiModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GameUiModuleTests.moc"
