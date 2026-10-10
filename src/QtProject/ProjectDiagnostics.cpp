#include "EditorProject.h"

#include <QStandardItemModel>

// 诊断列表归工程所有：打开 / 保存 / 编辑各环节写入同一模型，界面只负责显示。
namespace War3Map::QtProject {

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
    AddDiagnostic(tr("信息"), tr("Core"), tr("NO_PROJECT"), tr("打开展开地图后开始只读扫描。"));
}

void EditorProject::ClearDiagnostics() {
    diagnosticsModel_->removeRows(0, diagnosticsModel_->rowCount());
}

void EditorProject::AddDiagnostic(const QString& level, const QString& subsystem,
    const QString& code, const QString& message) {
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

} // namespace War3Map::QtProject
