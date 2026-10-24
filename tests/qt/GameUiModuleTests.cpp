#include "AudastraUiCatalog.h"
#include "CastSpellLayer.h"
#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiModule.h"
#include "PartyLayer.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableWidget>
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
    void FramePropertiesEditLayoutAndText();
    void GeneratesCastSystem();
    void CreatesFromPresets();
    void GeneratesPartySystem();
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
    auto* tile = FindItem(nodes, QStringLiteral("ConsoleTile"));
    QVERIFY(tile);
    tile->setCheckState(0, Qt::Unchecked);
    QCOMPARE(module.DrawList().commands.size(), before - 1);
    // 隐藏按节点身份保存，种族切换重建后仍然隐藏。
    module.SetRace(QStringLiteral("Human"));
    QCOMPARE(module.DrawList().commands.size(), before - 1);
    QCOMPARE(FindItem(nodes, QStringLiteral("ConsoleTile"))->checkState(0), Qt::Unchecked);

    // 只显示一项：同级隐藏，自身与父节点显示。
    FindItem(nodes, QStringLiteral("ConsoleText"))->setCheckState(0, Qt::Unchecked);
    module.ShowOnly(*module.Tree()->FindByName("ConsoleText"));
    QCOMPARE(FindItem(nodes, QStringLiteral("ConsoleText"))->checkState(0), Qt::Checked);
    QCOMPARE(FindItem(nodes, QStringLiteral("层 BACKGROUND"))->checkState(0), Qt::Unchecked);
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

    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Bar, &error), QStringLiteral("bar1"));
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

    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Text, &error), QStringLiteral("text1"));
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
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Bar, &error), QStringLiteral("bar1"));
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
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Button, &error), QStringLiteral("button1"));
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
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Bar, &error), QStringLiteral("bar1"));
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Text, &error), QStringLiteral("text1"));
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

    // 描述不合法的部分（锚在游戏代码区域）整体拒绝，什么都不改。
    QVERIFY2(module.EditDescription(QStringLiteral("Cast"),
                 QStringLiteral(R"([{"op":"set_frame","frame":"bar1","fields":{"anchors":[{"point":"center","to":"game:Minimap","toPoint":"center"}]}}])"),
                 {}, &error),
        qPrintable(error));
    QVERIFY(!module.GenerateDescriptionTriggers(QStringLiteral("Cast"), false, &error));
    QVERIFY(error.contains(QStringLiteral("Minimap")));
    QVERIFY(!category() && project.triggerUndoStack_->index() == 0);
}

void GameUiModuleTests::FramePropertiesEditLayoutAndText() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    QVERIFY2(module.CreateDescription(QStringLiteral("Hud"), {}, &error), qPrintable(error));
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Backdrop, &error), QStringLiteral("back1"));
    QVERIFY(module.SelectFrame(QStringLiteral("Hud")));
    QCOMPARE(module.AddDescriptionFrame(UiFrameType::Text, &error), QStringLiteral("text1"));
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
    edit(field(QStringLiteral("颜色")), QStringLiteral("#80FF0000"));
    QCOMPARE(frame("text1")->color, (std::array<double, 4>{1.0, 0.0, 0.0, 0.502}));
    QCOMPARE(field(QStringLiteral("颜色"))->text(1), QStringLiteral("#80FF0000"));
    edit(field(QStringLiteral("颜色")), QStringLiteral("red"));
    QCOMPARE(frame("text1")->color[3], 0.502);
    edit(field(QStringLiteral("颜色")), QString());
    QCOMPARE(frame("text1")->color, (std::array<double, 4>{1.0, 1.0, 1.0, 1.0}));
    edit(field(QStringLiteral("字体")), QStringLiteral("Fonts\\FRIZQT__.TTF"));
    QCOMPARE(frame("text1")->font, std::string("Fonts\\FRIZQT__.TTF"));

    // 锚点：目标只列自身以外的框架；改对齐点时目标点保持不变。
    QComboBox* targets = open(field(QStringLiteral("目标"), QStringLiteral("锚点 1")));
    QVERIFY(targets);
    QVERIFY(targets->findData(QStringLiteral("back1")) >= 0);
    QVERIFY(targets->findData(QStringLiteral("text1")) < 0);
    QVERIFY(targets->findData(QStringLiteral("game:Minimap")) >= 0);
    emit properties->itemDelegateForColumn(1)->closeEditor(targets);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    choose(field(QStringLiteral("目标"), QStringLiteral("锚点 1")), QStringLiteral("back1"));
    choose(field(QStringLiteral("对齐点"), QStringLiteral("锚点 1")), QStringLiteral("TOPLEFT"));
    QCOMPARE(frame("text1")->anchors.size(), std::size_t{1});
    QVERIFY(frame("text1")->anchors[0].target == UiAnchorTarget::Frame);
    QCOMPARE(frame("text1")->anchors[0].targetId, std::string("back1"));
    QVERIFY(frame("text1")->anchors[0].point == FramePoint::TopLeft);
    QVERIFY(frame("text1")->anchors[0].targetPoint == FramePoint::Center);
    choose(field(QStringLiteral("目标点"), QStringLiteral("锚点 1")), QStringLiteral("BOTTOMLEFT"));
    QVERIFY(frame("text1")->anchors[0].targetPoint == FramePoint::BottomLeft);
    edit(field(QStringLiteral("横向偏移"), QStringLiteral("锚点 1")), QStringLiteral("0.01"));
    QCOMPARE(frame("text1")->anchors[0].x, 0.01);
    choose(field(QStringLiteral("目标"), QStringLiteral("锚点 1")), QStringLiteral("parent"));
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

    // 重复：填次数才有间距，清空次数即不重复。
    QVERIFY(field(QStringLiteral("次数")));
    QVERIFY(!field(QStringLiteral("横向间距")));
    edit(field(QStringLiteral("次数")), QStringLiteral("3"));
    QVERIFY(frame("text1")->repeat && frame("text1")->repeat->count == 3);
    edit(field(QStringLiteral("横向间距")), QStringLiteral("0.02"));
    QCOMPARE(frame("text1")->repeat->dx, 0.02);
    edit(field(QStringLiteral("次数")), QStringLiteral("0"));
    QCOMPARE(frame("text1")->repeat->count, 3u);
    edit(field(QStringLiteral("次数")), QString());
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

void GameUiModuleTests::GeneratesCastSystem() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    (void)module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    Modules::GameUiModule::CastSystemRequest request;
    QVERIFY(!module.GenerateCastSystem(request, true, &error));
    QVERIFY(error.contains(QStringLiteral("触发器")));

    AudastraUiTest::Catalog files;
    files.extras = {AudastraUiTest::SpellLayer()};
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    const auto count = [&project] {
        const auto& doc = *project.triggerDocument_;
        const auto category = std::find_if(doc.categories.begin(), doc.categories.end(),
            [](const auto& row) { return row.name.text == kCastTriggerCategory; });
        if (category == doc.categories.end()) return -1;
        return static_cast<int>(std::count_if(doc.triggers.begin(), doc.triggers.end(),
            [&](const auto& row) { return row.categoryId == category->categoryId; }));
    };

    // 预览不改地图；生成一次撤销。
    const auto preview = module.GenerateCastSystem(request, true, &error);
    QVERIFY2(preview, qPrintable(error));
    QCOMPARE(preview->triggers.size(), qsizetype{11});
    QVERIFY(!preview->registrationKept);
    QCOMPARE(count(), -1);
    QVERIFY2(module.GenerateCastSystem(request, false, &error), qPrintable(error));
    QCOMPARE(count(), 11);
    QCOMPARE(project.triggerUndoStack_->count(), 1);

    // 再次生成：登记触发器已在，保留作者的登记。
    request.abilities = {GameUi::CastAbility{"A000", 1.0, 0.0, 0.0, "missing"}};
    const auto again = module.GenerateCastSystem(request, true, &error);
    QVERIFY2(again, qPrintable(error));
    QVERIFY(again->registrationKept);
    QCOMPARE(again->triggers.size(), qsizetype{10});

    // 读条条：界面触发器须先生成。
    QVERIFY2(module.CreateDescription(QStringLiteral("Bar"), QStringLiteral("施法条"), &error), qPrintable(error));
    QVERIFY2(module.EditDescription(QStringLiteral("Bar"), QStringLiteral(
        R"([{"op":"set_values","values":[{"id":"shown","type":"boolean","default":false},{"id":"progress","type":"real","default":0}]},)"
        R"({"op":"add_frame","frame":{"id":"fill","type":"bar","width":0.2,"height":0.02,"value":"progress","visible":"shown",)"
        R"("anchors":[{"point":"CENTER"}]}}])"), {}, &error), qPrintable(error));
    request.abilities.clear();
    request.bar = QStringLiteral("Bar");
    request.visible = QStringLiteral("shown");
    request.progress = QStringLiteral("progress");
    QVERIFY(!module.GenerateCastSystem(request, false, &error));
    QVERIFY(error.contains(QStringLiteral("Bar")));
    QVERIFY2(module.GenerateDescriptionTriggers(QStringLiteral("Bar"), false, &error), qPrintable(error));
    const auto bound = module.GenerateCastSystem(request, false, &error);
    QVERIFY2(bound, qPrintable(error));
    QVERIFY(bound->featureText.contains(QStringLiteral("udg_AUIV_Bar_progress")));
    QCOMPARE(count(), 12);
}

void GameUiModuleTests::CreatesFromPresets() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QString error;
    // 新建界面菜单里的模板列表与 Core 的模板一致。
    auto* presets = page->findChild<QComboBox*>(QStringLiteral("gameUiNewDescriptionPreset"));
    QVERIFY(presets);
    QCOMPARE(presets->count(), 3);
    QVERIFY(presets->findData(QStringLiteral("castbar")) > 0 && presets->findData(QStringLiteral("party")) > 0);
    QVERIFY2(module.CreateDescription(QStringLiteral("Party"), {}, &error, QStringLiteral("party")), qPrintable(error));
    QCOMPARE(module.Description(QStringLiteral("Party"))->values.size(), std::size_t{10});
    QCOMPARE(module.Description(QStringLiteral("Party"))->events.size(), std::size_t{1});
    QCOMPARE(module.Description(QStringLiteral("Party"))->label, std::string("团队框架"));
    QVERIFY(module.SelectFrame(QStringLiteral("slot#4")));
    QVERIFY2(module.CreateDescription(QStringLiteral("Bar"), QStringLiteral("施法条"), &error, QStringLiteral("castbar")), qPrintable(error));
    QCOMPARE(module.Description(QStringLiteral("Bar"))->states.size(), std::size_t{2});
    QVERIFY(!module.CreateDescription(QStringLiteral("Other"), {}, &error, QStringLiteral("nothing")));
    QVERIFY(!module.Description(QStringLiteral("Other")));
}

void GameUiModuleTests::GeneratesPartySystem() {
    QtProject::EditorProject project;
    Modules::GameUiModule module(project);
    auto* page = module.Widget();
    module.LoadDefinitionsForTesting(SyntheticDefinitions());
    QVERIFY(page->findChild<QPushButton*>(QStringLiteral("gameUiPartySystem")));
    QString error;
    using namespace GameUi::PartyPreset;
    const auto id = [](std::string_view value) { return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size())); };
    Modules::GameUiModule::PartySystemRequest request;
    request.frame = QStringLiteral("Party");
    request.shown = id(kShown);
    QVERIFY(!module.GeneratePartySystem(request, true, &error));
    QVERIFY(error.contains(QStringLiteral("触发器")));

    AudastraUiTest::Catalog files;
    files.extras = {AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer()};
    const auto loaded = files.Load();
    QVERIFY2(loaded.empty(), loaded.c_str());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*files.catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*files.document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    const auto count = [&project] {
        const auto& doc = *project.triggerDocument_;
        const auto category = std::find_if(doc.categories.begin(), doc.categories.end(),
            [](const auto& row) { return row.name.text == kPartyTriggerCategory; });
        if (category == doc.categories.end()) return -1;
        return static_cast<int>(std::count_if(doc.triggers.begin(), doc.triggers.end(),
            [&](const auto& row) { return row.categoryId == category->categoryId; }));
    };

    // 界面描述与它的界面触发器都须先有。
    QVERIFY(!module.GeneratePartySystem(request, true, &error));
    QVERIFY(error.contains(QStringLiteral("Party")));
    QVERIFY2(module.CreateDescription(QStringLiteral("Party"), QStringLiteral("队伍"), &error, QStringLiteral("party")), qPrintable(error));
    request.health = id(kHealth);
    request.mana = id(kMana);
    request.name = id(kName);
    request.dead = id(kDead);
    request.outOfRange = id(kFar);
    request.select = id(kSelect);
    QVERIFY(!module.GeneratePartySystem(request, true, &error));
    QVERIFY(error.contains(QStringLiteral("Party")));
    QVERIFY2(module.GenerateDescriptionTriggers(QStringLiteral("Party"), false, &error), qPrintable(error));

    // 预览不改地图；生成一次撤销。
    const auto preview = module.GeneratePartySystem(request, true, &error);
    QVERIFY2(preview, qPrintable(error));
    QCOMPARE(preview->triggers.size(), qsizetype{7});
    QCOMPARE(preview->slotCount, std::uint32_t{5});
    QCOMPARE(preview->joinTrigger, QStringLiteral("团队 加入 Party_Join"));
    QCOMPARE(count(), -1);
    const int undo = project.triggerUndoStack_->count();
    QVERIFY2(module.GeneratePartySystem(request, false, &error), qPrintable(error));
    QCOMPARE(count(), 7);
    QCOMPARE(project.triggerUndoStack_->count(), undo + 1);

    // 不合法的绑定整体拒绝，不改地图。
    request.dead = id(kName);
    QVERIFY(!module.GeneratePartySystem(request, false, &error));
    QCOMPARE(project.triggerUndoStack_->count(), undo + 1);

    // 增减益：图标取自物编，取不到时拒绝；生成登记触发器，再次生成时保留作者的登记。
    request.dead = id(kDead);
    for (const auto icon : kBuffs) request.buffIcons.append(id(icon));
    request.buffs = {QStringLiteral("Bxxx")};
    QVERIFY(!module.GeneratePartySystem(request, true, &error));
    QVERIFY(error.contains(QStringLiteral("Bxxx")));
    request.buffs.clear();
    const auto withBuffs = module.GeneratePartySystem(request, false, &error);
    QVERIFY2(withBuffs, qPrintable(error));
    QCOMPARE(withBuffs->triggers.size(), qsizetype{8});
    QVERIFY(!withBuffs->registrationKept);
    request.buffs = {QStringLiteral("Bxxx")};
    const auto again = module.GeneratePartySystem(request, true, &error);
    QVERIFY2(again, qPrintable(error));
    QVERIFY(again->registrationKept);
    QCOMPARE(again->triggers.size(), qsizetype{7});
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    GameUiModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GameUiModuleTests.moc"
