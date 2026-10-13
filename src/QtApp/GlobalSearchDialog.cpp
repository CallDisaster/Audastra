#include "GlobalSearchDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <array>

namespace War3Map::QtApp {
namespace {
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Category(Services::ProjectSearchCategory category) {
    using C = Services::ProjectSearchCategory;
    switch (category) {
    case C::Trigger: return QObject::tr("触发器与参数");
    case C::Variable: return QObject::tr("变量");
    case C::MapObject: return QObject::tr("地图物体");
    case C::StandardObject: return QObject::tr("标准物体");
    case C::CodeSymbol: return QObject::tr("代码声明");
    case C::Sound: return QObject::tr("声音");
    case C::ImportedFile: return QObject::tr("资源");
    case C::UnitPlacement: return QObject::tr("单位摆放");
    case C::DoodadPlacement: return QObject::tr("地物摆放");
    case C::Region: return QObject::tr("区域");
    case C::Camera: return QObject::tr("镜头");
    default: return QObject::tr("其他");
    }
}
} // namespace
GlobalSearchDialog::GlobalSearchDialog(std::function<std::string()> revision, Query query,
    std::function<void(const Services::ProjectSearchRow&)> activate, QWidget* parent)
    : QDialog(parent), revision_(std::move(revision)), query_(std::move(query)), activate_(std::move(activate)) {
    setObjectName(QStringLiteral("globalSearchDialog"));
    setWindowTitle(tr("全局搜索 · Audastra"));
    setModal(false);
    resize(820, 560);
    setMinimumSize(640, 420);
    auto* layout = new QVBoxLayout(this);
    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("globalSearchQuery"));
    edit_->setMaxLength(256);
    edit_->setPlaceholderText(tr("搜索名称、ID 或触发器参数"));
    layout->addWidget(edit_);
    results_ = new QTreeWidget(this);
    results_->setObjectName(QStringLiteral("globalSearchResults"));
    results_->setColumnCount(2);
    results_->setHeaderLabels({tr("名称或参数"), tr("位置")});
    results_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    results_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    results_->setColumnWidth(1, 220);
    results_->setRootIsDecorated(true);
    results_->setUniformRowHeights(true);
    layout->addWidget(results_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("globalSearchStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (auto* close = buttons->button(QDialogButtonBox::Close)) close->setText(tr("关闭"));
    previous_ = buttons->addButton(tr("上一页"), QDialogButtonBox::ActionRole);
    next_ = buttons->addButton(tr("下一页"), QDialogButtonBox::ActionRole);
    previous_->setObjectName(QStringLiteral("globalSearchPrevious"));
    next_->setObjectName(QStringLiteral("globalSearchNext"));
    previous_->setEnabled(false); next_->setEnabled(false);
    previous_->setAutoDefault(false); next_->setAutoDefault(false);
    for (auto* button : buttons->findChildren<QPushButton*>()) button->setAutoDefault(false);
    layout->addWidget(buttons);
    timer_ = new QTimer(this); timer_->setSingleShot(true); timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, [this]() { Refresh(true); });
    connect(edit_, &QLineEdit::textChanged, this, [this]() {
        ++requestGeneration_;
        dirty_ = true;
        results_->clear(); rows_.clear(); starts_.clear(); nextCursor_.reset();
        previous_->setEnabled(false); next_->setEnabled(false); timer_->start();
    });
    connect(edit_, &QLineEdit::returnPressed, this, [this]() {
        const QPointer<GlobalSearchDialog> dialog(this);
        if (timer_->isActive() || dirty_) { timer_->stop(); Refresh(true); }
        if (dialog) dialog->ActivateCurrent();
    });
    connect(results_, &QTreeWidget::itemActivated, this, [this]() { ActivateCurrent(); });
    connect(previous_, &QPushButton::clicked, this, [this]() { if (pageIndex_) { --pageIndex_; Refresh(false); } });
    connect(next_, &QPushButton::clicked, this, [this]() {
        if (!nextCursor_) return;
        starts_.resize(pageIndex_ + 1);
        starts_.push_back(nextCursor_); ++pageIndex_; Refresh(false);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
}
void GlobalSearchDialog::FocusQuery(const QString& text) {
    if (!text.isEmpty()) edit_->setText(text);
    show(); raise(); activateWindow(); edit_->setFocus(); edit_->selectAll();
}
void GlobalSearchDialog::ContextChanged() {
    ++requestGeneration_;
    dirty_ = true;
    timer_->stop(); rows_.clear(); results_->clear(); starts_.clear(); nextCursor_.reset();
    previous_->setEnabled(false); next_->setEnabled(false);
    status_->setText(tr("内容已变化，请重新搜索"));
}
void GlobalSearchDialog::Refresh(bool reset) {
    if (!revision_ || !query_) return;
    const auto generation = ++requestGeneration_;
    const QPointer<GlobalSearchDialog> dialog(this);
    const auto getRevision = revision_;
    const auto query = query_;
    if (reset) {
        const auto text = edit_->text().toUtf8().toStdString();
        const auto revision = getRevision();
        if (!dialog || requestGeneration_ != generation) return;
        boundRevision_ = revision; boundQuery_ = text;
        starts_ = {std::nullopt}; pageIndex_ = 0;
    }
    rows_.clear(); results_->clear(); nextCursor_.reset();
    previous_->setEnabled(false); next_->setEnabled(false);
    if (pageIndex_ >= starts_.size()) return;
    const auto start = starts_[pageIndex_];
    const auto boundRevision = boundRevision_, boundQuery = boundQuery_;
    Services::ProjectSearchOptions options; options.limit = 128;
    const auto result = query(boundRevision, boundQuery, options, 40, start ? &*start : nullptr);
    if (!dialog || requestGeneration_ != generation) return;
    if (!result.Succeeded() || !result.value) {
        starts_.clear();
        dirty_ = true;
        status_->setText(result.diagnostics.empty() ? tr("查询失败，请重新搜索。") : Text(result.diagnostics.front().message));
        return;
    }
    Populate(*result.value);
    dirty_ = false;
}
void GlobalSearchDialog::Populate(const Services::ProjectSearchPage& page) {
    rows_ = page.rows; nextCursor_ = page.next;
    std::array<QTreeWidgetItem*, static_cast<std::size_t>(Services::ProjectSearchCategory::Count)> groups{};
    QTreeWidgetItem* first = nullptr;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        const auto category = static_cast<std::size_t>(row.category);
        if (category >= groups.size()) continue;
        auto*& group = groups[category];
        if (!group) {
            group = new QTreeWidgetItem(results_, {Category(row.category)});
            group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
            group->setExpanded(true);
        }
        auto* item = new QTreeWidgetItem(group, {Text(row.title), Text(row.detail)});
        item->setData(0, Qt::UserRole, static_cast<int>(i));
        const auto tooltip = Text(row.stableId) + (row.rawValue ? QStringLiteral("\n") + Text(*row.rawValue) : QString{});
        item->setToolTip(0, QStringLiteral("<qt>") + tooltip.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")) + QStringLiteral("</qt>"));
        if (!first) first = item;
    }
    if (first) results_->setCurrentItem(first);
    previous_->setEnabled(pageIndex_ > 0); next_->setEnabled(nextCursor_.has_value());
    QStringList notices;
    notices << tr("第 %1 页 · 当前排序窗口 %2 项 · 每页最多 40 项").arg(pageIndex_ + 1).arg(page.totalWindowRows);
    if (page.windowHasMore) notices << tr("窗口外还有匹配项；请缩小查询。窗口最多 128 项。");
    QStringList incomplete;
    bool queryTooShort = false;
    for (const auto& coverage : page.coverage) {
        queryTooShort = queryTooShort || coverage.state == Services::ProjectSearchState::QueryTooShort;
        if (coverage.state != Services::ProjectSearchState::Ready && coverage.state != Services::ProjectSearchState::QueryTooShort)
            incomplete << Category(coverage.category);
    }
    if (queryTooShort) notices << tr("搜索文字过短，请增加名称或 ID。");
    if (!incomplete.isEmpty()) notices << tr("部分内容未载入或覆盖有限：%1").arg(incomplete.join(QStringLiteral("、")));
    if (rows_.empty()) notices << tr("当前窗口没有匹配项。");
    status_->setText(notices.join(QLatin1Char('\n')));
}
void GlobalSearchDialog::ActivateCurrent() {
    const auto* item = results_->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid()) return;
    const auto index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || static_cast<std::size_t>(index) >= rows_.size()) return;
    const QPointer<GlobalSearchDialog> dialog(this);
    const auto revision = revision_;
    const auto boundRevision = boundRevision_;
    const auto generation = requestGeneration_;
    const auto row = rows_[static_cast<std::size_t>(index)];
    const auto callback = activate_;
    if (!revision) { ContextChanged(); return; }
    const auto currentRevision = revision();
    if (!dialog || requestGeneration_ != generation) return;
    if (currentRevision != boundRevision) { ContextChanged(); return; }
    if (callback) callback(row);
}
} // namespace War3Map::QtApp
