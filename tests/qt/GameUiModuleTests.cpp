#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiModule.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

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

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    GameUiModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GameUiModuleTests.moc"
