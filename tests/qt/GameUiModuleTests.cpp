#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiModule.h"

#include <QApplication>
#include <QCheckBox>
#include <QStandardItemModel>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

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

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    GameUiModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "GameUiModuleTests.moc"
