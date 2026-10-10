#include "Widgets/CodeProblemsView.h"
#include "Widgets/CodeView.h"
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace War3Map::UiKit {
CodeProblemsView::CodeProblemsView(QWidget* parent) : QWidget(parent) {
    setObjectName("codeProblemsView");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    status_ = new QLabel(tr("未检查"), this); status_->setObjectName("codeCheckStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    issues_ = new QTreeWidget(this); issues_->setObjectName("codeIssues");
    issues_->setHeaderLabels({tr("级别"), tr("来源 / 位置"), tr("问题")}); issues_->setRootIsDecorated(false);
    issues_->setUniformRowHeights(true); issues_->setMaximumHeight(150);
    issues_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents); issues_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    issues_->header()->setSectionResizeMode(2, QHeaderView::Stretch); issues_->hide(); layout->addWidget(issues_);
    connect(issues_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        emit LocationRequested(item->data(0, Qt::UserRole).toString(), item->data(1, Qt::UserRole).toInt(), item->data(2, Qt::UserRole).toInt());
    });
}
void CodeProblemsView::SetView(CodeView* view) {
    const auto refresh = [this, view] { SetResult(view->CheckResult(), view->IsChecking(), view->DecodeNotice()); };
    connect(view, &CodeView::CheckStateChanged, this, refresh);
    connect(view, &CodeView::TextLoaded, this, refresh);
    connect(this, &CodeProblemsView::LocationRequested, view, [view](const QString& source, int line, int column) {
        if (source == view->SourceName()) view->GoToLine(line, column);
        else emit view->JumpRequested({}, source, line, column);
    });
    refresh();
}
void CodeProblemsView::SetResult(const CodeCheckResult& result, bool checking, const QString& decodeNotice) {
    QString status;
    if (checking) status = tr("检查中…");
    else if (!result.completed) status = result.cancelled ? tr("检查已取消") : tr("未检查");
    else {
        int errors = 0, warnings = 0;
        for (const auto& issue : result.diagnostics) {
            if (issue.severity == CodeSeverity::Error) ++errors;
            else if (issue.severity == CodeSeverity::Warning) ++warnings;
        }
        const auto scope = result.semanticChecked
            ? (result.contextComplete ? tr("语法与语义检查") : tr("语法检查与部分语义提示"))
            : result.syntaxChecked ? tr("仅语法检查，声明上下文不完整") : tr("检查未完成");
        status = scope + tr(" · %1 错误，%2 警告").arg(errors).arg(warnings);
    }
    if (!result.reason.isEmpty()) status += QStringLiteral(" · ") + result.reason;
    if (!decodeNotice.isEmpty()) status += QStringLiteral(" · ") + decodeNotice;
    if (!result.externalSourcesComplete) status += tr(" · 导航来源清单不完整");
    status_->setText(status);
    issues_->clear();
    int shown = 0;
    for (const auto& issue : result.diagnostics) {
        if (shown++ >= 512) break;
        const auto severity = issue.severity == CodeSeverity::Error ? tr("错误") : issue.severity == CodeSeverity::Warning ? tr("警告") : tr("信息");
        const auto location = issue.line > 0 ? tr("%1:%2:%3").arg(issue.source).arg(issue.line).arg(issue.column) : issue.source;
        auto* item = new QTreeWidgetItem(issues_, {severity, location, issue.message});
        item->setToolTip(2, issue.code + QStringLiteral("\n") + issue.message);
        item->setData(0, Qt::UserRole, issue.source); item->setData(1, Qt::UserRole, issue.line); item->setData(2, Qt::UserRole, issue.column);
    }
    if (result.diagnostics.size() > 512) status_->setText(status_->text() + tr(" · 列表显示前 512 项"));
    issues_->setVisible(!result.diagnostics.isEmpty());
}
QString CodeProblemsView::StatusText() const { return status_->text(); }
int CodeProblemsView::IssueCount() const { return issues_->topLevelItemCount(); }
}
