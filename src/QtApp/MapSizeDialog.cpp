#include "MapSizeDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace War3Map::QtApp {
namespace {
QString Text(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
QString Bytes(std::optional<std::uint64_t> value) {
    return value ? QObject::tr("%1 B").arg(QString::number(*value)) : QObject::tr("未知");
}
QString Category(Services::MapSizeCategory category) {
    using C = Services::MapSizeCategory;
    switch (category) {
    case C::Model: return QObject::tr("模型");
    case C::Texture: return QObject::tr("纹理");
    case C::Sound: return QObject::tr("声音");
    case C::Script: return QObject::tr("脚本");
    default: return QObject::tr("其他");
    }
}
QString Trust(Services::MapSizeByteTrust trust) {
    switch (trust) {
    case Services::MapSizeByteTrust::OwnedMetadata: return QObject::tr("已持有元数据");
    case Services::MapSizeByteTrust::ReportedUnverified: return QObject::tr("外盘上报，未核验");
    default: return QObject::tr("未知");
    }
}
QString State(Services::MapSizeState state) {
    switch (state) {
    case Services::MapSizeState::Ready: return QObject::tr("已捕获范围完整");
    case Services::MapSizeState::Partial: return QObject::tr("部分覆盖");
    default: return QObject::tr("未知");
    }
}
void Cell(QTableWidget* table, int row, int column, const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, column, item);
}
} // namespace

MapSizeDialog::MapSizeDialog(Revision revision, Query query, Activate activate, QWidget* parent)
    : QDialog(parent), revision_(std::move(revision)), query_(std::move(query)), activate_(std::move(activate)) {
    setObjectName(QStringLiteral("mapSizeDialog"));
    setWindowTitle(tr("地图体积分析 · Audastra")); setModal(false); resize(980, 680); setMinimumSize(760, 540);
    auto* layout = new QVBoxLayout(this);
    scope_ = new QLabel(this); scope_->setObjectName(QStringLiteral("mapSizeScope"));
    scope_->setTextFormat(Qt::PlainText); scope_->setWordWrap(true); layout->addWidget(scope_);
    totals_ = new QTableWidget(this); totals_->setObjectName(QStringLiteral("mapSizeCategoryTotals"));
    totals_->setColumnCount(5); totals_->setHorizontalHeaderLabels({tr("类别"), tr("原始：已知逻辑字节"),
        tr("当前：已知逻辑字节"), tr("当前：上报未核验字节"), tr("当前：未知大小项")});
    totals_->verticalHeader()->hide(); totals_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    totals_->setSelectionMode(QAbstractItemView::NoSelection); totals_->setMinimumHeight(220); layout->addWidget(totals_);
    top_ = new QTableWidget(this); top_->setObjectName(QStringLiteral("mapSizeTopMembers")); top_->setColumnCount(4);
    top_->setHorizontalHeaderLabels({tr("当前成员路径"), tr("类别"), tr("当前逻辑字节"), tr("大小来源")});
    top_->verticalHeader()->hide(); top_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column) top_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    top_->setSelectionBehavior(QAbstractItemView::SelectRows); top_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(top_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("mapSizeStatus")); status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (auto* close = buttons->button(QDialogButtonBox::Close)) close->setText(tr("关闭"));
    auto* refresh = buttons->addButton(tr("刷新"), QDialogButtonBox::ActionRole);
    refresh->setObjectName(QStringLiteral("mapSizeRefresh"));
    for (auto* button : buttons->findChildren<QPushButton*>()) button->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(refresh, &QPushButton::clicked, this, [this] { Refresh(); });
    connect(top_, &QTableWidget::itemActivated, this, [this] { ActivateCurrent(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
}

void MapSizeDialog::Focus() {
    const QPointer<MapSizeDialog> dialog(this);
    show(); raise(); activateWindow(); Refresh();
    if (dialog) top_->setFocus();
}
void MapSizeDialog::ContextChanged() {
    ++requestGeneration_; report_.reset(); totals_->setRowCount(0); top_->setRowCount(0);
    scope_->clear(); status_->setText(tr("内容已变化，请刷新"));
}
void MapSizeDialog::Refresh() {
    const auto generation = ++requestGeneration_;
    const QPointer<MapSizeDialog> dialog(this);
    const auto revision = revision_; const auto query = query_;
    report_.reset(); totals_->setRowCount(0); top_->setRowCount(0); scope_->clear();
    if (!revision || !query) { status_->setText(tr("没有可分析的地图")); return; }
    const auto expected = revision();
    if (!dialog || generation != requestGeneration_) return;
    const auto result = query(expected);
    if (!dialog || generation != requestGeneration_) return;
    const auto current = revision();
    if (!dialog || generation != requestGeneration_) return;
    if (current != expected || (result.value && result.value->contextRevision != expected)) { ContextChanged(); return; }
    if (!result.Succeeded() || !result.value) {
        status_->setText(result.diagnostics.empty() ? tr("读取失败，请刷新重试") : Text(result.diagnostics.front().message)); return;
    }
    // 返回报告自有字符串；窗口不借用 map、正文、索引或资源对象。
    report_ = *result.value; Populate(*report_);
}
void MapSizeDialog::Populate(const Services::MapSizeReport& report) {
    const auto source = report.sourceKind == Services::MapSizeSourceKind::ArchiveMembers
        ? tr("归档成员") : tr("目录快照");
    scope_->setText(tr("%1 · 未压缩字节 · 原始：%2 · 当前：%3")
        .arg(source, State(report.baseline.state), State(report.effective.state)));
    constexpr int categories = static_cast<int>(Services::MapSizeCategory::Count);
    totals_->setRowCount(categories + 1);
    for (int row = 0; row <= categories; ++row) {
        const bool total = row == categories;
        const auto index = static_cast<std::size_t>(row);
        Cell(totals_, row, 0, total ? tr("合计小计") : Category(report.baseline.categories[index].category));
        Cell(totals_, row, 1, Bytes(total ? report.baseline.knownLogicalSubtotal : report.baseline.categories[index].knownBytes));
        Cell(totals_, row, 2, Bytes(total ? report.effective.knownLogicalSubtotal : report.effective.categories[index].knownBytes));
        Cell(totals_, row, 3, Bytes(total ? report.effective.reportedUnverifiedSubtotal : report.effective.categories[index].reportedUnverifiedBytes));
        Cell(totals_, row, 4, report.effectiveUnknown ? tr("未知")
            : QString::number(total ? report.effective.unknownRows : report.effective.categories[index].unknownRows));
    }
    const auto count = (std::min)(report.top20.size(), std::size_t{20}); top_->setRowCount(static_cast<int>(count));
    for (std::size_t index = 0; index < count; ++index) {
        const auto row = static_cast<int>(index); const auto& member = report.top20[index];
        Cell(top_, row, 0, Text(member.currentPath)); Cell(top_, row, 1, Category(member.category));
        Cell(top_, row, 2, Bytes(member.currentLogicalBytes)); Cell(top_, row, 3, Trust(member.trust));
        const auto hint = tr("原始路径：%1\n来源身份：%2\n%3").arg(Text(member.originalPath), Text(member.identity),
            member.navigationAllowed ? tr("可定位当前资源") : tr("只显示元数据，不读取正文或自动编辑受管成员"));
        top_->item(row, 0)->setToolTip(QStringLiteral("<qt>") + hint.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")) + QStringLiteral("</qt>"));
    }
    QStringList notices;
    notices << (report.topWindowComplete ? tr("已捕获范围内最多 20 个最大成员。") : tr("只列已处理窗口内最多 20 个成员，不能称为全图最大成员。"));
    notices << tr("原始快照已列未知大小 %1 项；当前未知大小项数：%2；已检查元数据 %3 项。")
        .arg(report.baseline.unknownRows).arg(report.effectiveUnknown ? tr("未知") : QString::number(report.effective.unknownRows))
        .arg(report.examinedMetadataRows);
    if (report.effectiveUnknown) notices << tr("当前编辑态大小未知，保留原始快照统计，不用旧大小代替。" );
    if (report.inputTruncated || report.outputTruncated) notices << tr("元数据或输出预算已截断范围。" );
    if (report.opaqueMemberCountUnknown) notices << tr("匿名成员数量未知。" );
    else if (report.opaqueMemberCount) notices << tr("另有 %1 个匿名成员，未计已知字节。" ).arg(report.opaqueMemberCount);
    for (const auto& diagnostic : report.diagnostics) notices << Text(diagnostic.message);
    status_->setText(notices.join(QLatin1Char('\n')));
}
void MapSizeDialog::ActivateCurrent() {
    const auto index = top_->currentRow();
    if (!report_ || index < 0 || static_cast<std::size_t>(index) >= report_->top20.size()) return;
    const auto generation = requestGeneration_;
    const QPointer<MapSizeDialog> dialog(this);
    const auto revision = revision_; const auto callback = activate_;
    const auto expected = report_->contextRevision; const auto row = report_->top20[static_cast<std::size_t>(index)];
    if (!revision) { ContextChanged(); return; }
    const auto current = revision();
    if (!dialog || generation != requestGeneration_) return;
    if (current != expected) { ContextChanged(); return; }
    if (callback) callback(row, expected);
}
} // namespace War3Map::QtApp
