#include "TriggerReplaceDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>
#include <chrono>

namespace War3Map::Modules {
namespace {
using namespace Services;
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Number(std::size_t value) { return QString::number(static_cast<qulonglong>(value)); }
QString Tooltip(const QString& text) { return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()); }
QString Failure(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return QObject::tr("共享服务未返回可用结果。");
    QStringList lines;
    for (std::size_t i = 0; i < std::min<std::size_t>(4, diagnostics.size()); ++i)
        lines.push_back(Text(diagnostics[i].code) + QStringLiteral(" · ") + Text(diagnostics[i].message));
    return lines.join(QLatin1Char('\n'));
}
QString Coverage(const TriggerReplaceCoverage& coverage, std::size_t offset, std::size_t rows, bool targets = false) {
    const bool complete = coverage.state == TriggerReplaceState::Ready && !coverage.scanTruncated && coverage.totalMatches.has_value();
    QString text;
    if (targets) {
        text = complete ? QObject::tr("可选目标 %1 项；本页 %2–%3。")
            .arg(Number(*coverage.totalMatches), Number(rows ? offset + 1 : 0), Number(offset + rows))
            : QObject::tr("已确认 %1 项；目录尚未完整。").arg(Number(coverage.matchedInScannedScope));
    } else {
        text = complete ? QObject::tr("找到 %1 项；本页 %2–%3。")
            .arg(Number(*coverage.totalMatches), Number(rows ? offset + 1 : 0), Number(offset + rows))
            : QObject::tr("已扫描范围找到 %1 项；总数尚未确认；本页 %2–%3。")
                .arg(Number(coverage.matchedInScannedScope), Number(rows ? offset + 1 : 0), Number(offset + rows));
    }
    if (coverage.customCodeSlotsExcluded)
        text += QObject::tr(" %1 个代码块未检查正文。函数、代码和数组索引仅可查找定位。").arg(Number(coverage.customCodeSlotsExcluded));
    if (coverage.unresolvedReferenceRows || coverage.displayResolutionPartial)
        text += QObject::tr(" 部分类型或显示名称尚未确认；原值仍保留。");
    return text;
}
QPushButton* Button(const QString& text, const char* name, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setAutoDefault(false);
    button->setDefault(false);
    return button;
}
void ConfigureTable(QTreeWidget& tree, const QStringList& columns) {
    tree.setColumnCount(static_cast<int>(columns.size())); tree.setHeaderLabels(columns);
    tree.setRootIsDecorated(false); tree.setUniformRowHeights(true);
    tree.setSelectionMode(QAbstractItemView::SingleSelection);
    tree.header()->setSectionResizeMode(QHeaderView::Interactive);
    tree.header()->setStretchLastSection(true);
}
QString Location(const TriggerReplaceHit& hit) {
    return Text(hit.triggerName) + QStringLiteral(" / ") + Text(hit.functionName)
        + (hit.argumentId ? QObject::tr(" · 参数 %1").arg(QString::number(*hit.argumentId)) : QString{});
}
} // namespace

TriggerReplaceDialog::TriggerReplaceDialog(std::function<std::string()> revision, std::function<Status()> prepareContext, Query query, Options options,
    Preview preview, Apply apply, std::function<void(const TriggerReplaceHit&)> activate, QWidget* parent)
    : QDialog(parent), revision_(std::move(revision)), prepareContext_(std::move(prepareContext)), query_(std::move(query)), options_(std::move(options)),
      previewCallback_(std::move(preview)), apply_(std::move(apply)), activate_(std::move(activate)) {
    setObjectName(QStringLiteral("triggerReplaceDialog"));
    setWindowTitle(tr("触发器查找与替换 · Audastra")); setModal(false); resize(1000, 650); setMinimumSize(760, 540);
    auto* layout = new QVBoxLayout(this);
    auto* queryBar = new QHBoxLayout;
    scope_ = new QComboBox(this); scope_->setObjectName(QStringLiteral("triggerReplaceScope"));
    scope_->addItem(tr("全部传统触发器"), QVariant::fromValue<qulonglong>(0));
    mode_ = new QComboBox(this); mode_->setObjectName(QStringLiteral("triggerReplaceMode"));
    mode_->addItem(tr("参数值"), static_cast<int>(TriggerReplaceMode::ParameterValue));
    mode_->addItem(tr("函数"), static_cast<int>(TriggerReplaceMode::Function));
    mode_->addItem(tr("变量引用"), static_cast<int>(TriggerReplaceMode::Variable));
    mode_->addItem(tr("物体引用"), static_cast<int>(TriggerReplaceMode::ObjectReference));
    queryEdit_ = new QLineEdit(this); queryEdit_->setObjectName(QStringLiteral("triggerReplaceQuery"));
    queryEdit_->setMaxLength(2048); queryEdit_->setPlaceholderText(tr("查找传统 T 的原值或引用；不替换 JASS / Lua 正文"));
    match_ = new QComboBox(this); match_->setObjectName(QStringLiteral("triggerReplaceMatch"));
    match_->addItem(tr("包含"), static_cast<int>(TriggerReplaceMatch::Substring));
    match_->addItem(tr("精确"), static_cast<int>(TriggerReplaceMatch::Exact));
    caseSensitive_ = new QCheckBox(tr("区分大小写"), this); caseSensitive_->setChecked(true);
    auto* search = Button(tr("查找"), "triggerReplaceSearch", this);
    for (auto* widget : std::initializer_list<QWidget*>{scope_, mode_, queryEdit_, match_, caseSensitive_, search})
        queryBar->addWidget(widget, widget == queryEdit_ ? 1 : 0);
    layout->addLayout(queryBar);
    auto* targetBar = new QHBoxLayout;
    targetBar->addWidget(new QLabel(tr("替换目标"), this));
    replacementKind_ = new QComboBox(this); replacementKind_->setObjectName(QStringLiteral("triggerReplaceTargetKind"));
    replacementKind_->addItem(tr("声明变量"), static_cast<int>(TriggerReplacementKind::Variable));
    replacementKind_->addItem(tr("物体编号"), static_cast<int>(TriggerReplacementKind::ObjectLiteral));
    replacementKind_->addItem(tr("预放物体"), static_cast<int>(TriggerReplacementKind::PreplacedObject));
    targetFilter_ = new QLineEdit(this); targetFilter_->setObjectName(QStringLiteral("triggerReplaceTargetFilter"));
    targetFilter_->setMaxLength(2048); targetFilter_->setPlaceholderText(tr("按名称、类型或四字码查已有目录"));
    auto* targetSearch = Button(tr("查目录"), "triggerReplaceTargetSearch", this);
    targets_ = new QComboBox(this); targets_->setObjectName(QStringLiteral("triggerReplaceTarget"));
    targets_->setMinimumContentsLength(20); targets_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    targetPrevious_ = Button(tr("上一页"), "triggerReplaceTargetPrevious", this);
    targetNext_ = Button(tr("下一页"), "triggerReplaceTargetNext", this);
    for (auto* widget : std::initializer_list<QWidget*>{replacementKind_, targetFilter_, targetSearch, targets_, targetPrevious_, targetNext_})
        targetBar->addWidget(widget, widget == targets_ ? 2 : widget == targetFilter_ ? 1 : 0);
    layout->addLayout(targetBar);
    targetStatus_ = new QLabel(tr("从当前已接受的真实声明或物体目录选择目标；不会按四字码前缀猜类型。"), this);
    targetStatus_->setObjectName(QStringLiteral("triggerReplaceTargetStatus"));
    targetStatus_->setTextFormat(Qt::PlainText); targetStatus_->setWordWrap(true); layout->addWidget(targetStatus_);
    tabs_ = new QTabWidget(this);
    results_ = new QTreeWidget(this); results_->setObjectName(QStringLiteral("triggerReplaceResults"));
    ConfigureTable(*results_, {tr("选择"), tr("位置"), tr("原值 / 显示名"), tr("声明类型"), tr("状态")});
    results_->setColumnWidth(0, 48); results_->setColumnWidth(1, 240); results_->setColumnWidth(2, 200); results_->setColumnWidth(3, 100);
    previewRows_ = new QTreeWidget(this); previewRows_->setObjectName(QStringLiteral("triggerReplacePreviewRows"));
    ConfigureTable(*previewRows_, {tr("位置"), tr("原值"), tr("替换为"), tr("期望类型"), tr("校验")});
    previewRows_->setColumnWidth(0, 240); previewRows_->setColumnWidth(1, 170); previewRows_->setColumnWidth(2, 170);
    tabs_->addTab(results_, tr("查找结果")); tabs_->addTab(previewRows_, tr("替换预览")); layout->addWidget(tabs_, 1);
    auto* selectionBar = new QHBoxLayout;
    auto* checkPage = Button(tr("勾选本页可预览项"), "triggerReplaceCheckPage", this);
    auto* clear = Button(tr("清除全部勾选"), "triggerReplaceClearSelection", this);
    selectionStatus_ = new QLabel(this); selectionStatus_->setObjectName(QStringLiteral("triggerReplaceSelectionStatus"));
    selectionStatus_->setTextFormat(Qt::PlainText);
    selectionBar->addWidget(checkPage); selectionBar->addWidget(clear); selectionBar->addWidget(selectionStatus_, 1); layout->addLayout(selectionBar);
    status_ = new QLabel(tr("查找和预览只读；勾选真实参数并完成类型校验后才可应用，一次撤销还原整批。"), this);
    status_->setObjectName(QStringLiteral("triggerReplaceStatus")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (auto* close = buttons->button(QDialogButtonBox::Close)) close->setText(tr("关闭"));
    previous_ = buttons->addButton(tr("上一页"), QDialogButtonBox::ActionRole); previous_->setObjectName(QStringLiteral("triggerReplacePrevious"));
    next_ = buttons->addButton(tr("下一页"), QDialogButtonBox::ActionRole); next_->setObjectName(QStringLiteral("triggerReplaceNext"));
    auto* locate = buttons->addButton(tr("定位"), QDialogButtonBox::ActionRole); locate->setObjectName(QStringLiteral("triggerReplaceLocate"));
    previewButton_ = buttons->addButton(tr("预览勾选项"), QDialogButtonBox::ActionRole); previewButton_->setObjectName(QStringLiteral("triggerReplacePreview"));
    cancelPreviewButton_ = buttons->addButton(tr("取消准备"), QDialogButtonBox::ActionRole);
    cancelPreviewButton_->setObjectName(QStringLiteral("triggerReplaceCancelPreview")); cancelPreviewButton_->setEnabled(false);
    applyButton_ = buttons->addButton(tr("应用已预览替换"), QDialogButtonBox::ActionRole); applyButton_->setObjectName(QStringLiteral("triggerReplaceApply"));
    for (auto* button : buttons->findChildren<QPushButton*>()) { button->setAutoDefault(false); button->setDefault(false); }
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
    connect(search, &QPushButton::clicked, this, [this] { Search(true); });
    connect(queryEdit_, &QLineEdit::returnPressed, this, [this] { Search(true); });
    connect(targetSearch, &QPushButton::clicked, this, [this] { SearchTargets(true); });
    connect(targetFilter_, &QLineEdit::returnPressed, this, [this] { SearchTargets(true); });
    connect(previous_, &QPushButton::clicked, this, [this] { if (pageIndex_) { --pageIndex_; Search(false); } });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (!nextCursor_) return; starts_.resize(pageIndex_ + 1); starts_.push_back(nextCursor_); ++pageIndex_; Search(false);
    });
    connect(targetPrevious_, &QPushButton::clicked, this, [this] { if (targetPageIndex_) { --targetPageIndex_; SearchTargets(false); } });
    connect(targetNext_, &QPushButton::clicked, this, [this] {
        if (!targetNextCursor_) return;
        targetStarts_.resize(targetPageIndex_ + 1); targetStarts_.push_back(targetNextCursor_); ++targetPageIndex_; SearchTargets(false);
    });
    connect(checkPage, &QPushButton::clicked, this, [this] { CheckCurrentPage(true); });
    connect(clear, &QPushButton::clicked, this, [this] { selected_.clear(); InvalidatePreview(); CheckCurrentPage(false); });
    connect(locate, &QPushButton::clicked, this, [this] { ActivateCurrent(); });
    connect(results_, &QTreeWidget::itemActivated, this, [this] { ActivateCurrent(); });
    connect(previewButton_, &QPushButton::clicked, this, [this] { PreparePreview(); });
    connect(cancelPreviewButton_, &QPushButton::clicked, this, [this] { CancelPreview(); });
    connect(applyButton_, &QPushButton::clicked, this, [this] { ApplyPreview(); });
    connect(queryEdit_, &QLineEdit::textChanged, this, [this] { ResetQuery(); });
    for (auto* combo : {scope_, mode_, match_}) connect(combo, &QComboBox::currentIndexChanged, this, [this] { ResetQuery(); });
    connect(caseSensitive_, &QCheckBox::toggled, this, [this] { ResetQuery(); });
    connect(targetFilter_, &QLineEdit::textChanged, this, [this] { ResetTargets(); });
    connect(replacementKind_, &QComboBox::currentIndexChanged, this, [this] { ResetTargets(); });
    connect(targets_, &QComboBox::currentIndexChanged, this, [this] { InvalidatePreview(); UpdateSelectionText(); });
    connect(results_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updatingRows_ || column != 0) return;
        const auto index = results_->indexOfTopLevelItem(item);
        if (index < 0 || static_cast<std::size_t>(index) >= rows_.size()) return;
        const auto& row = rows_[static_cast<std::size_t>(index)];
        if (item->checkState(0) == Qt::Checked) {
            if (row.eligibility != TriggerReplaceEligibility::PreviewRequired ||
                (!selected_.contains(row.rowKey) && selected_.size() >= limits_.maxSelection)) {
                const QScopedValueRollback guard(updatingRows_, true); item->setCheckState(0, Qt::Unchecked);
                status_->setText(tr("最多勾选 %1 项；不可替换项只可定位。").arg(Number(limits_.maxSelection))); return;
            }
            selected_.insert(row.rowKey);
        } else selected_.erase(row.rowKey);
        InvalidatePreview(); UpdateSelectionText();
    });
    ResetTargets(); ResetQuery();
}

void TriggerReplaceDialog::SetCurrentScope(std::optional<StableId> id, const QString& name) {
    const QSignalBlocker blocked(scope_);
    scope_->clear(); scope_->addItem(tr("全部传统触发器"), QVariant::fromValue<qulonglong>(0));
    if (id && *id) { scope_->addItem(tr("此触发器：%1").arg(name), QVariant::fromValue<qulonglong>(*id)); scope_->setCurrentIndex(1); }
    ResetQuery();
}
void TriggerReplaceDialog::FocusQuery() {
    const QPointer<TriggerReplaceDialog> self(this);
    const QPointer<QLineEdit> edit(queryEdit_);
    show();
    if (!self || !edit) return;
    self->raise();
    if (!self || !edit) return;
    self->activateWindow();
    if (!self || !edit) return;
    edit->setFocus();
    if (!self || !edit) return;
    edit->selectAll();
}
TriggerReplaceDialog::~TriggerReplaceDialog() {
    if (previewStop_) previewStop_->request_stop();
}
bool TriggerReplaceDialog::PreviewPending() const noexcept { return previewStarting_ || previewWatcher_; }
std::shared_ptr<const TriggerReplaceDialog::PreviewTiming> TriggerReplaceDialog::LastPreviewTiming() const noexcept {
    return PreviewPending() ? nullptr : previewTiming_;
}
void TriggerReplaceDialog::SetAsyncPreview(AsyncPreview preview) {
    InvalidatePreview(); asyncPreview_ = std::move(preview); UpdateSelectionText();
}
void TriggerReplaceDialog::CancelPreview() {
    if (!PreviewPending()) return;
    InvalidatePreview(); UpdateSelectionText();
    status_->setText(tr("已请求取消准备；后台将在安全阶段停止，没有修改地图。"));
}
void TriggerReplaceDialog::hideEvent(QHideEvent* event) {
    InvalidatePreview(); UpdateSelectionText(); QDialog::hideEvent(event);
}
void TriggerReplaceDialog::InvalidatePreview() {
    ++requestGeneration_;
    if (previewStop_) previewStop_->request_stop();
    preview_.reset(); previewTiming_.reset(); previewRows_->clear(); applyButton_->setEnabled(false);
}
void TriggerReplaceDialog::ResetQuery() {
    InvalidatePreview(); proof_.reset(); rows_.clear(); results_->clear(); selected_.clear(); starts_.clear(); nextCursor_.reset();
    previous_->setEnabled(false); next_->setEnabled(false); UpdateSelectionText();
    status_->setText(tr("请查找当前范围；旧结果和预览已清除。代码正文不作替换。"));
}
void TriggerReplaceDialog::ResetTargets() {
    InvalidatePreview(); targetRows_.clear(); targetStarts_.clear(); targetNextCursor_.reset();
    const QSignalBlocker blocked(targets_); targets_->clear(); targets_->addItem(tr("先查目录，再明确选择目标"));
    targetPrevious_->setEnabled(false); targetNext_->setEnabled(false); UpdateSelectionText();
}
void TriggerReplaceDialog::ContextChanged() {
    ResetQuery(); ResetTargets();
    status_->setText(tr("工程修订、目录或来源已变化；旧命中和预览失效，请重新查找。"));
}
TriggerReplaceQuery TriggerReplaceDialog::ReadQuery() const {
    TriggerReplaceQuery query;
    query.mode = static_cast<TriggerReplaceMode>(mode_->currentData().toInt());
    query.match = static_cast<TriggerReplaceMatch>(match_->currentData().toInt());
    query.needle = queryEdit_->text().toUtf8().toStdString(); query.caseSensitive = caseSensitive_->isChecked();
    const auto id = scope_->currentData().toULongLong(); if (id) query.scopeTriggerId = id;
    return query;
}
std::optional<TriggerReplacement> TriggerReplaceDialog::Replacement() const {
    const auto index = targets_->currentIndex() - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= targetRows_.size()) return std::nullopt;
    return targetRows_[static_cast<std::size_t>(index)].replacement;
}
void TriggerReplaceDialog::UpdateSelectionText() {
    selectionStatus_->setText(tr("跨页已勾选 %1 / %2 项").arg(Number(selected_.size()), Number(limits_.maxSelection)));
    previewButton_->setEnabled(!PreviewPending() && proof_.has_value() && !selected_.empty() && Replacement().has_value());
    cancelPreviewButton_->setEnabled(PreviewPending() && (!previewStop_ || !previewStop_->stop_requested()));
}

void TriggerReplaceDialog::Search(bool reset) {
    if (!revision_ || !query_) return;
    if (reset && prepareContext_) {
        const auto prepare = prepareContext_; const QPointer<TriggerReplaceDialog> guard(this);
        const auto generation = ++requestGeneration_; const auto status = prepare();
        if (!guard || requestGeneration_ != generation) return;
        if (!status.Succeeded()) { ResetQuery(); status_->setText(Failure(status.diagnostics)); return; }
    }
    if (reset) { ResetQuery(); boundQuery_ = ReadQuery(); starts_ = {std::nullopt}; pageIndex_ = 0; }
    if (pageIndex_ >= starts_.size()) return;
    const auto generation = ++requestGeneration_;
    const QPointer<TriggerReplaceDialog> guard(this); const auto getRevision = revision_; const auto query = query_;
    const auto revision = getRevision(); if (!guard || requestGeneration_ != generation) return;
    if (reset) boundRevision_ = revision;
    if (revision != boundRevision_) { ContextChanged(); return; }
    const auto request = boundQuery_; const auto cursor = starts_[pageIndex_]; const auto limits = limits_;
    const auto result = query(revision, request, limits, cursor ? &*cursor : nullptr);
    if (!guard || requestGeneration_ != generation) return;
    const auto after = getRevision(); if (!guard || requestGeneration_ != generation) return;
    if (after != revision) { ContextChanged(); return; }
    if (!result.Succeeded() || !result.value) { ResetQuery(); status_->setText(Failure(result.diagnostics)); return; }
    if (result.value->proof.contextRevision != revision || (proof_ && *proof_ != result.value->proof)) { ContextChanged(); return; }
    proof_ = result.value->proof; Populate(*result.value);
}
void TriggerReplaceDialog::Populate(const TriggerReplacePage& page) {
    const QScopedValueRollback updating(updatingRows_, true);
    rows_ = page.rows; results_->clear(); nextCursor_ = page.next;
    for (const auto& row : rows_) {
        auto* item = new QTreeWidgetItem(results_);
        item->setText(1, Location(row));
        item->setText(2, Text(row.oldValue) + (row.displayValue.empty() || row.displayValue == row.oldValue ? QString{} : QStringLiteral(" · ") + Text(row.displayValue)));
        item->setToolTip(1, Tooltip(item->text(1))); item->setToolTip(2, Tooltip(item->text(2)));
        item->setText(3, row.declaredType.empty() ? tr("待预览确认") : Text(row.declaredType));
        item->setText(4, row.eligibility == TriggerReplaceEligibility::ReadOnly ? Text(row.reasonCode) + QStringLiteral(" · ") + Text(row.reason) : tr("预览时校验真实类型与来源"));
        if (row.eligibility == TriggerReplaceEligibility::PreviewRequired) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(0, selected_.contains(row.rowKey) ? Qt::Checked : Qt::Unchecked);
        } else item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
    if (results_->topLevelItemCount()) results_->setCurrentItem(results_->topLevelItem(0));
    previous_->setEnabled(pageIndex_ != 0); next_->setEnabled(nextCursor_.has_value());
    status_->setText(Coverage(page.coverage, page.matchOffset, page.rows.size())); tabs_->setCurrentIndex(0); UpdateSelectionText();
}
void TriggerReplaceDialog::SearchTargets(bool reset) {
    if (!revision_ || !options_) return;
    if (reset && prepareContext_) {
        const auto prepare = prepareContext_; const QPointer<TriggerReplaceDialog> guard(this);
        const auto generation = ++requestGeneration_; const auto status = prepare();
        if (!guard || requestGeneration_ != generation) return;
        if (!status.Succeeded()) { ResetTargets(); targetStatus_->setText(Failure(status.diagnostics)); return; }
    }
    if (reset) {
        ResetTargets(); targetStarts_ = {std::nullopt}; targetPageIndex_ = 0;
        boundTargetFilter_ = targetFilter_->text().toUtf8().toStdString();
        boundTargetKind_ = static_cast<TriggerReplacementKind>(replacementKind_->currentData().toInt());
    }
    if (targetPageIndex_ >= targetStarts_.size()) return;
    const auto generation = ++requestGeneration_; const QPointer<TriggerReplaceDialog> guard(this);
    const auto getRevision = revision_; const auto options = options_; const auto revision = getRevision();
    if (!guard || requestGeneration_ != generation) return;
    if (reset) targetRevision_ = revision;
    if (revision != targetRevision_) { ContextChanged(); return; }
    const auto cursor = targetStarts_[targetPageIndex_]; const auto filter = boundTargetFilter_; const auto kind = boundTargetKind_; const auto limits = limits_;
    const auto result = options(revision, kind, filter, limits, cursor ? &*cursor : nullptr);
    if (!guard || requestGeneration_ != generation) return;
    const auto after = getRevision(); if (!guard || requestGeneration_ != generation) return;
    if (after != revision) { ContextChanged(); return; }
    if (!result.Succeeded() || !result.value) { ResetTargets(); targetStatus_->setText(Failure(result.diagnostics)); return; }
    if (result.value->proof.contextRevision != revision) { ContextChanged(); return; }
    PopulateTargets(*result.value);
}
void TriggerReplaceDialog::PopulateTargets(const TriggerReplacementOptionsPage& page) {
    InvalidatePreview(); targetRows_ = page.rows; targetNextCursor_ = page.next;
    const QSignalBlocker blocked(targets_); targets_->clear(); targets_->addItem(tr("请选择已确认的目标"));
    for (const auto& row : targetRows_) targets_->addItem(Text(row.display) + QStringLiteral(" · ") + Text(row.typeName) + QStringLiteral(" · ") + Text(row.source));
    targetPrevious_->setEnabled(targetPageIndex_ != 0); targetNext_->setEnabled(targetNextCursor_.has_value());
    targetStatus_->setText(Coverage(page.coverage, page.matchOffset, page.rows.size(), true)); UpdateSelectionText();
}
void TriggerReplaceDialog::CheckCurrentPage(bool checked) {
    for (int index = 0; index < results_->topLevelItemCount(); ++index) {
        if (rows_[static_cast<std::size_t>(index)].eligibility != TriggerReplaceEligibility::PreviewRequired) continue;
        results_->topLevelItem(index)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
    }
    UpdateSelectionText();
}
void TriggerReplaceDialog::ActivateCurrent() {
    if (!proof_ || !activate_ || !revision_) return;
    const auto index = results_->indexOfTopLevelItem(results_->currentItem());
    if (index < 0 || static_cast<std::size_t>(index) >= rows_.size()) return;
    const auto row = rows_[static_cast<std::size_t>(index)]; const auto expected = proof_->contextRevision;
    const auto generation = requestGeneration_; const QPointer<TriggerReplaceDialog> guard(this);
    const auto getRevision = revision_; const auto activate = activate_; const auto revision = getRevision();
    if (!guard || requestGeneration_ != generation) return;
    if (revision != expected) { ContextChanged(); return; }
    activate(row);
}
void TriggerReplaceDialog::PreparePreview() {
    const auto replacement = Replacement();
    if (PreviewPending() || !proof_ || selected_.empty() || !replacement || !revision_ || (!previewCallback_ && !asyncPreview_)) return;
    InvalidatePreview();
    const auto generation = ++requestGeneration_; const QPointer<TriggerReplaceDialog> guard(this);
    const auto getRevision = revision_; const auto prepare = previewCallback_; const auto async = asyncPreview_;
    const auto proof = *proof_; const auto request = boundQuery_; const auto limits = limits_;
    const std::vector<std::string> keys(selected_.begin(), selected_.end());
    const auto revision = getRevision(); if (!guard || requestGeneration_ != generation) return;
    if (revision != proof.contextRevision || revision != targetRevision_) { ContextChanged(); return; }
    if (async) {
        previewStarting_ = true; UpdateSelectionText();
        Result<PreviewTask> task;
        try { task = async(revision, request, proof, keys, *replacement, limits); }
        catch (...) { task = {std::nullopt, Status::Failure("Trigger.Replace.Qt", "TRIGGER_REPLACE_ASYNC_START", "预览准备未能启动；当前地图保持。").diagnostics}; }
        if (!guard) { if (task.value && task.value->stop) task.value->stop->request_stop(); return; }
        previewStarting_ = false;
        if (requestGeneration_ != generation) {
            if (task.value && task.value->stop) task.value->stop->request_stop();
            UpdateSelectionText(); return;
        }
        if (!task.Succeeded() || !task.value || !task.value->stop || !task.value->future.isStarted()) {
            if (task.value && task.value->stop) task.value->stop->request_stop();
            UpdateSelectionText(); status_->setText(Failure(task.diagnostics)); return;
        }
        previewStop_ = task.value->stop; previewTiming_ = task.value->timing;
        auto* watcher = new QFutureWatcher<Result<TriggerReplacePreviewResult>>(this); previewWatcher_ = watcher;
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, proof, revision, getRevision] {
            const QPointer<TriggerReplaceDialog> owner(this);
            const auto future = watcher->future();
            const auto stopped = previewStop_ && previewStop_->stop_requested();
            previewWatcher_ = nullptr; previewStop_.reset(); watcher->deleteLater(); UpdateSelectionText();
            if (!owner || generation != requestGeneration_) return;
            if (stopped) { previewTiming_.reset(); return; }
            // revision 回调可以重绑或销毁窗口；不在它返回后借用旧成员/来源。
            try {
                const auto current = getRevision(); if (!owner || generation != requestGeneration_) return;
                if (current != revision) { ContextChanged(); return; }
                if (future.resultCount() != 1) {
                    previewTiming_.reset(); status_->setText(tr("后台预览未返回完整结果；没有修改地图。")); return;
                }
                const auto result = future.result(); PresentPreview(result, proof);
                if (!owner || generation != requestGeneration_ || !previewTiming_) return;
                const auto finished = previewTiming_->finishedAtNanoseconds.load();
                if (finished > 0) previewTiming_->deliveryNanoseconds.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() - finished);
            } catch (...) { if (owner && generation == requestGeneration_) { previewTiming_.reset(); status_->setText(tr("后台预览未完成；当前编辑保持。")); } }
        });
        status_->setText(tr("正在准备替换预览；可取消，没有修改地图。")); UpdateSelectionText();
        watcher->setFuture(task.value->future); return;
    }
    const auto result = prepare(revision, request, proof, keys, *replacement, limits);
    if (!guard || requestGeneration_ != generation) return;
    const auto after = getRevision(); if (!guard || requestGeneration_ != generation) return;
    if (after != revision) { ContextChanged(); return; }
    PresentPreview(result, proof);
}
void TriggerReplaceDialog::PresentPreview(const Result<TriggerReplacePreviewResult>& result, const TriggerReplaceQueryProof& proof) {
    if (!result.Succeeded() || !result.value) { previewTiming_.reset(); status_->setText(Failure(result.diagnostics)); return; }
    if (result.value->proof != proof) { ContextChanged(); return; }
    for (const auto& row : result.value->rows) {
        auto* item = new QTreeWidgetItem(previewRows_);
        item->setText(0, Location(row.hit)); item->setText(1, Text(row.hit.oldValue)); item->setText(2, Text(row.replacementValue));
        item->setToolTip(0, Tooltip(item->text(0))); item->setToolTip(1, Tooltip(item->text(1)));
        item->setText(3, Text(row.expectedType)); item->setText(4, row.eligible ? tr("校验通过") : Text(row.reasonCode) + QStringLiteral(" · ") + Text(row.reason));
    }
    preview_ = result.value->preview; applyButton_->setEnabled(preview_.has_value()); tabs_->setCurrentIndex(1);
    status_->setText(preview_ ? tr("%1 项全部校验通过；尚未修改地图。应用后可一次撤销。")
        .arg(Number(result.value->rows.size()))
        : tr("所选项中存在拒绝；整批不能应用。请取消对应勾选后重新预览，没有静默跳过任何项。"));
}
void TriggerReplaceDialog::ApplyPreview() {
    if (!preview_ || !revision_ || !apply_) return;
    const auto preview = *preview_; const auto expected = preview.Proof().contextRevision; const auto limits = limits_;
    const auto generation = ++requestGeneration_; const QPointer<TriggerReplaceDialog> guard(this);
    const auto getRevision = revision_; const auto apply = apply_; const auto revision = getRevision();
    if (!guard || requestGeneration_ != generation) return;
    if (revision != expected) { ContextChanged(); return; }
    applyButton_->setEnabled(false);
    const auto status = apply(expected, preview, limits);
    if (!guard || requestGeneration_ != generation) return;
    ContextChanged(); status_->setText(status.Succeeded() ? tr("已应用整批替换；可一次撤销。请重新查找当前内容。") : Failure(status.diagnostics));
}

} // namespace War3Map::Modules
