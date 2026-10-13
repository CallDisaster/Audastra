#include "ChangesPanel.h"
#include "Widgets/ElidedLabel.h"

#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr std::size_t kEntryLimit = 256;
constexpr int EntryRole = Qt::UserRole + 1;

QLabel* PlainLabel(QWidget* parent, const QString& text, const QString& name) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QString KindLabel(ChangesPanel::Kind kind) {
    switch (kind) {
    case ChangesPanel::Kind::Added: return QObject::tr("新增");
    case ChangesPanel::Kind::Removed: return QObject::tr("删除");
    case ChangesPanel::Kind::Member: return QObject::tr("成员");
    default: return QObject::tr("修改");
    }
}
} // namespace

ChangesPanel::ChangesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("changesPanel"));
    setMinimumWidth(320);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    summary_ = PlainLabel(this, tr("尚未读取修改。"), QStringLiteral("changesSummary"));
    QFont heading = summary_->font(); heading.setWeight(QFont::Medium); summary_->setFont(heading);
    layout->addWidget(summary_);
    unsupported_ = PlainLabel(this, {}, QStringLiteral("changesUnsupported"));
    unsupported_->setForegroundRole(QPalette::PlaceholderText);
    unsupported_->hide(); layout->addWidget(unsupported_);
    truncated_ = PlainLabel(this, {}, QStringLiteral("changesTruncated"));
    truncated_->setForegroundRole(QPalette::PlaceholderText);
    truncated_->hide(); layout->addWidget(truncated_);
    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("changesTree"));
    tree_->setAccessibleName(tr("本次修改"));
    tree_->setHeaderHidden(true);
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree_->setTextElideMode(Qt::ElideRight);
    model_ = new QStandardItemModel(tree_); tree_->setModel(model_);
    layout->addWidget(tree_, 1);
    auto* fields = new QFormLayout;
    before_ = new War3Map::UiKit::ElidedLabel(this); before_->setObjectName(QStringLiteral("changesBefore"));
    after_ = new War3Map::UiKit::ElidedLabel(this); after_->setObjectName(QStringLiteral("changesAfter"));
    fields->addRow(tr("原值"), before_); fields->addRow(tr("当前值"), after_);
    layout->addLayout(fields);
    reason_ = PlainLabel(this, QString(), QStringLiteral("changesReason"));
    reason_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(reason_);
    auto* rowActions = new QHBoxLayout;
    reveal_ = new QPushButton(tr("转到"), this); reveal_->setObjectName(QStringLiteral("changesReveal"));
    revert_ = new QPushButton(tr("撤销这项"), this); revert_->setObjectName(QStringLiteral("changesRevert"));
    reveal_->setEnabled(false); revert_->setEnabled(false);
    rowActions->addWidget(reveal_); rowActions->addWidget(revert_); rowActions->addStretch();
    layout->addLayout(rowActions);
    auto* separator = new QFrame(this); separator->setFrameShape(QFrame::HLine); layout->addWidget(separator);
    preflight_ = PlainLabel(this, tr("尚未检查保存。"), QStringLiteral("changesPreflight"));
    layout->addWidget(preflight_);
    preflightDetails_ = new QTextEdit(this);
    preflightDetails_->setObjectName(QStringLiteral("changesPreflightDetails"));
    preflightDetails_->setReadOnly(true); preflightDetails_->setMaximumHeight(110);
    preflightDetails_->hide(); layout->addWidget(preflightDetails_);
    auto* saveActions = new QHBoxLayout;
    check_ = new QPushButton(tr("检查保存"), this); check_->setObjectName(QStringLiteral("changesCheckSave"));
    save_ = new QPushButton(tr("保存"), this); save_->setObjectName(QStringLiteral("changesSave"));
    saveActions->addWidget(check_); saveActions->addStretch(); saveActions->addWidget(save_);
    layout->addLayout(saveActions);
    saveReason_ = PlainLabel(this, {}, QStringLiteral("changesSaveReason"));
    saveReason_->setForegroundRole(QPalette::PlaceholderText); saveReason_->hide(); layout->addWidget(saveReason_);
    SetActionsAvailable(false, false);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) { SelectEntry(current); });
    connect(tree_, &QTreeView::clicked, this, &ChangesPanel::SelectEntry);
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& current) { SelectEntry(current); InvokeSelected(false); });
    connect(reveal_, &QPushButton::clicked, this, [this] { InvokeSelected(false); });
    connect(revert_, &QPushButton::clicked, this, [this] { InvokeSelected(true); });
    connect(save_, &QPushButton::clicked, this, &ChangesPanel::SaveRequested);
    connect(check_, &QPushButton::clicked, this, &ChangesPanel::CheckSaveRequested);
}

void ChangesPanel::SetEntries(std::vector<Entry> entries, const QStringList& unsupported, bool hasMore) {
    selected_ = -1; entries_.clear(); model_->clear(); SelectEntry({});
    SetPreflightResult(tr("尚未检查保存。"));
    SetActionsAvailable(false, false);
    const bool limited = hasMore || entries.size() > kEntryLimit;
    if (entries.size() > kEntryLimit) entries.resize(kEntryLimit);
    entries_ = std::move(entries);
    summary_->setText(entries_.empty() ? limited ? tr("有未展开的修改，详见范围说明。")
        : tr("当前列出的范围暂无修改。") : tr("已列出 %1 项修改").arg(entries_.size()));
    unsupported_->setText(tr("未展开的范围：%1").arg(unsupported.mid(0, 12).join(tr("、")))
        + (unsupported.size() > 12 ? tr("（另 %1 项）").arg(unsupported.size() - 12) : QString()));
    unsupported_->setToolTip(unsupported.join(QLatin1Char('\n')));
    unsupported_->setVisible(!unsupported.isEmpty());
    truncated_->setText(tr("只列出前 256 项"));
    truncated_->setVisible(limited);
    QHash<QString, QStandardItem*> members;
    QHash<QString, QHash<QString, QStandardItem*>> records;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        auto* member = members.value(entry.member);
        if (!member) {
            member = new QStandardItem(entry.member.isEmpty() ? tr("未标识成员") : entry.member);
            member->setFlags(Qt::ItemIsEnabled); member->setToolTip(member->text());
            model_->appendRow(member); members.insert(entry.member, member);
        }
        auto& memberRecords = records[entry.member];
        auto* record = memberRecords.value(entry.record);
        if (!record) {
            record = new QStandardItem(entry.record.isEmpty() ? tr("成员级修改") : entry.record);
            record->setFlags(Qt::ItemIsEnabled); record->setToolTip(record->text());
            member->appendRow(record); memberRecords.insert(entry.record, record);
        }
        auto* field = new QStandardItem(KindLabel(entry.kind) + tr(" · ") + entry.title);
        field->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        field->setData(entry.id, Qt::UserRole); field->setData(static_cast<int>(i), EntryRole);
        field->setToolTip(entry.title + tr("\n原值：") + entry.before + tr("\n当前值：") + entry.after
            + (entry.reason.isEmpty() ? QString() : QLatin1Char('\n') + entry.reason));
        record->appendRow(field);
    }
    tree_->expandAll(); tree_->clearSelection(); tree_->setCurrentIndex({});
}

QString ChangesPanel::SelectedId() const {
    return selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) ? entries_[selected_].id : QString();
}

void ChangesPanel::SelectEntry(const QModelIndex& index) {
    selected_ = -1; reveal_->setEnabled(false); revert_->setEnabled(false);
    before_->clear(); after_->clear(); before_->setToolTip({}); after_->setToolTip({});
    reason_->clear();
    if (!index.isValid() || index.model() != model_ || !index.data(EntryRole).isValid()) return;
    const int row = index.data(EntryRole).toInt();
    if (row < 0 || row >= static_cast<int>(entries_.size())) return;
    selected_ = row;
    const auto& entry = entries_[row];
    before_->setText(entry.before); after_->setText(entry.after);
    before_->setToolTip(entry.before); after_->setToolTip(entry.after);
    reveal_->setEnabled(!entry.id.isEmpty() && static_cast<bool>(entry.reveal));
    revert_->setEnabled(!entry.id.isEmpty() && entry.canRevert && static_cast<bool>(entry.revert));
    reason_->setText(!entry.reason.isEmpty() ? entry.reason : entry.id.isEmpty() ? tr("缺少条目标识，不能执行操作。")
        : revert_->isEnabled() ? tr("可撤销这项。") : tr("本项仅供查看，不能单独撤销。"));
}

void ChangesPanel::InvokeSelected(bool reverse) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
    const auto& entry = entries_[selected_];
    if (entry.id.isEmpty() || (reverse && !entry.canRevert)) return;
    const auto action = reverse ? entry.revert : entry.reveal;
    if (action) action(); // 复制后调用；宿主可以刷新面板或删除它，之后不再访问缓存。
}

void ChangesPanel::SetPreflightResult(const QString& summary, const QStringList& details) {
    preflight_->setText(summary);
    preflightDetails_->setPlainText(details.join(QLatin1Char('\n')));
    preflightDetails_->setVisible(!details.isEmpty());
}

void ChangesPanel::SetActionsAvailable(bool canSave, bool canCheck, const QString& reason) {
    save_->setEnabled(canSave); check_->setEnabled(canCheck);
    saveReason_->setText(reason); saveReason_->setVisible(!reason.isEmpty());
    save_->setToolTip(reason); check_->setToolTip(reason);
}
