// 开发核对用：离屏打开“界面”工作区，用本机游戏目录读取原版界面，等待读取完成后保存整页与画布截图。
// 不进入发行包；输出只写到调用方给的目录。
#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "GameUiModule.h"
#include "Theme/ThemeManager.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTextStream>
#include <QWidget>

#include <functional>

namespace {

bool WaitUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTextStream out(stdout);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 3) {
        out << "usage: audastra-game-ui-capture <war3-root> <out-dir> [--race R] [--classic] [--frame NAME]... "
               "[--only NAME]... [--select NAME] [--bounds] [--names]\n";
        return 2;
    }
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
    War3Map::UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));

    War3Map::QtProject::EditorProject project;
    project.war3GameRoot_ = std::filesystem::path(arguments[1].toStdWString());
    const QDir output(arguments[2]);
    if (!QDir().mkpath(output.absolutePath())) return 3;

    War3Map::Modules::GameUiModule module(project);
    bool scene = false;
    QObject::connect(&module, &War3Map::Modules::GameUiModule::SceneChanged, [&] { scene = true; });
    QWidget* page = module.Widget();
    page->resize(1920, 1080);
    page->show();
    if (!WaitUntil([&] { return scene && !module.IsLoading(); }, 60000)) {
        out << "timeout waiting for definitions\n";
        return 4;
    }
    QString select;
    for (qsizetype i = 3; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        const bool hasValue = i + 1 < arguments.size();
        if (argument == QLatin1String("--race") && hasValue) module.SetRace(arguments[++i]);
        else if (argument == QLatin1String("--classic")) module.SetExpansion(false);
        else if (argument == QLatin1String("--frame") && hasValue) {
            if (!module.AddRoot(arguments[++i])) out << "frame not found: " << arguments[i] << '\n';
        } else if (argument == QLatin1String("--select") && hasValue) select = arguments[++i];
        else if (argument == QLatin1String("--only") && hasValue) {
            const auto node = module.Tree() ? module.Tree()->FindByName(arguments[++i].toStdString()) : std::nullopt;
            if (node) module.ShowOnly(*node);
            else out << "only not found: " << arguments[i] << '\n';
        }
        else if (argument == QLatin1String("--bounds")) page->findChild<QWidget*>(QStringLiteral("gameUiShowBounds"))->setProperty("checked", true);
        else if (argument == QLatin1String("--names")) page->findChild<QWidget*>(QStringLiteral("gameUiShowNames"))->setProperty("checked", true);
    }
    if (!WaitUntil([&] { return !module.IsLoading(); }, 60000)) {
        out << "timeout waiting for assets\n";
        return 4;
    }
    if (!select.isEmpty() && !module.SelectFrame(select)) out << "select not found: " << select << '\n';
    QApplication::processEvents();
    const bool pageSaved = page->grab().save(output.filePath(QStringLiteral("workspace.png")));
    const bool canvasSaved = module.RenderImage(QSize(1920, 1080)).save(output.filePath(QStringLiteral("game-ui-1920x1080.png")));
    out << "nodes=" << (module.Tree() ? module.Tree()->Nodes().size() : 0) << " commands=" << module.DrawList().commands.size()
        << " saved=" << (pageSaved && canvasSaved) << '\n';
    return pageSaved && canvasSaved ? 0 : 5;
}
