#include "HelpDialog.h"
#include "Icons/SvgIcons.h"
#include "Theme/ThemeManager.h"
#include "Widgets/SearchField.h"

#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <utility>
#include <vector>

namespace {
constexpr auto kHelpResource = ":/audastra/help/help.zh-CN.md";
constexpr int kSectionRole = Qt::UserRole + 1;

QString SectionTitle(const QString& section) {
    const auto line = section.section(QLatin1Char('\n'), 0, 0);
    return line.startsWith(QStringLiteral("## ")) ? line.mid(3).trimmed() : line.trimmed();
}

// 帮助正文按当前主题令牌着色。setMarkdown 走 Markdown 导入而不是 HTML 导入，
// 文档默认样式表对它不起作用，所以载入后直接改字符与块格式：
// 标题 @text、正文 @textBody、行内代码 @inlineCodeAccent（不加底色）、代码块 @text 配 @raised 底、链接 @accent。
void ApplyThemeColors(QTextDocument& document, const War3Map::UiKit::Tokens::ThemeTokens& theme) {
    using War3Map::UiKit::ToQColor;
    const auto& c = theme.colors;
    struct Span { int position; int length; QColor color; };
    std::vector<Span> spans;
    std::vector<int> codeBlocks;
    // 先收集再修改：合并格式会拆分或合并片段，不能边遍历边改。
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat blockFormat = block.blockFormat();
        const bool codeBlock = blockFormat.hasProperty(QTextFormat::BlockCodeFence)
            || blockFormat.hasProperty(QTextFormat::BlockCodeLanguage);
        const bool heading = blockFormat.headingLevel() > 0;
        if (codeBlock) codeBlocks.push_back(block.position());
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            const QColor color = format.isAnchor() ? ToQColor(c.accent)
                : codeBlock ? ToQColor(c.text)
                : format.fontFixedPitch() ? ToQColor(c.inlineCodeAccent)
                : heading ? ToQColor(c.text) : ToQColor(c.textBody);
            spans.push_back({fragment.position(), fragment.length(), color});
        }
    }
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    for (const auto& span : spans) {
        QTextCharFormat format;
        format.setForeground(span.color);
        cursor.setPosition(span.position);
        cursor.setPosition(span.position + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    for (const int position : codeBlocks) {
        QTextBlockFormat format;
        format.setBackground(ToQColor(c.raised));
        cursor.setPosition(position);
        cursor.mergeBlockFormat(format);
    }
    cursor.endEditBlock();
}

// 调色板变化（切换主题）后合并成一次回调，在事件循环里重新着色，不在样式抛光过程中改文档。
class PaletteWatcher final : public QObject {
public:
    PaletteWatcher(QWidget& target, std::function<void()> changed)
        : QObject(&target), changed_(std::move(changed)) {
        target.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::PaletteChange && !pending_) {
            pending_ = true;
            QTimer::singleShot(0, this, [this] {
                pending_ = false;
                changed_();
            });
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> changed_;
    bool pending_ = false;
};
}

QStringList HelpDialog::SplitSections(const QString& markdown) {
    QStringList sections;
    QString current;
    const auto lines = QString(markdown).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        // 一级标题是文档名，不单独成节。
        if (line.startsWith(QStringLiteral("# "))) continue;
        if (line.startsWith(QStringLiteral("## ")) && !current.trimmed().isEmpty()) {
            sections << current.trimmed();
            current.clear();
        }
        current += line + QLatin1Char('\n');
    }
    if (!current.trimmed().isEmpty()) sections << current.trimmed();
    return sections;
}

HelpDialog::HelpDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("helpDialog"));
    setWindowTitle(tr("使用帮助 · Audastra"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(980, 680);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto* splitter = new QSplitter(this);
    splitter->setChildrenCollapsible(false);
    auto* side = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 6, 0);
    sideLayout->setSpacing(8);
    search_ = new QLineEdit(side);
    search_->setObjectName(QStringLiteral("helpSearch"));
    search_->setPlaceholderText(tr("搜索帮助章节或正文…"));
    search_->setClearButtonEnabled(true);
    War3Map::UiKit::InstallSearchIcon(search_);
    sideLayout->addWidget(search_);
    topics_ = new QListWidget(side);
    topics_->setObjectName(QStringLiteral("helpTopics"));
    topics_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    topics_->setTextElideMode(Qt::ElideRight);
    sideLayout->addWidget(topics_, 1);
    page_ = new QTextBrowser(splitter);
    page_->setObjectName(QStringLiteral("helpPage"));
    page_->setOpenExternalLinks(true);
    page_->document()->setDocumentMargin(14);
    // 切换主题后按新令牌重新着色当前节，保留滚动位置。
    new PaletteWatcher(*page_, [this] {
        if (sections_.isEmpty()) return;
        const int scroll = page_->verticalScrollBar()->value();
        const auto* item = topics_->currentItem();
        ShowSection(item ? item->data(kSectionRole).toInt() : -1);
        page_->verticalScrollBar()->setValue(scroll);
    });
    splitter->addWidget(side);
    splitter->addWidget(page_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({236, 720});
    layout->addWidget(splitter);

    QFile file(QString::fromLatin1(kHelpResource));
    if (file.open(QIODevice::ReadOnly)) sections_ = SplitSections(QString::fromUtf8(file.readAll()));
    for (int index = 0; index < sections_.size(); ++index) {
        auto* item = new QListWidgetItem(SectionTitle(sections_[index]), topics_);
        item->setData(kSectionRole, index);
    }
    connect(search_, &QLineEdit::textChanged, this, &HelpDialog::Filter);
    connect(topics_, &QListWidget::currentRowChanged, this, [this](int) {
        const auto* item = topics_->currentItem();
        ShowSection(item ? item->data(kSectionRole).toInt() : -1);
    });
    if (topics_->count() > 0) topics_->setCurrentRow(0);
    else page_->setPlainText(tr("帮助文档不可用"));
}

void HelpDialog::ShowTopic(const QString& topic) {
    for (int row = 0; row < topics_->count(); ++row) {
        if (topics_->item(row)->text().contains(topic, Qt::CaseInsensitive)) {
            topics_->setCurrentRow(row);
            return;
        }
    }
}

void HelpDialog::Filter(const QString& text) {
    int first = -1;
    for (int row = 0; row < topics_->count(); ++row) {
        auto* item = topics_->item(row);
        const bool match = text.isEmpty()
            || sections_.value(item->data(kSectionRole).toInt()).contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
        if (match && first < 0) first = row;
    }
    if (const auto* current = topics_->currentItem(); (!current || current->isHidden()) && first >= 0)
        topics_->setCurrentRow(first);
}

void HelpDialog::ShowSection(int row) {
    if (row < 0 || row >= sections_.size()) { page_->clear(); return; }
    page_->setMarkdown(sections_[row]);
    ApplyThemeColors(*page_->document(), War3Map::UiKit::ThemeManager::ThemeForPalette(page_->palette()));
}
