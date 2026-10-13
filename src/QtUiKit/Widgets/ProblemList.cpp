#include "Widgets/ProblemList.h"

#include "Icons/SvgIcons.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace War3Map::UiKit {

class ProblemList::Filter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    SeverityOf severityOf;
    std::array<bool, 3> shown{true, true, true};

    void SetShown(int index, bool on) {
        if (shown[index] == on) return;
        shown[index] = on;
        invalidateRowsFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (parent.isValid() || !severityOf || !sourceModel()) return true;
        return shown[static_cast<int>(severityOf(*sourceModel(), row))];
    }
};

namespace {
struct ToggleSpec { const char* name; const char* icon; const char* color; const char* label; };
constexpr std::array<ToggleSpec, 3> kToggles{{
    {"problemErrors", "lucide/circle-x", "#f85149", QT_TRANSLATE_NOOP("War3Map::UiKit::ProblemList", "错误")},
    {"problemWarnings", "lucide/triangle-alert", "#e3b341", QT_TRANSLATE_NOOP("War3Map::UiKit::ProblemList", "警告")},
    {"problemMessages", "lucide/info", "#58a6ff", QT_TRANSLATE_NOOP("War3Map::UiKit::ProblemList", "消息")},
}};
} // namespace

ProblemList::ProblemList(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("problemList"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    for (std::size_t index = 0; index < kToggles.size(); ++index) {
        auto* toggle = new QToolButton(this);
        toggle->setObjectName(QString::fromLatin1(kToggles[index].name));
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setAutoRaise(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setIcon(SvgIcon(QString::fromLatin1(kToggles[index].icon), QColor(QString::fromLatin1(kToggles[index].color))));
        connect(toggle, &QToolButton::toggled, this, [this, index](bool on) {
            filter_->SetShown(static_cast<int>(index), on);
        });
        toggles_[index] = toggle;
        bar->addWidget(toggle);
    }
    bar->addStretch();
    state_ = new QLabel(this);
    state_->setObjectName(QStringLiteral("problemListState"));
    state_->hide();
    bar->addWidget(state_);
    layout->addLayout(bar);

    filter_ = new Filter(this);
    view_ = new QTreeView(this);
    view_->setObjectName(QStringLiteral("problemListView"));
    view_->setModel(filter_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(false);
    view_->header()->setStretchLastSection(true);
    layout->addWidget(view_, 1);
    connect(view_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        emit Activated(filter_->mapToSource(index));
    });

    countTimer_ = new QTimer(this);
    countTimer_->setSingleShot(true);
    connect(countTimer_, &QTimer::timeout, this, &ProblemList::RefreshCounts);
    RefreshCounts();
}

void ProblemList::SetSourceModel(QAbstractItemModel* model, SeverityOf severityOf) {
    if (auto* previous = filter_->sourceModel()) disconnect(previous, nullptr, this, nullptr);
    filter_->severityOf = std::move(severityOf);
    filter_->setSourceModel(model);
    if (model) {
        // 逐行追加时合并成一次计数，避免大批诊断写入时反复全表扫描。
        connect(model, &QAbstractItemModel::rowsInserted, this, &ProblemList::ScheduleCounts);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &ProblemList::ScheduleCounts);
        connect(model, &QAbstractItemModel::modelReset, this, &ProblemList::ScheduleCounts);
        connect(model, &QAbstractItemModel::dataChanged, this, &ProblemList::ScheduleCounts);
        connect(model, &QAbstractItemModel::layoutChanged, this, &ProblemList::ScheduleCounts);
    }
    RefreshCounts();
}

void ProblemList::SetState(const QString& text) {
    state_->setText(text);
    state_->setVisible(!text.isEmpty());
}

int ProblemList::Count(ProblemSeverity severity) const {
    if (countTimer_->isActive()) const_cast<ProblemList*>(this)->RefreshCounts();
    return counts_[static_cast<std::size_t>(severity)];
}

bool ProblemList::IsShown(ProblemSeverity severity) const {
    return toggles_[static_cast<std::size_t>(severity)]->isChecked();
}

void ProblemList::SetShown(ProblemSeverity severity, bool shown) {
    toggles_[static_cast<std::size_t>(severity)]->setChecked(shown);
}

ProblemSeverity ProblemList::SeverityFromText(const QString& text) {
    if (text == tr("错误")) return ProblemSeverity::Error;
    if (text == tr("警告")) return ProblemSeverity::Warning;
    return ProblemSeverity::Info;
}

void ProblemList::ScheduleCounts() {
    if (!countTimer_->isActive()) countTimer_->start(0);
}

void ProblemList::RefreshCounts() {
    countTimer_->stop();
    counts_ = {};
    if (const auto* model = filter_->sourceModel(); model && filter_->severityOf) {
        for (int row = 0; row < model->rowCount(); ++row)
            ++counts_[static_cast<std::size_t>(filter_->severityOf(*model, row))];
    }
    for (std::size_t index = 0; index < toggles_.size(); ++index)
        toggles_[index]->setText(tr("%1 %2").arg(counts_[index]).arg(tr(kToggles[index].label)));
    emit CountsChanged();
}

} // namespace War3Map::UiKit
