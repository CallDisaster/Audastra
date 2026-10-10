#include "Widgets/WarcraftTextPreview.h"

#include "War3MapCore/Text/WarcraftText.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>

#include <exception>
#include <utility>

namespace War3Map::UiKit {
namespace {

constexpr int MaxEditorCharacters = 256 * 1024;

// 纯文字插入之外再拒绝资源请求，来源里的 HTML/img/link 没有执行通道。
class PlainDocument final : public QTextDocument {
public:
    using QTextDocument::QTextDocument;
protected:
    QVariant loadResource(int, const QUrl&) override { return {}; }
};

bool StringReference(const QString& text) {
    if (!text.startsWith(QStringLiteral("TRIGSTR_")) || text.size() <= 8) return false;
    for (qsizetype index = 8; index < text.size(); ++index) {
        if (text[index] < QLatin1Char('0') || text[index] > QLatin1Char('9')) return false;
    }
    return true;
}

} // namespace

WarcraftTextPreview::WarcraftTextPreview(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("warcraftTextPreview"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);
    auto* tools = new QHBoxLayout;
    auto* title = new QLabel(tr("预览"), this);
    title->setTextFormat(Qt::PlainText);
    tools->addWidget(title);
    tools->addStretch();
    colour_ = new QPushButton(tr("颜色…"), this);
    colour_->setToolTip(tr("取色后仅在当前草稿插入 |cffRRGGBB"));
    colour_->setObjectName(QStringLiteral("warcraftTextColour"));
    reset_ = new QPushButton(tr("重置"), this);
    reset_->setToolTip(tr("仅在当前草稿插入恢复颜色标记 |r"));
    reset_->setObjectName(QStringLiteral("warcraftTextReset"));
    newline_ = new QPushButton(tr("换行"), this);
    newline_->setToolTip(tr("仅在当前草稿插入换行标记 |n"));
    newline_->setObjectName(QStringLiteral("warcraftTextNewline"));
    for (auto* button : {colour_, reset_, newline_}) {
        button->setProperty("quiet", true);
        tools->addWidget(button);
    }
    outer->addLayout(tools);
    preview_ = new QTextEdit(this);
    preview_->setObjectName(QStringLiteral("warcraftTextRendered"));
    preview_->setDocument(new PlainDocument(preview_));
    preview_->document()->setUndoRedoEnabled(false);
    preview_->setReadOnly(true);
    preview_->setAcceptRichText(false);
    preview_->setMinimumHeight(62);
    preview_->setMaximumHeight(132);
    preview_->setTabChangesFocus(true);
    outer->addWidget(preview_);
    source_ = new QLabel(this);
    source_->setObjectName(QStringLiteral("warcraftTextSource"));
    source_->setTextFormat(Qt::PlainText);
    source_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    source_->setWordWrap(true);
    source_->setVisible(false);
    outer->addWidget(source_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("warcraftTextStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setWordWrap(true);
    outer->addWidget(status_);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this] { Refresh(); });
    connect(colour_, &QPushButton::clicked, this, [this] { ChooseColour(); });
    connect(reset_, &QPushButton::clicked, this, [this] { (void)InsertReset(); });
    connect(newline_, &QPushButton::clicked, this, [this] { (void)InsertNewline(); });
    Refresh();
}

void WarcraftTextPreview::ResetBinding(const QString& explanation, bool available) {
    timer_->stop();
    if (multiline_) disconnect(multiline_, nullptr, this, nullptr);
    if (singleline_) disconnect(singleline_, nullptr, this, nullptr);
    multiline_.clear(); singleline_.clear();
    ++generation_;
    rendered_ = false;
    preview_->clear();
    sourceAvailable_ = available;
    sourceResolver_ = {};
    source_->setText(explanation);
    source_->setVisible(!explanation.isEmpty());
}

void WarcraftTextPreview::Bind(QPlainTextEdit* editor, const QString& explanation, bool available,
    WarcraftTextSourceResolver resolver) {
    ResetBinding(explanation, available);
    sourceResolver_ = std::move(resolver);
    multiline_ = editor;
    if (editor) {
        connect(editor, &QPlainTextEdit::textChanged, this, [this] { Schedule(); });
        connect(editor, &QObject::destroyed, this, [this] { ++generation_; Refresh(); });
    }
    Refresh();
}

void WarcraftTextPreview::Bind(QLineEdit* editor, const QString& explanation, bool available,
    WarcraftTextSourceResolver resolver) {
    ResetBinding(explanation, available);
    sourceResolver_ = std::move(resolver);
    singleline_ = editor;
    if (editor) {
        connect(editor, &QLineEdit::textChanged, this, [this] { Schedule(); });
        connect(editor, &QObject::destroyed, this, [this] { ++generation_; Refresh(); });
    }
    Refresh();
}

void WarcraftTextPreview::Schedule() {
    ++generation_;
    timer_->start();
    const bool allowed = CanInsert();
    colour_->setEnabled(allowed); reset_->setEnabled(allowed); newline_->setEnabled(allowed);
}

bool WarcraftTextPreview::CanInsert() const {
    if (!sourceAvailable_ || (!multiline_ && !singleline_)) return false;
    if ((multiline_ && multiline_->isReadOnly()) || (singleline_ && singleline_->isReadOnly())) return false;
    // 只为短引用读取草稿；大正文不因预览帽失去原编辑/插入资格。
    if (multiline_ && multiline_->document()->characterCount() <= 64)
        return !StringReference(multiline_->toPlainText());
    if (singleline_ && singleline_->text().size() <= 64)
        return !StringReference(singleline_->text());
    return true;
}

void WarcraftTextPreview::Refresh() {
    timer_->stop();
    const bool allowed = CanInsert();
    colour_->setEnabled(allowed); reset_->setEnabled(allowed); newline_->setEnabled(allowed);
    const auto stale = [this](const QString& reason) {
        status_->setText(rendered_ ? reason + tr(" 下方保留上次预览，未代表当前草稿。") : reason);
    };
    if (!multiline_ && !singleline_) { stale(tr("当前编辑框不可用。")); return; }
    if (!sourceAvailable_) { stale(tr("当前文字来源或编码未确认；原文保持不变。")); return; }
    // 取完整 QString/UTF-8 前先读取 O(1) 文档长度，最多 256K UTF-16 字符。
    if (multiline_ && multiline_->document()->characterCount() - 1 > MaxEditorCharacters) {
        stale(tr("正文超过 256K 字符预览预算；编辑内容保持完整。")); return;
    }
    QString text = multiline_ ? multiline_->toPlainText() : singleline_->text();
    if (text.size() > MaxEditorCharacters) {
        stale(tr("正文超过 256K 字符预览预算；编辑内容保持完整。")); return;
    }
    if (!sourceResolver_ && StringReference(text)) {
        stale(tr("这是未解析的 WTS 引用，当前无法确认其正文；不会把编号当作文字。")); return;
    }
    const QPointer<WarcraftTextPreview> lifetime(this);
    const auto startingGeneration = generation_;
    try {
        // 先验证原草稿，再消费宿主准备的文字。回调可重绑/销毁控件，故持局部副本并双验代次。
        const auto originalBytes = text.toUtf8();
        if (QString::fromUtf8(originalBytes) != text) {
            stale(tr("草稿包含无法精确往返的 Unicode 字符；原文保持不变。")); return;
        }
        if (sourceResolver_) {
            const QPointer<WarcraftTextPreview> guard(this);
            const auto generation = generation_;
            const auto resolver = sourceResolver_;
            const auto resolved = resolver(text);
            if (!guard || guard->generation_ != generation) return;
            source_->setText(resolved.explanation);
            source_->setVisible(!resolved.explanation.isEmpty());
            if (resolved.state != WarcraftTextSourceState::Available) {
                stale(resolved.state == WarcraftTextSourceState::Stale
                    ? tr("预览来源已失效；请关闭并重新打开字段编辑。")
                    : tr("当前没有可确认的正文；原字段值保持不变。"));
                return;
            }
            text = resolved.text;
            if (text.size() > MaxEditorCharacters) {
                stale(tr("正文超过 256K 字符预览预算；编辑内容保持完整。")); return;
            }
        }
        const QByteArray bytes = text.toUtf8();
        if (QString::fromUtf8(bytes) != text) {
            stale(tr("草稿包含无法精确往返的 Unicode 字符；原文保持不变。")); return;
        }
        const auto parsed = Text::ParseWarcraftText(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        preview_->clear();
        QTextCursor cursor(preview_->document());
        cursor.beginEditBlock();
        for (const auto& run : parsed.runs) {
            QTextCharFormat format;
            if (run.argb) {
                const auto rgb = *run.argb & 0x00FFFFFFu;
                format.setForeground(QColor::fromRgb(rgb)); // 保留源 alpha，但不猜测 Warcraft 的 alpha 语义。
            }
            const auto view = std::string_view(parsed.plainText).substr(run.textOffset, run.textBytes);
            cursor.insertText(QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size())), format);
        }
        cursor.endEditBlock();
        preview_->document()->setUndoRedoEnabled(false);
        preview_->moveCursor(QTextCursor::Start);
        rendered_ = true;
        if (parsed.state == Text::WarcraftTextState::Truncated)
            status_->setText(tr("预览已截断；原文与应用资格保持不变。"));
        else if (!parsed.Complete()) status_->setText(tr("本次预览未完成；下方只显示已检查前缀。原文保持不变。"));
        else if (!parsed.diagnostics.empty() || parsed.omittedDiagnostics)
            status_->setText(tr("未知或不完整的格式标记按字面显示；预览只模拟 RGB 颜色和换行。"));
        else status_->setText(tr("预览只模拟 RGB 颜色和换行；游戏字体、alpha 和额外热键规则尚未验证。"));
    } catch (const std::exception&) {
        if (lifetime && lifetime->generation_ == startingGeneration)
            stale(tr("预览准备失败；原文保持不变。"));
    }
}

bool WarcraftTextPreview::Insert(const QString& text) {
    if (!CanInsert()) return false;
    const QPointer<WarcraftTextPreview> guard(this);
    if (multiline_) {
        const QPointer<QPlainTextEdit> editor(multiline_);
        QTextCursor cursor = editor->textCursor();
        cursor.beginEditBlock(); cursor.insertText(text); cursor.endEditBlock();
        if (!guard || !editor) return false;
        editor->setTextCursor(cursor);
    } else if (singleline_) {
        const QPointer<QLineEdit> editor(singleline_);
        const qsizetype removed = editor->selectedText().size();
        if (editor->text().size() - removed + text.size() > editor->maxLength()) return false;
        editor->insert(text);
    } else return false;
    if (!guard) return false;
    guard->Refresh();
    return true;
}

bool WarcraftTextPreview::InsertColour(const QColor& colour) {
    if (!colour.isValid()) return false;
    const auto code = Text::WarcraftColourCode(colour.rgb());
    return Insert(QString::fromLatin1(code.data(), static_cast<qsizetype>(code.size())));
}
bool WarcraftTextPreview::InsertReset() { return Insert(QStringLiteral("|r")); }
bool WarcraftTextPreview::InsertNewline() { return Insert(QStringLiteral("|n")); }
QString WarcraftTextPreview::StatusText() const { return status_->text(); }

void WarcraftTextPreview::ChooseColour() {
    if (!CanInsert()) return;
    const QPointer<WarcraftTextPreview> guard(this);
    const auto generation = generation_;
    const QColor selected = QColorDialog::getColor(Qt::white, this, tr("插入文字颜色"), QColorDialog::DontUseNativeDialog);
    if (guard && guard->generation_ == generation && selected.isValid()) (void)guard->InsertColour(selected);
}

} // namespace War3Map::UiKit
