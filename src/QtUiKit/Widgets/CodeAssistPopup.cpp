#include "Widgets/CodeAssistPopup.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Widgets/RoundedPopups.h"

#include <QAbstractListModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>

#include <algorithm>

namespace War3Map::UiKit {
namespace {
bool WordStart(const QString& text, int index) {
    if (index == 0) return true;
    const auto previous = text[index - 1], current = text[index];
    if (previous == '_' || previous == '.') return current != '_';
    return current.isUpper() && !previous.isUpper();
}
// 词首缩写：前缀每个字符依次落在单词开头（不区分大小写）；返回匹配下标。
QVector<int> HumpMatch(const QString& label, const QString& prefix) {
    QVector<int> matches;
    int at = 0;
    for (const auto ch : prefix) {
        while (at < label.size() && !(WordStart(label, at) && label[at].toLower() == ch.toLower())) ++at;
        if (at == label.size()) return {};
        matches.push_back(at++);
    }
    return matches;
}
QVector<int> Subsequence(const QString& label, const QString& prefix) {
    QVector<int> matches;
    int at = 0;
    for (const auto ch : prefix) {
        while (at < label.size() && label[at].toLower() != ch.toLower()) ++at;
        if (at == label.size()) return {};
        matches.push_back(at++);
    }
    return matches;
}
int Tier(const QString& label, const QString& prefix) {
    if (prefix.isEmpty()) return 0;
    if (label.startsWith(prefix, Qt::CaseSensitive)) return 0;
    if (label.startsWith(prefix, Qt::CaseInsensitive)) return 1;
    if (!HumpMatch(label, prefix).isEmpty()) return 2;
    if (label.contains(prefix, Qt::CaseInsensitive)) return 3;
    if (prefix.size() >= 2 && !Subsequence(label, prefix).isEmpty()) return 4;
    return -1;
}
QIcon KindIcon(CodeCompletionKind kind) {
    switch (kind) {
    case CodeCompletionKind::Function:
    case CodeCompletionKind::Method: return SvgIcon(QStringLiteral("lucide/box"), QColor(0xb1, 0x80, 0xd7));
    case CodeCompletionKind::Field: return SvgIcon(QStringLiteral("lucide/square"), QColor(0x75, 0xbe, 0xff));
    case CodeCompletionKind::Variable:
    case CodeCompletionKind::Local:
    case CodeCompletionKind::Parameter: return SvgIcon(QStringLiteral("lucide/variable"), QColor(0x75, 0xbe, 0xff));
    case CodeCompletionKind::Constant: return SvgIcon(QStringLiteral("lucide/hash"), QColor(0xd3, 0xba, 0x79));
    case CodeCompletionKind::Module: return SvgIcon(QStringLiteral("lucide/puzzle"), QColor(0xe8, 0xab, 0x53));
    case CodeCompletionKind::Type: return SvgIcon(QStringLiteral("lucide/layers"), QColor(0x4e, 0xc9, 0xb0));
    case CodeCompletionKind::Keyword: return SvgIcon(QStringLiteral("lucide/code-xml"), QColor(0x9d, 0xa5, 0xb4));
    case CodeCompletionKind::Text: break;
    }
    return SvgIcon(QStringLiteral("lucide/file"), QColor(0x9d, 0xa5, 0xb4));
}
QString Html(const QString& text) { return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>")); }

class CandidateModel final : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    void Reset(const QVector<CodeCompletionCandidate>* items, const QVector<int>* visible) {
        beginResetModel(); items_ = items; visible_ = visible; endResetModel();
    }
    int rowCount(const QModelIndex& parent) const override { return parent.isValid() || !visible_ ? 0 : static_cast<int>(visible_->size()); }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || !visible_ || index.row() >= visible_->size()) return {};
        const auto& item = (*items_)[(*visible_)[index.row()]];
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return item.label;
        if (role == Qt::DecorationRole) return KindIcon(item.kind);
        if (role == Qt::ToolTipRole) return {};
        return {};
    }
private:
    const QVector<CodeCompletionCandidate>* items_ = nullptr;
    const QVector<int>* visible_ = nullptr;
};

class CandidateDelegate final : public QStyledItemDelegate {
public:
    CandidateDelegate(const QString* filter, QObject* parent) : QStyledItemDelegate(parent), filter_(filter) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {option.rect.width(), QFontMetrics(option.font).height() + 6};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        const auto& palette = option.palette;
        const bool selected = option.state & QStyle::State_Selected;
        if (selected) {
            painter->fillRect(option.rect, palette.brush(QPalette::Highlight));
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), 2, option.rect.height()), palette.brush(QPalette::Accent));
        } else if (option.state & QStyle::State_MouseOver) {
            painter->fillRect(option.rect, palette.brush(QPalette::Light));
        }
        const int iconSize = 16;
        const QRect icon(option.rect.left() + 6, option.rect.center().y() - iconSize / 2 + 1, iconSize, iconSize);
        index.data(Qt::DecorationRole).value<QIcon>().paint(painter, icon);
        const auto label = index.data().toString();
        const auto matches = CompletionMatches(label, *filter_);
        QFont normal = option.font, bold = option.font;
        bold.setBold(true);
        int x = icon.right() + 7;
        const int right = option.rect.right() - 6;
        painter->setPen(palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        // 按匹配与否分段绘制，匹配段加粗。
        int start = 0;
        while (start < label.size() && x < right) {
            const bool matched = matches.contains(start);
            int end = start + 1;
            while (end < label.size() && matches.contains(end) == matched) ++end;
            const auto piece = label.mid(start, end - start);
            const auto& font = matched ? bold : normal;
            painter->setFont(font);
            const QFontMetrics metrics(font);
            const auto text = metrics.elidedText(piece, Qt::ElideRight, right - x);
            painter->drawText(QRect(x, option.rect.top(), right - x, option.rect.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
            x += metrics.horizontalAdvance(text);
            start = end;
        }
        painter->restore();
    }
private:
    const QString* filter_;
};
QRect ScreenFor(const QRect& globalCursor) {
    auto* screen = QGuiApplication::screenAt(globalCursor.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}
void ConfigureFloating(QFrame& frame, QWidget* editor) {
    frame.setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    frame.setAttribute(Qt::WA_ShowWithoutActivating);
    frame.setFocusPolicy(Qt::NoFocus);
    frame.setPalette(editor->palette());
    frame.setFont(editor->font());
    frame.setProperty("forgeSurfaceOwnPaint", true);
}
} // namespace

QVector<int> RankCompletions(const QVector<CodeCompletionCandidate>& items, const QString& prefix) {
    QVector<QPair<int, int>> ranked; // (档次, 下标)
    for (int index = 0; index < items.size(); ++index)
        if (const auto tier = Tier(items[index].label, prefix); tier >= 0) ranked.push_back({tier, index});
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& left, const auto& right) {
        if (left.first != right.first) return left.first < right.first;
        const auto& a = items[left.second].label;
        const auto& b = items[right.second].label;
        const auto order = a.compare(b, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a < b;
    });
    QVector<int> result;
    result.reserve(ranked.size());
    for (const auto& [tier, index] : ranked) result.push_back(index);
    return result;
}

int CompletionTier(const QString& label, const QString& prefix) { return Tier(label, prefix); }

QVector<int> CompletionMatches(const QString& label, const QString& prefix) {
    if (prefix.isEmpty()) return {};
    QVector<int> matches;
    if (label.startsWith(prefix, Qt::CaseInsensitive)) {
        for (int index = 0; index < prefix.size(); ++index) matches.push_back(index);
        return matches;
    }
    if (auto hump = HumpMatch(label, prefix); !hump.isEmpty()) return hump;
    if (const auto at = label.indexOf(prefix, 0, Qt::CaseInsensitive); at >= 0) {
        for (int index = 0; index < prefix.size(); ++index) matches.push_back(at + index);
        return matches;
    }
    return Subsequence(label, prefix);
}

QString CodeCompletionKindText(CodeCompletionKind kind) {
    switch (kind) {
    case CodeCompletionKind::Keyword: return QObject::tr("关键字");
    case CodeCompletionKind::Module: return QObject::tr("模块");
    case CodeCompletionKind::Function: return QObject::tr("函数");
    case CodeCompletionKind::Method: return QObject::tr("方法");
    case CodeCompletionKind::Field: return QObject::tr("字段");
    case CodeCompletionKind::Variable: return QObject::tr("变量");
    case CodeCompletionKind::Constant: return QObject::tr("常量");
    case CodeCompletionKind::Local: return QObject::tr("局部变量");
    case CodeCompletionKind::Parameter: return QObject::tr("参数");
    case CodeCompletionKind::Type: return QObject::tr("类型");
    case CodeCompletionKind::Text: break;
    }
    return {};
}

CodeCompletionPopup::CodeCompletionPopup(QWidget* editor) : QFrame(editor) {
    setObjectName(QStringLiteral("codeCompletionPopup"));
    ConfigureFloating(*this, editor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    list_ = new QListView;
    list_->setObjectName(QStringLiteral("codeCompletionList"));
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setMouseTracking(true);
    list_->setUniformItemSizes(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->viewport()->setAutoFillBackground(false);
    list_->setAutoFillBackground(false);
    auto* model = new CandidateModel(list_);
    list_->setModel(model);
    list_->setItemDelegate(new CandidateDelegate(&filter_, list_));
    layout->addWidget(list_);
    details_ = new QLabel;
    details_->setObjectName(QStringLiteral("codeCompletionDetails"));
    details_->setTextFormat(Qt::RichText);
    details_->setWordWrap(true);
    details_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details_->setContentsMargins(10, 6, 10, 8);
    details_->setFixedWidth(340);
    details_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(details_);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { UpdateDetails(); });
    connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) { list_->setCurrentIndex(index); });
    connect(list_, &QListView::doubleClicked, this, [this] { emit Accepted(); });
    ConfigureRoundedPopup(*this);
    hide();
}

void CodeCompletionPopup::SetItems(QVector<CodeCompletionCandidate> items) {
    items_ = std::move(items);
    filter_.clear();
    visible_.clear();
    static_cast<CandidateModel*>(list_->model())->Reset(&items_, &visible_);
}

bool CodeCompletionPopup::SetFilter(const QString& prefix) {
    filter_ = prefix;
    visible_ = RankCompletions(items_, prefix);
    static_cast<CandidateModel*>(list_->model())->Reset(&items_, &visible_);
    if (!visible_.isEmpty()) list_->setCurrentIndex(list_->model()->index(0, 0));
    UpdateDetails();
    return !visible_.isEmpty();
}

void CodeCompletionPopup::ShowNear(const QRect& globalCursor) {
    const QFontMetrics metrics(font());
    const int row = metrics.height() + 6;
    int labelWidth = 0;
    for (const auto index : visible_) {
        labelWidth = (std::max)(labelWidth, metrics.horizontalAdvance(items_[index].label));
        if (labelWidth > 400) break;
    }
    const int rows = (std::min)(static_cast<int>(visible_.size()), 12);
    list_->setFixedSize(std::clamp(labelWidth + 52, 240, 420), (std::max)(1, rows) * row + 4);
    details_->setMinimumHeight(list_->height());
    adjustSize();
    const auto margins = layout()->contentsMargins();
    const auto area = ScreenFor(globalCursor);
    QPoint at(globalCursor.left() - margins.left() - 28, globalCursor.bottom() + 2 - margins.top());
    if (at.y() + height() > area.bottom() && globalCursor.top() - height() + margins.bottom() >= area.top())
        at.setY(globalCursor.top() - height() + margins.bottom() - 2);
    at.setX(std::clamp(at.x(), area.left(), (std::max)(area.left(), area.right() - width())));
    move(at);
    show();
    raise();
}

void CodeCompletionPopup::MoveSelection(int delta) {
    const int count = static_cast<int>(visible_.size());
    if (count == 0) return;
    int row = list_->currentIndex().isValid() ? list_->currentIndex().row() : 0;
    if (delta == 1 || delta == -1) row = (row + delta + count) % count; // 单步到头循环
    else row = std::clamp(row + delta, 0, count - 1);
    list_->setCurrentIndex(list_->model()->index(row, 0));
}

const CodeCompletionCandidate* CodeCompletionPopup::Current() const {
    const auto index = list_->currentIndex();
    if (!index.isValid() || index.row() >= visible_.size()) return nullptr;
    return &items_[visible_[index.row()]];
}

void CodeCompletionPopup::UpdateDetails() {
    const auto* item = Current();
    if (!item || (item->detail.isEmpty() && item->documentation.isEmpty())) {
        details_->hide();
        if (isVisible()) adjustSize();
        return;
    }
    const auto muted = palette().color(QPalette::PlaceholderText).name();
    QString html = QStringLiteral("<div style=\"font-family:'%1'\">%2</div>")
        .arg(MonoFont().family(), Html(item->detail.isEmpty() ? item->label : item->detail));
    if (const auto kind = CodeCompletionKindText(item->kind); !kind.isEmpty())
        html += QStringLiteral("<div style='color:%1; margin-top:2px'>%2</div>").arg(muted, kind.toHtmlEscaped());
    if (!item->documentation.isEmpty()) html += QStringLiteral("<p style='margin-top:6px'>%1</p>").arg(Html(item->documentation));
    details_->setText(html);
    const bool wasHidden = details_->isHidden();
    details_->show();
    if (isVisible() && wasHidden) adjustSize();
}

void CodeCompletionPopup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    PaintRoundedSurface(painter, QRectF(rect()), palette());
    if (details_->isVisible()) {
        painter.setPen(palette().color(QPalette::Mid));
        const int x = details_->geometry().left();
        painter.drawLine(x, details_->geometry().top() + 4, x, details_->geometry().bottom() - 4);
    }
}

CodeSignatureTip::CodeSignatureTip(QWidget* editor) : QFrame(editor) {
    setObjectName(QStringLiteral("codeSignatureTip"));
    ConfigureFloating(*this, editor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    label_ = new QLabel;
    label_->setTextFormat(Qt::RichText);
    label_->setWordWrap(true);
    label_->setContentsMargins(10, 5, 10, 6);
    layout->addWidget(label_);
    ConfigureRoundedPopup(*this);
    hide();
}

void CodeSignatureTip::ShowHelp(const CodeSignatureHelp& help, const QRect& globalCursor, bool preferBelow) {
    const auto accent = palette().color(QPalette::Accent).name();
    const auto muted = palette().color(QPalette::PlaceholderText).name();
    QString signature;
    int at = 0;
    if (help.active >= 0 && help.active < help.parameters.size()) {
        const auto [begin, end] = help.parameters[help.active];
        signature = help.label.left(begin).toHtmlEscaped()
            + QStringLiteral("<b style='color:%1'>%2</b>").arg(accent, help.label.mid(begin, end - begin).toHtmlEscaped());
        at = end;
    }
    signature += help.label.mid(at).toHtmlEscaped();
    QString html = QStringLiteral("<div style=\"font-family:'%1'\">%2</div>").arg(MonoFont().family(), signature);
    if (!help.documentation.isEmpty())
        html += QStringLiteral("<div style='color:%1; margin-top:3px'>%2</div>").arg(muted, Html(help.documentation));
    label_->setText(html);
    const auto area = ScreenFor(globalCursor);
    const QFontMetrics metrics(MonoFont());
    label_->setFixedWidth(std::clamp(metrics.horizontalAdvance(help.label) + 32, 260, (std::min)(720, area.width() - 40)));
    adjustSize();
    const auto margins = layout()->contentsMargins();
    QPoint position(globalCursor.left() - margins.left() - 10, globalCursor.top() - height() + margins.bottom() - 2);
    if (preferBelow || position.y() < area.top()) position.setY(globalCursor.bottom() + 2 - margins.top());
    position.setX(std::clamp(position.x(), area.left(), (std::max)(area.left(), area.right() - width())));
    move(position);
    show();
    raise();
}

QString CodeSignatureTip::Text() const { return label_->text(); }

void CodeSignatureTip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    PaintRoundedSurface(painter, QRectF(rect()), palette());
}

} // namespace War3Map::UiKit
