#include "EditorProject.h"

#include "War3MapCore/Feedback/RunLog.h"

#include <QHash>
#include <QStandardItemModel>

#include <chrono>
#include <unordered_map>

// 诊断列表归工程所有：打开 / 保存 / 编辑各环节写入同一模型，界面只负责显示。
namespace War3Map::QtProject {
namespace {
// 每条诊断同时写进运行日志。代码检查会反复替换同一批诊断：同样的内容 30 秒内只记一次。
void LogDiagnostic(const QString& level, const QString& errorLevel, const QString& warningLevel,
    const QString& subsystem, const QString& code, const QString& message) {
    static std::unordered_map<std::size_t, std::chrono::steady_clock::time_point> recent;
    const auto now = std::chrono::steady_clock::now();
    const std::size_t key = qHash(level + QLatin1Char('|') + subsystem + QLatin1Char('|') + code + QLatin1Char('|') + message);
    if (const auto found = recent.find(key); found != recent.end() && now - found->second < std::chrono::seconds(30)) return;
    if (recent.size() > 4096) recent.clear();
    recent[key] = now;
    const auto severity = level == errorLevel ? RunLog::Level::Error : level == warningLevel ? RunLog::Level::Warning : RunLog::Level::Info;
    RunLog::Write(severity, "诊断", (subsystem + QLatin1Char(' ') + code + QLatin1Char(' ') + message).toStdString());
}
} // namespace

QString EditorProject::DiagnosticLevel(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Info: return tr("信息");
    case DiagnosticSeverity::Warning: return tr("警告");
    case DiagnosticSeverity::Error: return tr("错误");
    }
    return tr("错误");
}

void EditorProject::InitializeDiagnostics() {
    diagnosticsModel_ = new QStandardItemModel(this);
    diagnosticsModel_->setHorizontalHeaderLabels({tr("级别"), tr("子系统"), tr("代码"), tr("说明")});
}

void EditorProject::ClearDiagnostics() {
    diagnosticsModel_->removeRows(0, diagnosticsModel_->rowCount());
}

void EditorProject::AddDiagnostic(const QString& level, const QString& subsystem,
    const QString& code, const QString& message) {
    if (code == QLatin1String("LUA_FOLDER_MISSING")) {
        for (int row = 0; row < diagnosticsModel_->rowCount(); ++row)
            if (diagnosticsModel_->index(row, 2).data().toString() == code) return;
    }
    LogDiagnostic(level, tr("错误"), tr("警告"), subsystem, code, message);
    QList<QStandardItem*> row;
    row << new QStandardItem(level) << new QStandardItem(subsystem)
        << new QStandardItem(code) << new QStandardItem(message);
    // 按级别着色：错误红 / 警告黄 / 信息灰。
    QColor foreground(QStringLiteral("#8b949e"));
    if (level == tr("错误")) {
        foreground = QColor(QStringLiteral("#f85149"));
    } else if (level == tr("警告")) {
        foreground = QColor(QStringLiteral("#e3b341"));
    }
    for (QStandardItem* item : row) {
        item->setForeground(foreground);
    }
    diagnosticsModel_->appendRow(row);
}

void EditorProject::AddDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        auto message = QString::fromUtf8(diagnostic.message);
        if (diagnostic.origin) {
            message += tr("\n位置：%1 · 字节 %2").arg(QString::fromStdWString(diagnostic.origin->source.wstring()))
                .arg(diagnostic.origin->offset);
        }
        AddDiagnostic(DiagnosticLevel(diagnostic.severity), QString::fromUtf8(diagnostic.subsystem),
            QString::fromUtf8(diagnostic.code), message);
    }
}

void EditorProject::ReplaceDiagnostics(const QString& subsystem, std::vector<Diagnostic> diagnostics) {
    for (int row = diagnosticsModel_->rowCount() - 1; row >= 0; --row)
        if (const auto* item = diagnosticsModel_->item(row, 1); item && item->text() == subsystem) diagnosticsModel_->removeRow(row);
    for (auto& diagnostic : diagnostics) diagnostic.subsystem = subsystem.toStdString();
    AddDiagnostics(diagnostics);
}

} // namespace War3Map::QtProject
