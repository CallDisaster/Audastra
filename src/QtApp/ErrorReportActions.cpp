#include "MainWindow.h"

#include "ErrorReportDialog.h"
#include "ErrorReportEnvironment.h"
#include "../QtCommon/MapTestDisplayPreference.h"
#include "War3MapCore/Feedback/ErrorReport.h"
#include "War3MapCore/Feedback/RunLog.h"
#include "War3MapCore/IO/FileBytes.h"

#include <QDesktopServices>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QUrl>

#include <algorithm>

extern const char* kWar3MapBuildStamp;

namespace {

namespace fs = std::filesystem;
using Rows = std::vector<std::pair<std::string, std::string>>;

// 目录是否存在，以及能认出的形态（YDWE 六件套 / KKWE 插件配置）。
std::string DirectoryState(const QString& text) {
    if (text.trimmed().isEmpty()) return "未设置";
    const fs::path path(text.trimmed().toStdWString());
    std::error_code error;
    if (!fs::is_directory(path, error)) return text.toStdString() + "（不存在）";
    auto root = path;
    const auto leaf = QString::fromStdWString(root.filename().wstring()).toLower();
    if (leaf == QLatin1String("ui")) root = root.parent_path();
    const auto has = [&](const wchar_t* relative) { return fs::exists(root / relative, error); };
    std::string kind;
    // 同时有 YDWE 的 lua53.dll 与 KKWE 的 luacore.dll，多半是两种编辑器的文件混放，测试时会对不上。
    if (has(L"bin/lua53.dll") && has(L"bin/luacore.dll")) kind = "，YDWE 与 KKWE 文件混放";
    else if (has(L"bin/lua53.dll") && has(L"plugin/warcraft3/yd_jass_api.dll")) kind = "，YDWE 运行库";
    else if (has(L"bin/ydbase.dll") && has(L"plugin/warcraft3")) kind = "，KKWE 运行库";
    else if (has(L"lua53.dll") && has(L"yd_jass_api.dll")) kind = "，独立六件套运行库";
    return text.toStdString() + kind;
}

std::string GameState(const QString& text) {
    if (text.trimmed().isEmpty()) return "未设置";
    const fs::path path(text.trimmed().toStdWString());
    std::error_code error;
    if (!fs::is_directory(path, error)) return text.toStdString() + "（不存在）";
    std::string state = text.toStdString();
    for (const auto* name : {L"war3.exe", L"Game.dll"}) {
        const auto version = War3Map::QtFeedback::FileVersion(path / name);
        state += "；" + War3Map::IO::PathUtf8(fs::path(name)) + " " + (version.isEmpty()
            ? (fs::exists(path / name, error) ? std::string("无版本信息") : std::string("缺失")) : version.toStdString());
    }
    return state;
}

} // namespace

War3Map::Feedback::ErrorReportInput MainWindow::CollectErrorReport() const {
    War3Map::Feedback::ErrorReportInput input;
    input.sections = War3Map::QtFeedback::CollectErrorReportEnvironment(QString::fromUtf8(kWar3MapBuildStamp));
    const QSettings settings;
    Rows editor;
    editor.emplace_back("游戏目录", GameState(settings.value(QStringLiteral("war3/gameRoot")).toString()));
    editor.emplace_back("YDWE / KKWE 目录", DirectoryState(settings.value(QStringLiteral("ui/dataRoot")).toString()));
    editor.emplace_back("测试运行库目录", DirectoryState(settings.value(QStringLiteral("mapTest/runtimeRoot")).toString()));
    const auto dependencies = settings.value(QStringLiteral("mapTest/externalDependenciesDirectory")).toString();
    editor.emplace_back("外部依赖目录", dependencies.trimmed().isEmpty() ? std::string("默认") : DirectoryState(dependencies));
    editor.emplace_back("测试 Lua 环境", settings.value(QStringLiteral("mapTest/luaEngine")).toString() == QLatin1String("ydwe")
        ? "YDWE 引擎" : "自带环境");
    const auto display = War3Map::QtCommon::ReadMapTestDisplay(settings);
    editor.emplace_back("测试窗口", std::string(display.windowMode == War3Map::Services::MapTestWindowMode::Borderless ? "无边框窗口化"
        : display.windowMode == War3Map::Services::MapTestWindowMode::Fullscreen ? "全屏" : "窗口化")
        + (display.renderer == War3Map::Services::MapTestRenderer::OpenGL ? "，OpenGL" : "，Direct3D")
        + (display.cursorLock ? "，鼠标限制在窗口内" : ""));
    editor.emplace_back("界面主题", settings.value(QStringLiteral("ui/theme")).toString().toStdString());
    input.sections.push_back({"编辑器设置", std::move(editor)});
    Rows map;
    const auto& source = project_->sourceArchivePath_;
    if (source.empty()) {
        map.emplace_back("地图", "未打开");
    } else {
        std::error_code error;
        const auto size = fs::file_size(source, error);
        map.emplace_back("地图", War3Map::IO::PathUtf8(source));
        map.emplace_back("大小", error ? std::string("无法读取") : std::to_string(size) + " 字节");
        map.emplace_back("未保存的修改", isWindowModified() ? "有" : "无");
    }
    const auto module = CurrentModule();
    map.emplace_back("当前页面", module ? *module : std::string("无"));
    input.sections.push_back({"当前地图", std::move(map)});
    if (const auto* model = project_->diagnosticsModel_) {
        // 诊断列表全文（最近 2000 条）。
        for (int row = std::max(0, model->rowCount() - 2000); row < model->rowCount(); ++row) {
            QStringList fields;
            for (int column = 0; column < model->columnCount(); ++column) fields << model->index(row, column).data().toString();
            input.diagnostics.push_back(fields.join(QStringLiteral(" · ")).replace(QLatin1Char('\n'), QStringLiteral(" / ")).toStdString());
        }
    }
    // 日志：本次运行（含上一段），上次没有正常退出的那次；最近两个崩溃转储。
    const auto session = War3Map::RunLog::SessionFiles();
    for (std::size_t i = 0; i < session.size(); ++i)
        input.files.push_back({i + 1 == session.size() ? "logs/current.log" : "logs/current-earlier.log", session[i], true});
    const auto unclean = War3Map::RunLog::UncleanPreviousRunFiles();
    for (std::size_t i = 0; i < unclean.size(); ++i)
        input.files.push_back({i + 1 == unclean.size() ? "logs/last-unclean-run.log" : "logs/last-unclean-run-earlier.log", unclean[i], true});
    for (const auto& dump : War3Map::QtFeedback::RecentCrashDumps(War3Map::RunLog::Directory(), 2))
        input.files.push_back({"dumps/" + War3Map::IO::PathUtf8(dump.filename()), dump, false});
    return input;
}

void MainWindow::ShowErrorReport() {
    War3Map::RunLog::Info("错误报告", "打开导出窗口");
    War3Map::QtFeedback::ErrorReportDialog dialog([this]() { return CollectErrorReport(); },
        QString::fromStdWString(War3Map::RunLog::Directory().wstring()), this);
    dialog.exec();
}

void MainWindow::OpenLogFolder() {
    const auto directory = War3Map::RunLog::Directory();
    if (directory.empty()) {
        ShowStatusMessage(tr("本次运行没有日志。"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(directory.wstring())));
}

void MainWindow::OfferErrorReportAfterUncleanExit() {
    if (startupMode_ != StartupMode::Normal || War3Map::RunLog::UncleanPreviousRunFiles().empty()) return;
    QMessageBox box(QMessageBox::Warning, tr("上次运行没有正常退出"), tr("上次运行没有正常退出（崩溃或被结束）。"),
        QMessageBox::NoButton, this);
    auto* report = box.addButton(tr("导出错误报告…"), QMessageBox::AcceptRole);
    box.addButton(tr("忽略"), QMessageBox::RejectRole);
    box.setDefaultButton(report);
    box.exec();
    if (box.clickedButton() == report) ShowErrorReport();
}
