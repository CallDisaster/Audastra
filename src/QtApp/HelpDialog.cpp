#include "HelpDialog.h"
#include "Icons/SvgIcons.h"
#include "Widgets/SearchField.h"

#include <QFile>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
constexpr auto kHelpResource = ":/audastra/help/help.zh-CN.md";
constexpr int kSectionRole = Qt::UserRole + 1;

QString SectionTitle(const QString& section) {
    const auto line = section.section(QLatin1Char('\n'), 0, 0);
    return line.startsWith(QStringLiteral("## ")) ? line.mid(3).trimmed() : line.trimmed();
}
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
    page_->document()->setDefaultStyleSheet(QStringLiteral(
        "h2 { color: #e8eaed; margin-top: 4px; margin-bottom: 10px; }"
        "h3 { color: #e8eaed; margin-top: 14px; margin-bottom: 6px; }"
        "p, li { color: #d5d8de; line-height: 155%; }"
        "code { background-color: #22252b; color: #8ae0b3; }"));
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
}
