#include "CommandPalette.h"

#include "Theme/DesignTokens.generated.h"
#include "Widgets/RoundedPopups.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHideEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
constexpr std::size_t kResultLimit = 100;
enum Role { DetailRole = Qt::UserRole + 1, ShortcutRole, SectionRole, HeaderRole };

class PaletteRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        return {0, index.data(HeaderRole).toBool() ? 26 : index.data(DetailRole).toString().isEmpty() ? 34 : 48};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        const auto& palette = option.palette;
        const bool header = index.data(HeaderRole).toBool();
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = !header && enabled && (option.state & QStyle::State_Selected);
        QRect row = option.rect;
        if (selected) {
            painter->fillRect(row, palette.brush(QPalette::Highlight));
            painter->fillRect(QRect(row.left(), row.top(), 2, row.height()), palette.brush(QPalette::Accent));
        } else if (!header && enabled && (option.state & QStyle::State_MouseOver)) {
            painter->fillRect(row, palette.brush(QPalette::Light));
        }
        row.adjust(12, 0, -12, 0);
        QFont font = option.font;
        if (header) {
            painter->setPen(palette.color(QPalette::PlaceholderText));
            painter->setFont(font);
            painter->drawText(row, Qt::AlignVCenter, index.data().toString());
            painter->setPen(palette.color(QPalette::Mid));
            painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        } else {
            const QString shortcut = index.data(ShortcutRole).toString();
            const QString detail = index.data(DetailRole).toString();
            const int keyWidth = std::min(row.width() / 3, QFontMetrics(font).horizontalAdvance(shortcut) + 14);
            const QRect title(row.left(), row.top(), std::max(0, row.width() - (shortcut.isEmpty() ? 0 : keyWidth + 10)), detail.isEmpty() ? row.height() : 26);
            font.setWeight(QFont::Medium);
            painter->setFont(font);
            painter->setPen(enabled ? palette.color(selected ? QPalette::Accent : QPalette::Text) : palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(title, Qt::AlignVCenter, QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, title.width()));
            painter->setFont(option.font);
            painter->setPen(enabled ? palette.color(QPalette::PlaceholderText) : palette.color(QPalette::Disabled, QPalette::Text));
            if (!shortcut.isEmpty()) {
                const QRect key(row.right() - keyWidth, row.top() + 6, keyWidth, 22);
                painter->drawRoundedRect(key, War3Map::UiKit::Tokens::Size::kRadiusXs,
                    War3Map::UiKit::Tokens::Size::kRadiusXs);
                painter->drawText(key.adjusted(5, 0, -5, 0), Qt::AlignCenter, QFontMetrics(option.font).elidedText(shortcut, Qt::ElideRight, key.width() - 10));
            }
            if (!detail.isEmpty()) {
                const QRect description(row.left(), row.top() + 26, row.width(), 20);
                painter->drawText(description, Qt::AlignVCenter, QFontMetrics(option.font).elidedText(detail, Qt::ElideRight, description.width()));
            }
        }
        painter->restore();
    }
};
} // namespace

CommandPalette::CommandPalette(QWidget* parent) : QDialog(parent) {
    traceInput_ = qEnvironmentVariableIntValue("FORGE_INPUT_TRACE") != 0;
    inputTraceClock_.start();
    if (traceInput_) inputTracePath_ = QDir::temp().filePath(QStringLiteral("FORGE-input-trace-%1.log").arg(QCoreApplication::applicationPid()));
    setObjectName(QStringLiteral("commandPalette"));
    setWindowTitle(tr("命令面板"));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setMaximumWidth(680);
    resize(640, 420);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    searchBox_ = new QLineEdit;
    searchBox_->setObjectName(QStringLiteral("commandPaletteSearch"));
    searchBox_->setAccessibleName(tr("搜索命令或工程内容"));
    searchBox_->setPlaceholderText(tr("搜索命令或工程内容…"));
    searchBox_->setMaxLength(512);
    searchBox_->setClearButtonEnabled(true);
    searchBox_->setAttribute(Qt::WA_InputMethodEnabled);
    searchBox_->installEventFilter(this);
    layout->addWidget(searchBox_);
    listView_ = new QListView;
    listView_->setObjectName(QStringLiteral("commandPaletteResults"));
    listView_->setAccessibleName(tr("搜索结果"));
    listModel_ = new QStandardItemModel(listView_);
    listView_->setModel(listModel_);
    listView_->setItemDelegate(new PaletteRowDelegate(listView_));
    listView_->setMouseTracking(true);
    listView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listView_->setSelectionMode(QAbstractItemView::SingleSelection);
    listView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 小屏幕先收缩可滚动的结果区，搜索框与快捷键页脚始终留在窗口内。
    listView_->setMinimumHeight(0);
    listView_->setMaximumHeight(360);
    listView_->installEventFilter(this);
    layout->addWidget(listView_, 1);
    connect(searchBox_, &QLineEdit::textChanged, this, &CommandPalette::FilterEntries);
    connect(listView_, &QListView::activated, this, &CommandPalette::ActivateIndex);
    connect(listView_, &QListView::clicked, this, &CommandPalette::ActivateIndex);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* focus) {
        if (isVisible()) TraceInput("focus-changed");
        if (focus && focus != this && !isAncestorOf(focus)) CheckFocusLater();
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (isVisible()) TraceInput("application-state", static_cast<int>(state));
        if (state != Qt::ApplicationActive) CheckFocusLater();
    });
    setProperty("forgeSurfaceOwnPaint", true);
    setProperty("forgeSurfaceDialog", true);
    War3Map::UiKit::ConfigureRoundedPopup(*this);
}

void CommandPalette::TraceInput(const char* event, int value1, int value2, int value3, int value4) {
    if (!traceInput_) return;
    const auto role = [this](const QWidget* widget) {
        if (!widget) return "null";
        if (widget == this) return "palette";
        if (widget == searchBox_) return "search";
        if (widget == listView_) return "results";
        if (isAncestorOf(widget)) return "inside";
        if (parentWidget() && (widget == parentWidget()->window() || parentWidget()->window()->isAncestorOf(widget))) return "parent";
        return "other";
    };
    const char* foreground = "unavailable";
#ifdef Q_OS_WIN
    const auto window = reinterpret_cast<quintptr>(GetForegroundWindow());
    foreground = !window ? "null" : window == static_cast<quintptr>(internalWinId()) ? "palette" :
        parentWidget() && window == static_cast<quintptr>(parentWidget()->window()->internalWinId()) ? "parent" : "other";
#endif
    // 只记面板状态与长度，绝不输出输入文字、命令标签或其他窗口标题。
    QString message;
    QDebug(&message).noquote() << "[FORGE_INPUT_TRACE]" << "seq=" << ++inputTraceSequence_ << "ms=" << inputTraceClock_.elapsed()
        << "event=" << event << "visible=" << isVisible() << "composing=" << composing_
        << "focusGeneration=" << focusGeneration_ << "queryGeneration=" << queryGeneration_
        << "focus=" << role(QApplication::focusWidget()) << "active=" << role(QApplication::activeWindow())
        << "popup=" << role(QApplication::activePopupWidget()) << "foreground=" << foreground
        << "application=" << static_cast<int>(QApplication::applicationState()) << "length=" << (searchBox_ ? searchBox_->text().size() : 0)
        << "values=" << value1 << value2 << value3 << value4;
    qInfo().noquote() << message;
    QFile output(inputTracePath_);
    if (output.size() < 1024 * 1024 && output.open(QIODevice::WriteOnly | QIODevice::Append)) output.write(message.toUtf8() + '\n');
}

void CommandPalette::SetEntries(std::vector<Entry> entries) {
    queryProvider_ = {};
    entries_ = std::move(entries);
    FilterEntries(searchBox_->text());
}

void CommandPalette::SetQueryProvider(QueryProvider provider) {
    queryProvider_ = std::move(provider);
    FilterEntries(searchBox_->text());
}

void CommandPalette::Show() { Show(QString()); }

void CommandPalette::DismissForContextChange(int source, int signal) {
    TraceInput("context-invalidated", source, signal);
    hide();
}

void CommandPalette::TraceSourceStateNotice() { TraceInput("notice-source-state"); }

void CommandPalette::done(int result) {
    TraceInput("dialog-done", result);
    QDialog::done(result);
}

void CommandPalette::Show(const QString& initial) {
    TraceInput("show-request");
    ++focusGeneration_;
    composing_ = false; restorePreviousFocus_ = true;
    if (!isVisible()) previousFocus_ = QApplication::focusWidget();
    const QSignalBlocker blocker(searchBox_);
    searchBox_->setText(initial);
    FilterEntries(searchBox_->text());
    auto* parent = parentWidget();
    const auto* screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect bounds = screen->availableGeometry().adjusted(8, 8, -8, -8);
        ensurePolished();
        layout()->activate();
        resize(std::min(640, bounds.width()), std::min(420, bounds.height()));
        const QPoint center = parent ? parent->mapToGlobal(QPoint{parent->width() / 2, parent->height() / 5}) : bounds.center();
        move(std::clamp(center.x() - width() / 2, bounds.left(), std::max(bounds.left(), bounds.right() - width() + 1)),
            std::clamp(center.y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - height() + 1)));
    }
    show();
    raise();
    activateWindow();
    searchBox_->setFocus(Qt::ShortcutFocusReason);
    TraceInput("show-focused");
}

void CommandPalette::FilterEntries(const QString& text) {
    TraceInput("query-begin");
    const auto generation = ++queryGeneration_;
    results_.clear(); rowEntries_.clear(); listModel_->clear();
    const bool commandOnly = text.startsWith(QLatin1Char('>'));
    const QString query = commandOnly ? text.mid(1) : text;
    std::vector<Entry> found;
    try {
        if (queryProvider_) found = queryProvider_(query, commandOnly);
        else {
            for (const auto& entry : entries_) {
                if (entry.label.contains(query, Qt::CaseInsensitive) || entry.detail.contains(query, Qt::CaseInsensitive)) found.push_back(entry);
                if (found.size() == kResultLimit) break;
            }
        }
    } catch (const std::exception& error) {
        found = {{tr("查询失败"), QString::fromUtf8(error.what()).left(512), {}, {}, {}, {}, false}};
    } catch (...) {
        found = {{tr("查询失败"), tr("查询提供者返回未知错误。"), {}, {}, {}, {}, false}};
    }
    if (generation != queryGeneration_) return; // 重入查询不能恢复旧结果。
    if (found.size() > kResultLimit) found.resize(kResultLimit);
    results_ = std::move(found);
    if (results_.empty()) results_.push_back({tr("没有匹配结果"), {}, {}, {}, {}, {}, false});
    QString section;
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const auto& entry = results_[i];
        if (!entry.section.isEmpty() && entry.section != section) {
            auto* header = new QStandardItem(entry.section);
            header->setData(true, HeaderRole);
            header->setFlags(Qt::NoItemFlags);
            listModel_->appendRow(header); rowEntries_.push_back(-1);
        }
        section = entry.section;
        auto* item = new QStandardItem(entry.label);
        item->setData(entry.stableId, Qt::UserRole);
        item->setData(entry.detail, DetailRole);
        item->setData(entry.shortcut, ShortcutRole);
        item->setData(entry.section, SectionRole);
        item->setToolTip(entry.detail.isEmpty() ? entry.label : entry.label + QLatin1Char('\n') + entry.detail);
        item->setFlags(entry.available && entry.action ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
        listModel_->appendRow(item); rowEntries_.push_back(static_cast<int>(i));
    }
    listView_->setCurrentIndex(QModelIndex());
    SelectNext(1);
    TraceInput("query-complete", listModel_->rowCount());
}

void CommandPalette::SelectNext(int direction) {
    for (int row = listView_->currentIndex().row() + direction; row >= 0 && row < listModel_->rowCount(); row += direction) {
        const auto index = listModel_->index(row, 0);
        if (index.flags().testFlag(Qt::ItemIsEnabled)) {
            listView_->setCurrentIndex(index);
            listView_->scrollTo(index, QAbstractItemView::EnsureVisible);
            return;
        }
    }
}

bool CommandPalette::event(QEvent* event) {
    if (event->type() == QEvent::Close) TraceInput("close-event");
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)
        TraceInput(event->type() == QEvent::WindowActivate ? "window-activate" : "window-deactivate");
    const bool handled = QDialog::event(event);
    if (event->type() == QEvent::WindowDeactivate) CheckFocusLater();
    return handled;
}

void CommandPalette::CheckFocusLater() {
    if (!isVisible()) return;
    const auto generation = focusGeneration_;
    TraceInput("focus-check-queued");
    QTimer::singleShot(0, this, [this, generation] {
        TraceInput("focus-check-fired");
        if (!isVisible() || generation != focusGeneration_) { TraceInput("focus-check-stale"); return; }
        const auto* active = QApplication::activeWindow();
        const bool applicationActive = QApplication::applicationState() == Qt::ApplicationActive;
        if (applicationActive && active == this) { TraceInput("focus-check-keep-active"); return; }
        const auto* popup = QApplication::activePopupWidget();
        if (applicationActive && popup && isAncestorOf(popup)) { TraceInput("focus-check-keep-popup"); return; }
        // 原生输入法候选可能暂时没有 Qt 焦点窗口；应用仍活动且在组合时不误关。
        if (!active && composing_ && applicationActive) { TraceInput("focus-check-keep-composition"); return; }
        TraceInput("focus-check-hide");
        restorePreviousFocus_ = false;
        hide();
    });
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == searchBox_ && event->type() == QEvent::InputMethod) {
        const auto* input = static_cast<QInputMethodEvent*>(event);
        TraceInput("ime-before", static_cast<int>(input->preeditString().size()), static_cast<int>(input->commitString().size()), input->replacementStart(), input->replacementLength());
        composing_ = !input->preeditString().isEmpty();
        TraceInput("ime-state", static_cast<int>(input->preeditString().size()), static_cast<int>(input->commitString().size()));
    }
    if ((watched == searchBox_ || watched == listView_) && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
        TraceInput(event->type() == QEvent::FocusIn ? "focus-in" : "focus-out", static_cast<int>(static_cast<QFocusEvent*>(event)->reason()));
    if ((watched == searchBox_ || watched == listView_) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        // 1 Enter，2 Escape，3 Up/Down，4 数字，0其他；不记录字母或文本。
        const int category = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ? 1 : key->key() == Qt::Key_Escape ? 2 :
            key->key() == Qt::Key_Down || key->key() == Qt::Key_Up ? 3 : key->key() >= Qt::Key_0 && key->key() <= Qt::Key_9 ? 4 : 0;
        TraceInput("key-press", category);
        if (watched == searchBox_ && composing_) {
            if (key->key() == Qt::Key_Escape) { QApplication::inputMethod()->reset(); composing_ = false; return true; }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { QApplication::inputMethod()->commit(); return true; }
            // 组合态的上下交给输入框与输入法，不能移动命令结果。
            if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)
                return QDialog::eventFilter(watched, event);
        }
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) { SelectNext(key->key() == Qt::Key_Down ? 1 : -1); return true; }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { ActivateCurrent(); return true; }
        if (key->key() == Qt::Key_Escape) { hide(); return true; }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::ActivateCurrent() {
    ActivateIndex(listView_->currentIndex());
}

void CommandPalette::ActivateIndex(const QModelIndex& index) {
    TraceInput("activate-request", index.row());
    if (!index.isValid() || index.model() != listModel_ || !index.flags().testFlag(Qt::ItemIsEnabled)) return;
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(rowEntries_.size())) return;
    const int entryIndex = rowEntries_[row];
    if (entryIndex < 0 || entryIndex >= static_cast<int>(results_.size())) return;
    const auto& entry = results_[entryIndex];
    if (!entry.available || !entry.action) return;
    const auto action = entry.action;
    TraceInput("activate-action", index.row());
    const QPointer<CommandPalette> alive(this);
    hide();
    action();
    if (alive) emit alive->EntryActivated();
}

void CommandPalette::hideEvent(QHideEvent* event) {
    TraceInput("hide-event", restorePreviousFocus_ ? 1 : 0);
    ++focusGeneration_; composing_ = false;
    QDialog::hideEvent(event);
    if (restorePreviousFocus_ && previousFocus_ && previousFocus_->isVisible() && previousFocus_->isEnabled()) {
        if (QApplication::applicationState() == Qt::ApplicationActive) previousFocus_->window()->activateWindow();
        previousFocus_->setFocus(Qt::ShortcutFocusReason);
    }
    previousFocus_.clear();
}

void CommandPalette::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    War3Map::UiKit::PaintRoundedSurface(painter, QRectF(rect()), palette(), true);
}
