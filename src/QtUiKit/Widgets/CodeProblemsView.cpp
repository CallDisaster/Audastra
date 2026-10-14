#include "Widgets/CodeProblemsView.h"
#include "Widgets/CodeView.h"
#include "Widgets/ProblemList.h"
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace War3Map::UiKit {
namespace {
constexpr int kSeverityRole = Qt::UserRole + 1;
constexpr int kSourceRole = Qt::UserRole + 2;
constexpr int kLineRole = Qt::UserRole + 3;
constexpr int kColumnRole = Qt::UserRole + 4;
}

CodeProblemsView::CodeProblemsView(QWidget* parent) : QWidget(parent) {
    setObjectName("codeProblemsView");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({tr("级别"), tr("位置"), tr("问题")});
    list_ = new ProblemList(this);
    list_->SetSourceModel(model_, [](const QAbstractItemModel& model, int row) {
        return static_cast<ProblemSeverity>(model.index(row, 0).data(kSeverityRole).toInt());
    });
    auto* view = list_->View();
    view->setMaximumHeight(150);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum); // 代码编辑区优先占高度
    view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    view->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(list_);
    connect(list_, &ProblemList::Activated, this, [this](const QModelIndex& index) {
        const auto first = model_->index(index.row(), 0);
        const auto source = first.data(kSourceRole).toString();
        if (!source.isEmpty()) emit LocationRequested(source, first.data(kLineRole).toInt(), first.data(kColumnRole).toInt());
    });
}
void CodeProblemsView::SetView(CodeView* view) {
    const auto refresh = [this, view] {
        const auto& result = view->CheckResult();
        const bool checking = view->IsChecking();
        // 同一文件编辑后到新结果出来前保留上次的列表（与 VS 相同），避免每输入一个字符列表收起又展开、编辑区跳动。
        if (checking && !result.completed && !shownSource_.isEmpty() && shownSource_ == view->SourceName()) {
            status_ = tr("检查中…"); list_->SetState(status_);
            return;
        }
        SetResult(result, checking, view->DecodeNotice());
        shownSource_ = result.completed ? view->SourceName() : QString();
    };
    connect(view, &CodeView::CheckStateChanged, this, refresh);
    connect(view, &CodeView::TextLoaded, this, refresh);
    connect(this, &CodeProblemsView::LocationRequested, view, [view](const QString& source, int line, int column) {
        if (source == view->SourceName()) view->GoToLine(line, column);
        else emit view->JumpRequested({}, source, line, column);
    });
    refresh();
}
void CodeProblemsView::SetResult(const CodeCheckResult& result, bool checking, const QString& decodeNotice) {
    model_->removeRows(0, model_->rowCount());
    issues_ = 0;
    const auto add = [this](ProblemSeverity severity, const QString& location, const QString& message,
                         const CodeDiagnostic* issue) {
        const auto level = severity == ProblemSeverity::Error ? tr("错误") : severity == ProblemSeverity::Warning ? tr("警告") : tr("消息");
        QList<QStandardItem*> row{new QStandardItem(level), new QStandardItem(location), new QStandardItem(message)};
        for (auto* item : row) item->setEditable(false);
        row[0]->setData(static_cast<int>(severity), kSeverityRole);
        if (issue) {
            row[0]->setData(issue->source, kSourceRole); row[0]->setData(issue->line, kLineRole); row[0]->setData(issue->column, kColumnRole);
            row[2]->setToolTip(issue->code + QStringLiteral("\n") + issue->message);
        }
        model_->appendRow(row);
    };
    int errors = 0, warnings = 0;
    int shown = 0;
    for (const auto& issue : result.diagnostics) {
        if (issue.severity == CodeSeverity::Error) ++errors;
        else if (issue.severity == CodeSeverity::Warning) ++warnings;
        if (shown++ >= 512) continue;
        const auto severity = issue.severity == CodeSeverity::Error ? ProblemSeverity::Error
            : issue.severity == CodeSeverity::Warning ? ProblemSeverity::Warning : ProblemSeverity::Info;
        const auto location = issue.line > 0 ? tr("%1:%2:%3").arg(issue.source).arg(issue.line).arg(issue.column) : issue.source;
        add(severity, location, issue.message, &issue);
        ++issues_;
    }
    // 检查范围与来源情况是“消息”，和问题一起在列表里，按开关筛选。
    if (result.completed && result.reason.isEmpty()) {
        if (!result.semanticChecked) add(ProblemSeverity::Info, tr("检查"), result.syntaxChecked ? tr("仅语法检查") : tr("检查未完成"), nullptr);
        else if (!result.contextComplete) add(ProblemSeverity::Info, tr("检查"), tr("声明上下文不完整，部分语义未检查"), nullptr);
    }
    if (!result.reason.isEmpty()) add(ProblemSeverity::Info, tr("检查"), result.reason, nullptr);
    if (!decodeNotice.isEmpty()) add(ProblemSeverity::Info, tr("编码"), decodeNotice, nullptr);
    if (!result.externalSourcesComplete) add(ProblemSeverity::Info, tr("导航"), tr("来源清单不完整"), nullptr);
    if (result.diagnostics.size() > 512) add(ProblemSeverity::Info, tr("列表"), tr("只列出前 512 项"), nullptr);

    if (checking) status_ = tr("检查中…");
    else if (!result.completed) status_ = result.cancelled ? tr("检查已取消") : tr("未检查");
    else status_ = tr("%1 错误 · %2 警告").arg(errors).arg(warnings);
    list_->SetState(checking || !result.completed ? status_ : QString{});
    // 没有条目时只留开关行，代码编辑区占满高度。
    list_->View()->setVisible(model_->rowCount() > 0);
}
QString CodeProblemsView::StatusText() const { return status_; }
int CodeProblemsView::IssueCount() const { return issues_; }
}
