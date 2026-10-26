#include "MainWindow.h"
#include "ApplicationIdentity.h"
#include "ProgramIssueApplication.h"
#include "McpProductTools.h"
#include "War3MapCore/Update/UpdateInstaller.h"

#include "Fonts/FontLoader.h"

#include <QApplication>
#include <QSettings>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QScreen>
#include <QRegularExpression>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <optional>

namespace {
int RunApplication(War3Map::QtFeedback::ProgramIssueApplication& application) {
    War3Map::QtApp::ConfigureApplicationIdentity();

    // Qt 自带对话框和标准按钮跟随系统中文；业务文案仍由源码中的中文 tr() 提供。
    // qt_*.qm 是元目录，覆盖 qtbase 与多媒体等模块。
    // 先找 exe 旁部署的 translations/（构建目录与发布目录都没有 qt.conf 时，
    // QLibraryInfo 的路径不一定指向这里），再退回 Qt 安装目录。
    QTranslator qtTranslator;
    for (const QString& directory : {
             QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
             QLibraryInfo::path(QLibraryInfo::TranslationsPath)}) {
        if (qtTranslator.load(QLocale::system(), QStringLiteral("qt"), QStringLiteral("_"), directory)) {
            application.installTranslator(&qtTranslator);
            break;
        }
    }

    const QStringList arguments = application.arguments();
    const auto healthValue = [&](const QString& name) -> QString {
        const auto index = arguments.indexOf(name);
        return arguments.count(name) == 1 && index + 1 < arguments.size() ? arguments[index + 1] : QString{};
    };
    const QString healthTransaction = healthValue(QStringLiteral("--update-health-transaction"));
    const QString healthChallenge = healthValue(QStringLiteral("--update-health-challenge"));
    const QString healthPipeText = healthValue(QStringLiteral("--update-health-pipe"));
    const bool healthRequested = arguments.contains(QStringLiteral("--update-health-transaction"))
        || arguments.contains(QStringLiteral("--update-health-challenge")) || arguments.contains(QStringLiteral("--update-health-pipe"));
    bool healthPipeValid = false;
    const auto healthPipe = healthPipeText.toULongLong(&healthPipeValid);
    if (healthRequested && (!QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(healthTransaction).hasMatch()
        || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(healthChallenge).hasMatch()
        || !QRegularExpression(QStringLiteral("^[0-9]+$")).match(healthPipeText).hasMatch() || !healthPipeValid || !healthPipe)) return 1;
    const qsizetype workspaceOption = arguments.indexOf(QStringLiteral("--workspace"));
    const bool exitAfterOpen = arguments.contains(QStringLiteral("--exit-after-open"));
    const auto captureUiOption = arguments.indexOf(QStringLiteral("--capture-ui"));
    const bool captureUiRequested = captureUiOption >= 0;
    if (captureUiRequested && (arguments.count(QStringLiteral("--capture-ui")) != 1
        || captureUiOption + 1 >= arguments.size() || arguments[captureUiOption + 1].isEmpty()
        || arguments[captureUiOption + 1].startsWith(QStringLiteral("--")))) return 1;
    if (!captureUiRequested && (arguments.contains(QStringLiteral("--capture-ui-size"))
        || arguments.contains(QStringLiteral("--capture-ui-expected-dpr")))) return 1;
    const bool requireLoadedWorkspace =
        arguments.contains(QStringLiteral("--require-loaded-workspace"));
    const bool runtimeSaveProbe = arguments.contains(QStringLiteral("--probe-runtime-save"));
    const bool headlessProbe = exitAfterOpen || requireLoadedWorkspace || captureUiRequested || runtimeSaveProbe;
    if (healthRequested && headlessProbe) return 1;
    const auto worldOption = arguments.indexOf(QStringLiteral("--probe-world"));
    const auto worldTimeoutOption = arguments.indexOf(QStringLiteral("--probe-world-ready-timeout"));
    const bool passiveWorldProbe = arguments.contains(QStringLiteral("--probe-world-passive"));
    const auto worldOutputOption = arguments.indexOf(QStringLiteral("--probe-world-output"));
    const bool frameOnlyWorldProbe = arguments.contains(QStringLiteral("--probe-world-frame-only"));
    const bool modelListOnlyWorldProbe = arguments.contains(QStringLiteral("--probe-world-model-list-only"));
    const auto worldFocusOption = arguments.indexOf(QStringLiteral("--probe-world-focus-host-key"));
    QString worldFocusHostKey;
    if (worldFocusOption >= 0) {
        if (arguments.count(QStringLiteral("--probe-world-focus-host-key")) != 1 || !headlessProbe
            || !frameOnlyWorldProbe || passiveWorldProbe || worldOption < 0 || worldOutputOption < 0
            || worldFocusOption + 1 >= arguments.size()) return 1;
        worldFocusHostKey = arguments[worldFocusOption + 1];
        if (worldFocusHostKey.isEmpty() || worldFocusHostKey.size() > 512 || worldFocusHostKey.startsWith(QStringLiteral("--"))) return 1;
    }
    std::optional<std::uint64_t> worldSequence, worldElapsed, worldGlobal;
    const auto probeClockOption = [&](const QString& option, const char* environment,
        std::optional<std::uint64_t>& output) {
        const auto index = arguments.indexOf(option);
        QString value;
        if (index >= 0) {
            if (arguments.count(option) != 1 || index + 1 >= arguments.size()) return false;
            value = arguments[index + 1];
        } else value = QString::fromLatin1(qgetenv(environment));
        if (value.isEmpty()) return index < 0;
        if (value.size() > 10 || std::any_of(value.begin(), value.end(), [](QChar ch) {
            return ch < QLatin1Char('0') || ch > QLatin1Char('9');
        })) return false;
        bool valid = false;
        const auto number = value.toULongLong(&valid);
        if (!valid || number > std::numeric_limits<std::uint32_t>::max()) return false;
        output = number; return true;
    };
    if (!probeClockOption(QStringLiteral("--probe-world-sequence-index"), "FORGE_WORLD_PROBE_SEQUENCE_INDEX", worldSequence)
        || !probeClockOption(QStringLiteral("--probe-world-elapsed-ms"), "FORGE_WORLD_PROBE_ELAPSED_MS", worldElapsed)
        || !probeClockOption(QStringLiteral("--probe-world-global-ms"), "FORGE_WORLD_PROBE_GLOBAL_MS", worldGlobal)) return 1;
    const bool probeClockRequested = worldSequence || worldElapsed || worldGlobal;
    if (probeClockRequested && (!worldSequence || !worldElapsed || !worldGlobal || worldFocusHostKey.isEmpty()
        || !headlessProbe || !frameOnlyWorldProbe || modelListOnlyWorldProbe || passiveWorldProbe)) return 1;
    const auto worldSequenceIndex = worldSequence
        ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*worldSequence)) : std::nullopt;
    if (modelListOnlyWorldProbe && (arguments.count(QStringLiteral("--probe-world-model-list-only")) != 1
        || arguments.count(QStringLiteral("--probe-world")) != 1
        || arguments.count(QStringLiteral("--probe-world-output")) != 1 || worldOption < 0
        || worldOutputOption < 0 || passiveWorldProbe || frameOnlyWorldProbe || !headlessProbe)) return 1;
    if (frameOnlyWorldProbe && (arguments.count(QStringLiteral("--probe-world-frame-only")) != 1
        || arguments.count(QStringLiteral("--probe-world")) != 1
        || arguments.count(QStringLiteral("--probe-world-output")) != 1 || worldOption < 0
        || worldOutputOption < 0 || passiveWorldProbe || !headlessProbe)) return 1;
    if (worldOutputOption >= 0 && (worldOption < 0 || passiveWorldProbe || !headlessProbe
        || worldOutputOption + 1 >= arguments.size()
        || arguments[worldOutputOption + 1].isEmpty()
        || arguments[worldOutputOption + 1].startsWith(QStringLiteral("--")))) return 1;
    int worldSeconds = 0, worldTimeoutSeconds = 180;
    if (worldOption >= 0) {
        bool valid = false;
        if (worldOption + 1 < arguments.size()) worldSeconds = arguments[worldOption + 1].toInt(&valid);
        if (!headlessProbe || !valid || worldSeconds < 0 || worldSeconds > 3600) return 1;
    }
    if (frameOnlyWorldProbe && worldSeconds != 0) return 1;
    if (modelListOnlyWorldProbe && worldSeconds != 0) return 1;
    if (passiveWorldProbe && worldOption < 0) return 1;
    if (worldTimeoutOption >= 0) {
        bool valid = false;
        if (worldTimeoutOption + 1 < arguments.size()) worldTimeoutSeconds = arguments[worldTimeoutOption + 1].toInt(&valid);
        if (worldOption < 0 || !valid || worldTimeoutSeconds < 1 || worldTimeoutSeconds > 180) return 1;
    }
    if (headlessProbe && arguments.contains(QStringLiteral("--measure-world"))) return 1;

    // 启动探针不显示窗口，也不进入事件循环。同步完成打开和析构可以避免
    // QCoreApplication::exit() 与工作区初始化中的 Qt 对象生命周期交叠。
    if (headlessProbe) {
        // 取 "--name value" 形式的路径参数；缺值返回空路径。
        const auto pathOption = [&arguments](QStringView name) {
            const qsizetype at = arguments.indexOf(name.toString());
            return at >= 0 && at + 1 < arguments.size()
                ? std::filesystem::path(arguments[at + 1].toStdWString()) : std::filesystem::path{};
        };
        const std::filesystem::path workspace = pathOption(u"--workspace");
        const std::filesystem::path archive = pathOption(u"--open-map");
        const std::filesystem::path editorProject = pathOption(u"--open-project");
        const int inputCount = static_cast<int>(!workspace.empty()) + static_cast<int>(!archive.empty())
            + static_cast<int>(!editorProject.empty());
        const bool startupCapture = captureUiRequested && inputCount == 0 && !requireLoadedWorkspace && worldOption < 0;
        if (inputCount != 1 && !startupCapture) {
            return 1;
        }
        if (captureUiRequested)
            War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
        MainWindow probeWindow(startupCapture ? MainWindow::StartupMode::StartupProbe : MainWindow::StartupMode::HeadlessProbe);
        // 资源根只在本次探针内生效，不写入用户设置。
        if (const auto uiRoot = pathOption(u"--ui-root"); !uiRoot.empty() && !probeWindow.UseUiDataRoot(uiRoot)) {
            return 3;
        }
        if (const auto gameRoot = pathOption(u"--game-root"); !gameRoot.empty() && !probeWindow.OpenGameResources(gameRoot)) {
            return 3;
        }
        if (!workspace.empty()) {
            std::error_code directoryError;
            if (!std::filesystem::is_directory(workspace, directoryError) || directoryError) {
                return 1;
            }
            if (!probeWindow.OpenWorkspacePath(workspace)) {
                return 1;
            }
        } else if (!archive.empty() && !probeWindow.OpenMapArchivePath(archive)) {
            return 1;
        } else if (!editorProject.empty() && !probeWindow.OpenEditorProjectPath(editorProject)) {
            return 1;
        }
        if (arguments.contains(QStringLiteral("--print-summary"))) {
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : probeWindow.LoadedStateSummary()) out << line << '\n';
        }
        if (requireLoadedWorkspace &&
            !probeWindow.HasLoadedTriggerAndObjectModels()) {
            return 2;
        }
        // 验收：编辑触发器后另存副本（含游戏脚本重建与语法检查）。
        const auto saveCopy = pathOption(u"--probe-edit-save");
        const bool saveInPlace = QCoreApplication::arguments().contains(QStringLiteral("--probe-edit-save-in-place"));
        const auto mapDataCopy = pathOption(u"--probe-map-data-save");
        const auto projectSave = pathOption(u"--probe-project-save");
        const auto projectWithData = pathOption(u"--probe-map-data-project");
        const auto objectCopy = pathOption(u"--probe-object-save");
        if (runtimeSaveProbe) {
            if (arguments.count(QStringLiteral("--probe-runtime-save")) != 1 || archive.empty() || captureUiRequested || worldOption >= 0
                || !saveCopy.empty() || saveInPlace || !mapDataCopy.empty() || !projectSave.empty() || !projectWithData.empty() || !objectCopy.empty()) return 1;
            const auto textOption = [&](const QString& name) { const auto index = arguments.indexOf(name);
                return arguments.count(name) == 1 && index + 1 < arguments.size() ? arguments[index + 1] : QString{}; };
            const auto package = textOption(QStringLiteral("--runtime-package-sha256"));
            const auto source = textOption(QStringLiteral("--runtime-source-sha256"));
            if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(package).hasMatch()
                || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(source).hasMatch()) return 1;
            QStringList log;
            const bool saved = probeWindow.ProbeRuntimeSave(pathOption(u"--probe-runtime-save"), package, source, log);
            QTextStream out(stdout); out.setEncoding(QStringConverter::Utf8);
            for (const auto& line : log) out << line << '\n';
            return saved ? 0 : 4;
        }
        if (!objectCopy.empty() && (!saveCopy.empty() || saveInPlace || !mapDataCopy.empty() || !projectSave.empty()
            || !projectWithData.empty())) return 1;
        if (!objectCopy.empty()) {
            QStringList log;
            const bool saved = probeWindow.ProbeObjectEditAndSave(objectCopy, log);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!saved) return 4;
        }
        if ((!mapDataCopy.empty() && (!saveCopy.empty() || saveInPlace || !projectSave.empty()))
            || (!projectSave.empty() && (!saveCopy.empty() || saveInPlace))
            || (!projectWithData.empty() && (!saveCopy.empty() || saveInPlace || !mapDataCopy.empty() || !projectSave.empty()))) return 1;
        if (!projectWithData.empty()) {
            QStringList log;
            const bool saved = probeWindow.ProbeMapDataEditAndSave(projectWithData, log, true);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!saved) return 4;
        }
        if (!projectSave.empty()) {
            const bool saved = probeWindow.SaveEditorProjectToPath(projectSave);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            out << "probe-project-saved=" << (saved ? "true" : "false") << '\n';
            if (!saved) return 4;
        }
        if (!mapDataCopy.empty()) {
            QStringList log;
            const bool saved = probeWindow.ProbeMapDataEditAndSave(mapDataCopy, log);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!saved) return 4;
        }
        if (const auto captureDir = pathOption(u"--capture-ui"); !captureDir.empty()) {
            QStringList log;
            const bool captured = probeWindow.CaptureUi(captureDir, log);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!captured) return 4;
        }
        if (worldOption >= 0) {
            QStringList log;
            const bool probed = probeWindow.ProbeWorldInteraction(worldSeconds, log, passiveWorldProbe, worldTimeoutSeconds,
                pathOption(u"--probe-world-output"), frameOnlyWorldProbe, modelListOnlyWorldProbe, worldFocusHostKey,
                worldSequenceIndex, worldElapsed, worldGlobal);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!probed) return 4;
        }
        if (!saveCopy.empty() || saveInPlace) {
            QStringList log;
            const bool saved = probeWindow.ProbeEditAndSave(saveInPlace ? std::filesystem::path{} : saveCopy, log);
            QTextStream out(stdout);
            out.setEncoding(QStringConverter::Utf8);
            for (const QString& line : log) out << line << '\n';
            if (!saved) return 4;
        }
        return 0;
    }

    // 正常宿主先取得唯一队列，之后的主窗/字体启动异常才能同步落草稿。探针不进入这里。
    if (!arguments.contains(QStringLiteral("--measure-world"))) {
        const auto feedback = War3Map::Mcp::ApplicationFeedbackQueue();
        if (feedback.Succeeded()) {
            application.SetProgramIssueReporter(std::make_shared<War3Map::QtFeedback::ProgramIssueReporter>(*feedback.value,
                War3Map::Feedback::ApplicationInfo{std::string(War3Map::BuildInfo::Version),
                    std::string(War3Map::BuildInfo::Commit), {}, {}}));
        }
    }
    // 自带界面 / 代码字体（程序目录 fonts）；缺失时回退系统字体。启动探针不需要字体。
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()),
        QSettings().value(QString::fromLatin1(War3Map::UiKit::kUiFontFamilySettingsKey)).toString());

    const qsizetype measurementOption = arguments.indexOf(QStringLiteral("--measure-world"));
    const qsizetype backgroundOption = arguments.indexOf(QStringLiteral("--measure-world-background"));
    const bool cameraTour = arguments.contains(QStringLiteral("--measure-world-camera-tour"));
    if (measurementOption >= 0 && (measurementOption + 1 >= arguments.size()
        || arguments[measurementOption + 1].startsWith(QStringLiteral("--"))
        || !arguments.contains(QStringLiteral("--open-map")))) return 1;
    if (backgroundOption >= 0 && (measurementOption < 0 || backgroundOption + 1 >= arguments.size())) return 1;
    bool backgroundValid = true;
    const int backgroundSeconds = backgroundOption >= 0 ? arguments[backgroundOption + 1].toInt(&backgroundValid) : 0;
    if (!backgroundValid || (backgroundOption >= 0 && (backgroundSeconds < 1 || backgroundSeconds > 3600))) return 1;
    if (cameraTour && backgroundOption < 0) return 1;
    MainWindow window(measurementOption >= 0 ? MainWindow::StartupMode::HeadlessProbe : MainWindow::StartupMode::Normal);
    // 与探针共用会话级 UI 根；不改作者全局设置，旧版目录同样由 Catalog 解析。
    const qsizetype uiRootOption = arguments.indexOf(QStringLiteral("--ui-root"));
    if (uiRootOption >= 0 && uiRootOption + 1 < arguments.size()) {
        if (!window.UseUiDataRoot(std::filesystem::path(arguments[uiRootOption + 1].toStdWString()))) return 3;
    }
    const qsizetype gameRootOption = arguments.indexOf(QStringLiteral("--game-root"));
    if (gameRootOption >= 0 && gameRootOption + 1 < arguments.size()) {
        if (!window.OpenGameResources(std::filesystem::path(arguments[gameRootOption + 1].toStdWString()))) return 3;
    }
    if (backgroundOption >= 0) {
        window.setAttribute(Qt::WA_ShowWithoutActivating);
        window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
        window.resize(1600, 1000);
        // 依据当前所有显示器边界选择桌面外位置，避免固定坐标落到作者的副屏。
        QRect desktop;
        for (const auto* screen : QGuiApplication::screens()) desktop = desktop.united(screen->geometry());
        window.move(desktop.left() - window.width() - 256, desktop.top() - window.height() - 256);
    }
    window.show();
    if (healthRequested) {
        // 真实事件循环开始后，主窗口与服务已经构造；不能在构造之前宣称启动健康。
        QTimer::singleShot(0, &window, [healthTransaction, healthChallenge, healthPipe] {
            const auto reply = War3Map::Update::ReplyUpdateHealth(static_cast<std::uintptr_t>(healthPipe),
                healthTransaction.toStdString(), healthChallenge.toStdString(), War3Map::BuildInfo::Version, War3Map::BuildInfo::Commit);
            if (!reply.Succeeded()) qWarning("UPDATE_HEALTH_REPLY_FAILED");
        });
    }

    const qsizetype mapOption = arguments.indexOf(QStringLiteral("--open-map"));
    if (mapOption >= 0 && mapOption + 1 < arguments.size()) {
        const std::filesystem::path archive(
            arguments[mapOption + 1].toStdWString());
        const QString measurementDirectory = measurementOption >= 0 ? arguments[measurementOption + 1] : QString{};
        const bool userOpen = measurementDirectory.isEmpty() && backgroundSeconds == 0 && !healthRequested;
        QTimer::singleShot(0, &window, [&window, archive, measurementDirectory, backgroundSeconds, cameraTour, userOpen]() {
            if (!(userOpen ? window.OpenUserMapArchivePath(archive) : window.OpenMapArchivePath(archive))) {
                if (!measurementDirectory.isEmpty()) QCoreApplication::exit(4);
                return;
            }
            if (!measurementDirectory.isEmpty() && !window.EnableWorldMeasurement(measurementDirectory, backgroundSeconds, cameraTour))
                QCoreApplication::exit(4);
        });
    }
    if (workspaceOption >= 0 && workspaceOption + 1 < arguments.size()) {
        const std::filesystem::path workspace(
            arguments[workspaceOption + 1].toStdWString());
        QTimer::singleShot(0, &window, [&window, workspace]() {
            window.OpenWorkspacePath(workspace);
        });
    }
    const auto result = application.exec();
    return application.HadUnhandledException()
        ? War3Map::QtFeedback::ProgramIssueApplication::kUnhandledExceptionExitCode : result;
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        War3Map::QtFeedback::ProgramIssueApplication application(argc, argv);
        try {
            const auto result = RunApplication(application);
            return application.HadUnhandledException() ? War3Map::QtFeedback::ProgramIssueApplication::kUnhandledExceptionExitCode : result;
        }
        catch (const std::exception&) { application.ReportStartupException(true); }
        catch (...) { application.ReportStartupException(false); }
    } catch (...) {
        // Qt 应用自身还未构造时没有安全队列；不在故障状态下另起 Qt/网络或声称报告已保存。
    }
    return War3Map::QtFeedback::ProgramIssueApplication::kUnhandledExceptionExitCode;
}
