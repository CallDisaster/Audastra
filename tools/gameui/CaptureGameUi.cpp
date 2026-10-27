// 开发核对用：离屏打开“界面”工作区，用本机游戏目录读取原版界面，等待读取完成后保存整页与画布截图。
// 不进入发行包；输出只写到调用方给的目录。
#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "GameUiKindPicker.h"
#include "GameUiModule.h"
#include "Theme/ThemeManager.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTabWidget>
#include <QTextStream>
#include <QWidget>

#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>

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
        out << "usage: audastra-game-ui-capture <war3-root> <out-dir> [--map MAP] [--race R] [--classic] [--frame NAME]... "
               "[--only NAME]... [--select NAME] [--bounds] [--names] [--simulate FUNC] [--set KEY=VALUE]... "
               "[--import ARCHIVE_PATH=FILE]... [--ui-new ID]... [--ui-add ID:KIND[@X,Y]]... [--ui-state ID=STATE] [--ui-set ID:KEY=VALUE]... "
               "[--show-hidden] [--ui-tab N] [--kind-picker KIND]\n";
        return 2;
    }
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
    War3Map::UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));

    War3Map::QtProject::EditorProject project;
    project.war3GameRoot_ = std::filesystem::path(arguments[1].toStdWString());
    const QDir output(arguments[2]);
    if (!QDir().mkpath(output.absolutePath())) return 3;

    // --map：只读打开地图（建议用独立副本），显示地图界面目录与脚本创建的界面。
    if (const qsizetype at = arguments.indexOf(QStringLiteral("--map")); at > 0 && at + 1 < arguments.size()) {
        const std::filesystem::path mapPath(arguments[at + 1].toStdWString());
        auto opened = War3Map::MpqWorkspace::OpenReadOnly(mapPath);
        if (!opened.Succeeded() || !opened.value) {
            out << "map open failed\n";
            return 6;
        }
        project.readOnlyArchive_ = std::make_shared<const War3Map::MpqWorkspace>(std::move(*opened.value));
        project.sourceArchivePath_ = mapPath;
    }
    // --import 归档路径=本地文件：放进本次会话的导入层（不写地图），例如界面描述 AudastraUI\X.json。
    for (qsizetype i = 3; i + 1 < arguments.size(); ++i) {
        if (arguments[i] != QLatin1String("--import")) continue;
        const QString pair = arguments[++i];
        const qsizetype equals = pair.indexOf(QLatin1Char('='));
        QFile file(pair.mid(equals + 1));
        if (equals <= 0 || !file.open(QIODevice::ReadOnly)) {
            out << "bad --import: " << pair << '\n';
            return 7;
        }
        const QByteArray data = file.readAll();
        auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(data.size()));
        std::memcpy(bytes->data(), data.constData(), bytes->size());
        std::vector<War3Map::QtProject::EditorProject::ImportPayload> payloads{{pair.left(equals), std::move(bytes), false}};
        if (const QString error = project.AddImportPayloads(payloads, true, QStringLiteral("capture")); !error.isEmpty()) {
            out << "import failed: " << error << '\n';
            return 7;
        }
    }

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
    QString simulate;
    int uiTab = -1;
    QString kindPicker;
    std::map<std::string, std::string> simulationValues;
    for (qsizetype i = 3; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        const bool hasValue = i + 1 < arguments.size();
        if (argument == QLatin1String("--map") && hasValue) ++i;
        else if (argument == QLatin1String("--race") && hasValue) module.SetRace(arguments[++i]);
        else if (argument == QLatin1String("--classic")) module.SetExpansion(false);
        else if (argument == QLatin1String("--frame") && hasValue) {
            if (!module.AddRoot(arguments[++i])) out << "frame not found: " << arguments[i] << '\n';
        } else if (argument == QLatin1String("--select") && hasValue) select = arguments[++i];
        else if (argument == QLatin1String("--simulate") && hasValue) simulate = arguments[++i];
        else if (argument == QLatin1String("--import") && hasValue) ++i;
        else if (argument == QLatin1String("--show-hidden")) module.SetShowHidden(true);
        else if (argument == QLatin1String("--ui-tab") && hasValue) uiTab = arguments[++i].toInt();
        else if (argument == QLatin1String("--ui-new") && hasValue) {
            // --ui-new 界面：本次会话里新建一份空白界面描述（不写地图）；要放在给它加框架、设值的参数之前。
            QString error;
            if (!module.CreateDescription(arguments[++i], {}, &error)) out << "bad --ui-new " << arguments[i] << ' ' << error << '\n';
        }
        else if (argument == QLatin1String("--ui-add") && hasValue) {
            // --ui-add 界面:类型[@X,Y]：按类型在顶层加一个框架，给了 X,Y（屏幕单位）时中心放在那里。
            const QString spec = arguments[++i];
            const qsizetype colon = spec.indexOf(QLatin1Char(':'));
            const qsizetype at = spec.indexOf(QLatin1Char('@'));
            std::optional<QPointF> position;
            if (at > colon) {
                const QStringList xy = spec.mid(at + 1).split(QLatin1Char(','));
                if (xy.size() == 2) position = QPointF(xy[0].toDouble(), xy[1].toDouble());
            }
            QString error;
            const QString kind = spec.mid(colon + 1, at > colon ? at - colon - 1 : -1);
            if (colon <= 0 || module.CreateDescriptionFrame(kind, spec.left(colon), {}, position, {}, &error).isEmpty())
                out << "bad --ui-add " << spec << ' ' << error << '\n';
        }
        else if (argument == QLatin1String("--kind-picker") && hasValue) kindPicker = arguments[++i];
        else if ((argument == QLatin1String("--ui-state") || argument == QLatin1String("--ui-set")) && hasValue) {
            // --ui-state 界面=状态；--ui-set 界面:数值键=值
            const QString spec = arguments[++i];
            QString error;
            bool ok = false;
            if (argument == QLatin1String("--ui-state")) {
                const qsizetype equals = spec.indexOf(QLatin1Char('='));
                ok = equals > 0 && module.SetDescriptionState(spec.left(equals), spec.mid(equals + 1), &error);
            } else {
                const qsizetype colon = spec.indexOf(QLatin1Char(':'));
                const qsizetype equals = spec.indexOf(QLatin1Char('='), colon + 1);
                ok = colon > 0 && equals > colon + 1 &&
                     module.SetDescriptionValues(spec.left(colon),
                         {{spec.mid(colon + 1, equals - colon - 1).toStdString(), spec.mid(equals + 1).toStdString()}}, &error);
            }
            if (!ok) out << "bad " << argument << ' ' << spec << ' ' << error << '\n';
        }
        else if (argument == QLatin1String("--set") && hasValue) {
            const QString pair = arguments[++i];
            const qsizetype equals = pair.indexOf(QLatin1Char('='));
            if (equals > 0) simulationValues[pair.left(equals).toStdString()] = pair.mid(equals + 1).toStdString();
            else out << "bad --set: " << pair << '\n';
        }
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
    if (!module.SimulationFunctions().isEmpty())
        out << "simulation functions: " << module.SimulationFunctions().join(QStringLiteral(", ")) << '\n';
    if (!simulate.isEmpty()) {
        QString error;
        if (!module.SetSimulation(simulate, &error)) out << "simulate failed: " << error << '\n';
        if (!simulationValues.empty()) module.SetSimulationValues(simulationValues);
        if (const auto* result = module.Simulation()) {
            for (const auto& input : result->inputs)
                out << "input " << QString::fromStdString(input.key) << " = " << QString::fromStdString(input.value) << '\n';
            out << "applied calls=" << result->appliedCalls << " touched=" << result->touched.size() << '\n';
        }
    }
    if (!select.isEmpty() && !module.SelectFrame(select)) out << "select not found: " << select << '\n';
    // --ui-tab：界面描述面板的页（0 预览、1 数值、2 状态、3 事件）。
    if (auto* tabs = page->findChild<QTabWidget*>(QStringLiteral("gameUiDescriptionTabs")); tabs && uiTab >= 0) tabs->setCurrentIndex(uiTab);
    QApplication::processEvents();
    const bool pageSaved = page->grab().save(output.filePath(QStringLiteral("workspace.png")));
    const bool canvasSaved = module.RenderImage(QSize(1920, 1080)).save(output.filePath(QStringLiteral("game-ui-1920x1080.png")));
    // --kind-picker 类型：打开右键“创建界面”的类型选取并选中该类型，等预览贴图读完后单独截图。
    bool pickerSaved = true;
    if (!kindPicker.isEmpty()) {
        auto* picker = qobject_cast<War3Map::Modules::GameUiKindPicker*>(
            module.OpenKindPicker(module.SelectedNode(), std::nullopt, QPoint(0, 0)).data());
        pickerSaved = picker && picker->SelectKind(kindPicker) && WaitUntil([&] { return !module.IsLoading(); }, 60000);
        QApplication::processEvents();
        pickerSaved = pickerSaved && picker->grab().save(output.filePath(QStringLiteral("kind-picker.png")));
        if (!pickerSaved) out << "kind picker capture failed: " << kindPicker << '\n';
    }
    out << "nodes=" << (module.Tree() ? module.Tree()->Nodes().size() : 0) << " commands=" << module.DrawList().commands.size()
        << " saved=" << (pageSaved && canvasSaved && pickerSaved) << '\n';
    return pageSaved && canvasSaved && pickerSaved ? 0 : 5;
}
