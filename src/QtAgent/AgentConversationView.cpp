#include "AgentConversationView.h"
#include "AgentLayoutDiagnostics.h"

#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Markdown/MarkdownRenderer.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QHash>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <memory>
#include <functional>

namespace War3Map::Agent {
namespace {

constexpr int kPayloadRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kDetailsLimit = 128 * 1024;
constexpr int kPreviewLimit = 20000;          // 与 DSH JsonBlock 相同的预览上限
constexpr int kMarkdownLimit = 200 * 1024;    // 超长正文退回纯文本，避免每次刷新整篇重排

// MCP 工具结果常把 JSON 再序列化成字符串（content[].text），直接显示会变成转义串、树里只剩“{”。
// 展示前把能完整解析的 JSON 字符串展开成结构，MCP 文本内容数组折叠成内容本身；原事件不改。
QJsonValue Unwrap(const QJsonValue& value, int depth = 0) {
    if (depth > 8) return value;
    if (value.isString()) {
        const QString text = value.toString();
        const QString trimmed = text.trimmed();
        if (trimmed.size() >= 2 && trimmed.size() <= 4 * 1024 * 1024
            && ((trimmed.startsWith(QLatin1Char('{')) && trimmed.endsWith(QLatin1Char('}')))
                || (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))))) {
            QJsonParseError error;
            const auto parsed = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError) {
                return Unwrap(parsed.isObject() ? QJsonValue(parsed.object()) : QJsonValue(parsed.array()), depth + 1);
            }
        }
        return value;
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const auto& item : value.toArray()) array.append(Unwrap(item, depth + 1));
        return array;
    }
    if (!value.isObject()) return value;
    QJsonObject object = value.toObject();
    const auto content = object.value(QStringLiteral("content"));
    if (content.isArray()) {
        // MCP CallToolResult：{content:[{type:"text",text:"…"}], isError?, structuredContent?}
        bool textOnly = !content.toArray().isEmpty();
        for (const auto& item : content.toArray()) {
            const auto entry = item.toObject();
            textOnly = textOnly && entry.value(QStringLiteral("type")).toString() == QStringLiteral("text") && entry.value(QStringLiteral("text")).isString();
        }
        if (textOnly) {
            QJsonArray texts;
            for (const auto& item : content.toArray()) texts.append(Unwrap(item.toObject().value(QStringLiteral("text")), depth + 1));
            const QJsonValue unwrapped = texts.size() == 1 ? texts.at(0) : QJsonValue(texts);
            QStringList keys = object.keys();
            keys.removeAll(QStringLiteral("content"));
            if (object.value(QStringLiteral("isError")).isBool() && !object.value(QStringLiteral("isError")).toBool()) keys.removeAll(QStringLiteral("isError"));
            if (keys.isEmpty()) return unwrapped;
            object.insert(QStringLiteral("content"), unwrapped);
        }
    }
    for (auto it = object.begin(); it != object.end(); ++it) it.value() = Unwrap(it.value(), depth + 1);
    return object;
}

QString DetailText(const QJsonValue& value) {
    QString result;
    if (value.isObject()) result = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
    else if (value.isArray()) result = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    else if (value.isString()) result = value.toString();
    else if (value.isBool()) result = value.toBool() ? QStringLiteral("真") : QStringLiteral("假");
    else if (value.isDouble()) result = QString::number(value.toDouble(), 'g', 16);
    else if (value.isNull()) result = QStringLiteral("null");
    else result = QStringLiteral("未提供");
    if (result.size() > kDetailsLimit) result = result.left(kDetailsLimit) + QStringLiteral("\n… 详情达到显示预算");
    return result;
}

void AddFields(QTreeWidgetItem* parent, const QJsonValue& value, int depth, int& nodes) {
    if (depth >= 5 || nodes >= 160) return;
    const auto add = [parent, depth, &nodes](const QString& key, const QJsonValue& childValue) {
        if (nodes >= 160) return;
        ++nodes;
        auto* child = new QTreeWidgetItem(parent);
        child->setText(0, key);
        child->setData(0, kPathRole, parent->data(0, kPathRole).toString()
            + QStringLiteral("/%1:%2").arg(key.size()).arg(key));
        const QString text = DetailText(childValue);
        child->setText(1, childValue.isObject() ? QStringLiteral("%1 个字段").arg(childValue.toObject().size())
            : childValue.isArray() ? QStringLiteral("%1 项").arg(childValue.toArray().size())
            : text.simplified().left(160));
        child->setToolTip(0, key);
        child->setToolTip(1, text.left(2048));
        child->setData(0, kPayloadRole, QVariant::fromValue(childValue));
        AddFields(child, childValue, depth + 1, nodes);
    };
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) add(it.key(), it.value());
    } else if (value.isArray()) {
        const auto array = value.toArray();
        for (qsizetype i = 0; i < array.size() && nodes < 160; ++i) add(QStringLiteral("[%1]").arg(i), array.at(i));
    }
}

class MessageText final : public QTextBrowser {
public:
    MessageText(bool markdown, QWidget* parent) : QTextBrowser(parent), markdown_(markdown) {
        setObjectName(QStringLiteral("agentMessageBody"));
        setFrameShape(QFrame::NoFrame);
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // 用户气泡按文字实际宽度收缩（上限由所在行限制）；助手正文占满列宽。
        setSizePolicy(markdown ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Fixed);
        document()->setDocumentMargin(0);
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDefaultTextOption(option);
        QFont bodyFont = font();
        bodyFont.setPixelSize(14);
        setFont(bodyFont);
        QObject::connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this] { UpdateHeight(); });
        // 只打开网页和邮件链接；本地路径、自定义协议不由对话正文触发。
        QObject::connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
            const QString scheme = url.scheme().toLower();
            if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https") || scheme == QStringLiteral("mailto")) {
                QDesktopServices::openUrl(url);
            }
        });
        // 流式输出合并刷新：整篇重新解析，避免半截表格/代码块残留错误格式。
        renderTimer_.setSingleShot(true);
        renderTimer_.setInterval(90);
        QObject::connect(&renderTimer_, &QTimer::timeout, this, [this] { Render(false); });
    }

    void SetText(const QString& text) {
        source_ = text;
        settled_ = false;
        if (UsesMarkdown()) { renderTimer_.stop(); Render(true); return; }
        // 不解释模型正文中的 HTML，不加载远程图片或本地文件。
        setPlainText(text);
        QTextCursor cursor(document());
        cursor.select(QTextCursor::Document);
        SetLeading(cursor);
        UpdateHeight();
        updateGeometry();
    }

    void Append(const QString& text) {
        const bool wasMarkdown = UsesMarkdown();
        source_ += text;
        settled_ = false;
        if (UsesMarkdown()) {
            if (!renderTimer_.isActive()) renderTimer_.start();
            return;
        }
        if (wasMarkdown) { SetText(source_); return; }
        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        const int start = cursor.position();
        cursor.insertText(text);
        cursor.setPosition(start, QTextCursor::KeepAnchor);
        SetLeading(cursor);
        UpdateHeight();
        updateGeometry();
    }

    // 消息结束：按完整语法（含公式）重排一次。流式阶段与 DSH 一致不解析公式，避免半截 $ 闪烁。
    void Settle() {
        if (!UsesMarkdown() || settled_) return;
        renderTimer_.stop();
        Render(true);
    }

    [[nodiscard]] const QString& Source() const { return source_; }

    // 纯文本（用户消息）的建议宽度是不换行时的文字宽度，气泡不再比文字宽出一截。
    [[nodiscard]] QSize sizeHint() const override {
        if (UsesMarkdown()) return QTextBrowser::sizeHint();
        const std::unique_ptr<QTextDocument> measure(document()->clone());
        measure->setTextWidth(-1);
        return {static_cast<int>(std::ceil(measure->idealWidth())) + 2, height()};
    }
    [[nodiscard]] QSize minimumSizeHint() const override {
        return UsesMarkdown() ? QTextBrowser::minimumSizeHint() : QSize(20, height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        UpdateHeight();
    }

    void changeEvent(QEvent* event) override {
        QTextBrowser::changeEvent(event);
        if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && UsesMarkdown() && !source_.isEmpty()) {
            QTimer::singleShot(0, this, [this] { Render(settled_); });
        }
    }

private:
    static void SetLeading(QTextCursor& cursor) {
        QTextBlockFormat format;
        format.setLineHeight(24, QTextBlockFormat::FixedHeight);
        format.setTopMargin(0);
        format.setBottomMargin(0);
        cursor.mergeBlockFormat(format);
    }

    [[nodiscard]] bool UsesMarkdown() const { return markdown_ && source_.size() <= kMarkdownLimit; }

    void Render(bool math) {
        UiKit::MarkdownTheme theme;
        const QPalette colors = palette();
        const auto* tokens = &UiKit::ThemeManager::Theme(colors.color(QPalette::Window).lightness() > 128
            ? QStringLiteral("paper") : QStringLiteral("graphite"));
        for (const auto& candidate : UiKit::Tokens::kThemes)
            if (UiKit::ToQColor(candidate.colors.bg) == colors.color(QPalette::Window)) { tokens = &candidate; break; }
        theme.text = UiKit::ToQColor(tokens->colors.textBody);
        theme.headingText = UiKit::ToQColor(tokens->colors.text);
        theme.inlineCodeAccent = UiKit::ToQColor(tokens->colors.inlineCodeAccent);
        // 颜色全部取主题令牌：独立浮动窗口里的控件调色板可能还是系统默认（浅色），
        // 按调色板取 AlternateBase 会让代码块和表头变成白底、浅色字看不见。
        const auto& c = tokens->colors;
        theme.muted = UiKit::ToQColor(c.muted);
        theme.link = UiKit::ToQColor(c.accent);
        theme.codeText = UiKit::ToQColor(c.text);
        theme.codeBackground = UiKit::ToQColor(c.panel);
        theme.inlineCodeBackground = UiKit::ToQColor(c.raised);
        const QColor line = UiKit::ToQColor(c.line), ink = UiKit::ToQColor(c.text);
        theme.border = QColor::fromRgbF(line.redF() * 0.7f + ink.redF() * 0.3f, line.greenF() * 0.7f + ink.greenF() * 0.3f,
            line.blueF() * 0.7f + ink.blueF() * 0.3f);
        theme.tableHeader = UiKit::ToQColor(c.panel);
        theme.quoteBar = line;
        theme.dark = tokens->dark;
        theme.textFamily = font().family();
        theme.codeFamily = UiKit::MonoFont(13).family();
        theme.pixelSize = 15;
        theme.devicePixelRatio = devicePixelRatioF();
        UiKit::RenderMarkdown(*document(), source_, theme, math);
        settled_ = math;
        UpdateHeight();
    }

    void UpdateHeight() {
        if (adjusting_) return;
        adjusting_ = true;
        document()->setTextWidth(std::max(20, viewport()->width()));
        const int height = std::max(24, static_cast<int>(std::ceil(document()->size().height())) + 2);
        if (height != this->height()) setFixedHeight(height);
        adjusting_ = false;
    }

    bool adjusting_ = false;
    bool markdown_ = false;
    bool settled_ = false;
    QString source_;
    QTimer renderTimer_;
};

class MessageRow final : public QWidget {
public:
    MessageRow(const QString& speaker, bool user, QWidget* parent) : QWidget(parent), user_(user) {
        setObjectName(user ? QStringLiteral("agentUserTurn") : QStringLiteral("agentAssistantTurn"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, user ? 16 : 10, 0, user ? 16 : 12);
        row->setSpacing(0);
        if (user) row->addStretch();
        auto* surface = new QFrame(this);
        surface->setObjectName(user ? QStringLiteral("agentUserBubble") : QStringLiteral("agentAssistantBody"));
        surface_ = surface;
        auto* column = new QVBoxLayout(surface);
        column->setContentsMargins(user ? 16 : 0, user ? 10 : 0, user ? 16 : 0, user ? 10 : 0);
        column->setSpacing(4);
        if (speaker.contains(QStringLiteral("历史")) || speaker == QStringLiteral("错误")) {
            auto* label = new QLabel(speaker, surface);
            label->setObjectName(QStringLiteral("agentMessageAnnotation"));
            column->addWidget(label);
        }
        body = new MessageText(!user, surface);
        column->addWidget(body);
        row->addWidget(surface, user ? 0 : 1);
    }

    MessageText* body = nullptr;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (user_) surface_->setMaximumWidth(std::max(150, static_cast<int>(width() * 0.78)));
    }

private:
    QWidget* surface_ = nullptr;
    bool user_ = false;
};

class DisclosureRow final : public QWidget {
public:
    explicit DisclosureRow(bool change, QWidget* parent) : QWidget(parent), change_(change) {
        setObjectName(change ? QStringLiteral("agentInlineChanges") : QStringLiteral("agentToolRow"));
        // 行高只由内容决定：滚动区画布比内容高时，多余空间不能分进行里，否则回复和下一条提问之间出现空白。
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 2, 0, 4);
        layout->setSpacing(4);
        auto* line = new QWidget(this);
        auto* lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->setSpacing(6);
        toggle_ = new QToolButton(line);
        toggle_->setObjectName(change ? QStringLiteral("agentChangeToggle") : QStringLiteral("agentToolToggle"));
        toggle_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/chevron-right")));
        toggle_->setFixedSize(22, 24);
        toggle_->setCheckable(true);
        toggle_->setAccessibleName(change ? QStringLiteral("展开修改前后值") : QStringLiteral("展开工具参数与结果"));
        lineLayout->addWidget(toggle_);
        title_ = new UiKit::ElidedLabel(line);
        title_->setObjectName(QStringLiteral("agentToolTitle"));
        title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lineLayout->addWidget(title_, 1);
        state_ = new QLabel(line);
        state_->setObjectName(QStringLiteral("agentToolState"));
        lineLayout->addWidget(state_);
        duration_ = new QLabel(line);
        duration_->setObjectName(QStringLiteral("agentToolDuration"));
        duration_->setFont(UiKit::MonoFont(12));
        lineLayout->addWidget(duration_);
        jump_ = new QPushButton(QStringLiteral("转到"), line);
        jump_->setObjectName(QStringLiteral("agentInlineNavigate"));
        jump_->hide();
        lineLayout->addWidget(jump_);
        layout->addWidget(line);
        panel_ = new QWidget(this);
        panel_->setObjectName(QStringLiteral("agentDisclosureBody"));
        auto* panelLayout = new QVBoxLayout(panel_);
        panelLayout->setContentsMargins(28, 4, 0, 4);
        panelLayout->setSpacing(4);
        tree_ = new QTreeWidget(panel_);
        tree_->setObjectName(change ? QStringLiteral("agentChangeDetails") : QStringLiteral("agentToolDetails"));
        tree_->setColumnCount(2);
        tree_->setHeaderHidden(true);
        tree_->setUniformRowHeights(true);
        tree_->setRootIsDecorated(true);
        tree_->setIndentation(16);
        // 字段名列按宽度比例定宽，值列占剩余宽度：长值在值列内省略，悬停或选中看完整内容。
        tree_->header()->setStretchLastSection(true);
        tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        tree_->header()->resizeSection(0, 150);
        tree_->setTextElideMode(Qt::ElideRight);
        tree_->setFixedHeight(132);
        panelLayout->addWidget(tree_);
        preview_ = new QPlainTextEdit(panel_);
        preview_->setObjectName(QStringLiteral("agentInlineDetailPreview"));
        preview_->setReadOnly(true);
        preview_->setFont(UiKit::MonoFont(12));
        preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
        preview_->setFixedHeight(180);
        preview_->hide();
        panelLayout->addWidget(preview_);
        layout->addWidget(panel_);
        panel_->hide();
        QObject::connect(toggle_, &QToolButton::toggled, this, [this](bool open) {
            toggle_->setIcon(UiKit::SvgIcon(open ? QStringLiteral("lucide/chevron-down") : QStringLiteral("lucide/chevron-right")));
            panel_->setVisible(open);
            if (open && !reduced_) UiKit::FadeIn(panel_, 120, 0.6);
        });
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* selected, QTreeWidgetItem*) {
            if (!rebuilding_) userSelected_ = selected != nullptr;
            preview_->setVisible(selected != nullptr);
            if (selected == nullptr) return;
            QString text = DetailText(selected->data(0, kPayloadRole).value<QJsonValue>());
            if (text.size() > kPreviewLimit) text = text.left(kPreviewLimit) + QStringLiteral("\n… 仅显示前 %1 个字符").arg(kPreviewLimit);
            preview_->setPlainText(text);
            // 预览高度跟随内容，最多 14 行后滚动
            const int lines = std::clamp(static_cast<int>(text.count(QLatin1Char('\n'))) + 1, 1, 14);
            preview_->setFixedHeight(lines * preview_->fontMetrics().lineSpacing() + 2 * preview_->frameWidth() + 24);
        });
        QObject::connect(tree_, &QTreeWidget::itemExpanded, this, [this] { FitTree(); });
        QObject::connect(tree_, &QTreeWidget::itemCollapsed, this, [this] { FitTree(); });
        QObject::connect(jump_, &QPushButton::clicked, this, [this] {
            if (navigate && !kind_.isEmpty() && !objectId_.isEmpty()) navigate(kind_, objectId_);
        });
    }

    void Update(const QJsonObject& event, const QString& state, const QString& duration) {
        event_ = event;
        QString title = event.value(QStringLiteral("title")).toString();
        const QString originalTitle = title;
        const auto toolName = event.value(QStringLiteral("tool_name")).toString();
        if (!change_ && !toolName.isEmpty()) {
            // MCP description 是完整使用说明；工具卡只显示简短操作名，原说明在展开区保留。
            static const QHash<QString, QString> labels{
                {QStringLiteral("editor_state"), QStringLiteral("读取编辑器状态")},
                {QStringLiteral("objects_list"), QStringLiteral("查找物编对象")},
                {QStringLiteral("objects_get"), QStringLiteral("读取对象字段")},
                {QStringLiteral("objects_set_fields"), QStringLiteral("修改对象字段")},
                {QStringLiteral("objects_create"), QStringLiteral("创建物编对象")},
                {QStringLiteral("objects_delete"), QStringLiteral("删除物编对象")},
                {QStringLiteral("objects_field_options"), QStringLiteral("查询字段选项")},
                {QStringLiteral("variables_list"), QStringLiteral("浏览变量")},
                {QStringLiteral("variables_get"), QStringLiteral("读取变量")},
                {QStringLiteral("variables_create"), QStringLiteral("创建变量")},
                {QStringLiteral("variables_update"), QStringLiteral("修改变量")},
                {QStringLiteral("variables_rename"), QStringLiteral("重命名变量")},
                {QStringLiteral("variables_delete"), QStringLiteral("删除变量")},
                {QStringLiteral("entities_query"), QStringLiteral("查询地图实体")},
                {QStringLiteral("entities_set_fields"), QStringLiteral("修改实体字段")},
                {QStringLiteral("entities_rotate"), QStringLiteral("调整实体朝向")},
                {QStringLiteral("entities_copy"), QStringLiteral("复制预放实体")},
                {QStringLiteral("sounds_list"), QStringLiteral("查询声音定义")},
                {QStringLiteral("sounds_get"), QStringLiteral("读取声音定义")},
                {QStringLiteral("sounds_update"), QStringLiteral("修改声音参数")},
                {QStringLiteral("imports_list"), QStringLiteral("查询导入资源")},
                {QStringLiteral("imports_get"), QStringLiteral("读取导入信息")},
                {QStringLiteral("editor_query_trigger_graph"), QStringLiteral("查询触发器结构")},
                {QStringLiteral("editor_edit_trigger_graph"), QStringLiteral("编辑触发器")},
                {QStringLiteral("editor_get_ui_catalog"), QStringLiteral("查询触发器函数库")},
                {QStringLiteral("editor_save_map"), QStringLiteral("保存地图")},
                {QStringLiteral("editor_true_save"), QStringLiteral("原版编辑器保存")}
            };
            title = labels.value(toolName, title.size() > 40 || title.contains(QLatin1Char('\n')) || title.isEmpty() ? toolName : title);
        }
        if (title.isEmpty()) title = toolName;
        if (title.isEmpty()) title = event.value(QStringLiteral("target")).toString();
        // 改动行压缩完整对象名；普通工具标题继续由 ElidedLabel 按实际宽度省略。
        // 仅收缩改动行展示文字；event_、悬停提示和展开详情保留原始名称与值。
        if (change_) {
            title = title.simplified();
            if (title.size() > 40) {
                title = title.left(39);
                if (title.at(title.size() - 1).isHighSurrogate()) title.chop(1);
                title += QChar(0x2026);
            }
        }
        title_->setText(title.isEmpty() ? change_ ? QStringLiteral("修改前后值") : QStringLiteral("工具调用") : title);
        title_->setToolTip(title_->text() + QLatin1Char('\n') + originalTitle
            + QLatin1Char('\n') + event.value(QStringLiteral("target")).toString());
        state_->setText(state);
        duration_->setText(duration);
        kind_ = event.value(QStringLiteral("kind")).toString();
        objectId_ = event.value(QStringLiteral("object_id")).toString();
        jump_->setVisible(!kind_.isEmpty() && !objectId_.isEmpty());
        // 持续到达的结果更新保留用户展开的字段和正在阅读的参数。
        QSet<QString> expanded;
        QString selected;
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
            QTreeWidgetItem* item = *it;
            const QString path = item->data(0, kPathRole).toString();
            if (item->isExpanded()) expanded.insert(path);
            if (item == tree_->currentItem()) selected = path;
        }
        rebuilding_ = true;
        tree_->clear();
        int nodes = 0;
        const QStringList fields{QStringLiteral("arguments"), QStringLiteral("details"), QStringLiteral("result"),
            QStringLiteral("text"), QStringLiteral("before"), QStringLiteral("after")};
        for (const auto& key : fields) {
            if (!event.contains(key)) continue;
            auto* root = new QTreeWidgetItem(tree_);
            root->setData(0, kPathRole, key);
            root->setText(0, key == QStringLiteral("arguments") ? QStringLiteral("参数")
                : key == QStringLiteral("result") ? QStringLiteral("结果")
                : key == QStringLiteral("before") ? QStringLiteral("修改前")
                : key == QStringLiteral("after") ? QStringLiteral("修改后")
                : key == QStringLiteral("text") ? QStringLiteral("输出") : QStringLiteral("详情"));
            const QJsonValue value = Unwrap(event.value(key));
            root->setData(0, kPayloadRole, QVariant::fromValue(value));
            root->setText(1, Summary(value));
            root->setToolTip(1, DetailText(value).left(2048));
            AddFields(root, value, 0, nodes);
            // 新出现的分组（如工具结束后才有的“结果”）默认展开一层，直接看到字段而不是“{”。
            if (!knownRoots_.contains(key)) { knownRoots_.insert(key); expanded.insert(key); }
        }
        if (!originalTitle.isEmpty() && originalTitle != title) {
            auto* description = new QTreeWidgetItem(tree_);
            description->setData(0, kPathRole, change_ ? QStringLiteral("object_name") : QStringLiteral("tool_description"));
            description->setData(0, kPayloadRole, QVariant::fromValue(QJsonValue(originalTitle)));
            description->setText(0, change_ ? QStringLiteral("对象") : QStringLiteral("说明"));
            description->setText(1, originalTitle);
            description->setToolTip(1, originalTitle);
        }
        QTreeWidgetItem* fallback = nullptr;
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
            QTreeWidgetItem* item = *it;
            const QString path = item->data(0, kPathRole).toString();
            item->setExpanded(expanded.contains(path));
            if (userSelected_ && !selected.isEmpty() && path == selected) tree_->setCurrentItem(item);
            if (item->parent() == nullptr && (fallback == nullptr || path == QStringLiteral("result"))) fallback = item;
        }
        // 未手动选择时预览默认显示结果（没有结果时显示首个分组），格式化后的完整 JSON 直接可读。
        if (!userSelected_ && fallback != nullptr) tree_->setCurrentItem(fallback);
        rebuilding_ = false;
        FitTree();
    }

    void SetReduced(bool reduced) {
        reduced_ = reduced;
        tree_->setAnimated(!reduced && UiKit::AnimationsEnabled());
    }

    [[nodiscard]] QString PlainText() const {
        return title_->text() + QLatin1Char('\n') + state_->text() + QLatin1Char('\n') + DetailText(event_);
    }

    std::function<void(QString, QString)> navigate;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        tree_->header()->resizeSection(0, std::clamp(tree_->width() / 3, 110, 200));
        duration_->setVisible(width() >= 500);
        setToolTip(duration_->text());
    }

private:
    QToolButton* toggle_ = nullptr;
    UiKit::ElidedLabel* title_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* duration_ = nullptr;
    QPushButton* jump_ = nullptr;
    QWidget* panel_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    // 树高度跟随可见行数，最多 12 行后滚动。
    void FitTree() {
        int rows = 0;
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr && rows <= 12; ++it) {
            bool visible = true;
            for (auto* parent = (*it)->parent(); parent != nullptr; parent = parent->parent()) visible = visible && parent->isExpanded();
            if (visible) ++rows;
        }
        // 样式表给行加了上下内边距，sizeHintForRow 不含这部分，取两者较大值
        const int rowHeight = std::max({20, tree_->sizeHintForRow(0), tree_->fontMetrics().height() + 10});
        tree_->setFixedHeight(std::clamp(rows, 1, 12) * rowHeight + 2 * tree_->frameWidth() + 12);
    }

    static QString Summary(const QJsonValue& value) {
        if (value.isObject()) return QStringLiteral("%1 个字段").arg(value.toObject().size());
        if (value.isArray()) return QStringLiteral("%1 项").arg(value.toArray().size());
        return DetailText(value).simplified().left(160);
    }

    QJsonObject event_;
    QSet<QString> knownRoots_;
    bool userSelected_ = false;
    bool rebuilding_ = false;
    QString kind_;
    QString objectId_;
    bool change_ = false;
    bool reduced_ = false;
};

} // namespace

struct AgentConversationView::Private {
    explicit Private(AgentConversationView* owner) : q(owner) {}
    AgentConversationView* q;
    QWidget* canvas = nullptr;
    QWidget* column = nullptr;
    QVBoxLayout* flow = nullptr;
    QWidget* empty = nullptr;
    MessageRow* active = nullptr;
    QList<MessageRow*> messages;
    QList<DisclosureRow*> changes;
    QHash<QString, DisclosureRow*> tools;
    bool follow = true;
    bool scrollInternal = false;
    bool reduced = false;
    int contentWidth = 760;
    QTimer diagnosticTimer;
    QElapsedTimer diagnosticClock;
    AgentLayoutDiagnostics diagnostics;

    void Add(QWidget* widget) {
        empty->hide();
        flow->addWidget(widget);
        widget->show();
        q->widget()->layout()->activate();
    }
};

AgentConversationView::AgentConversationView(QWidget* parent) : QScrollArea(parent), d_(std::make_unique<Private>(this)) {
    setObjectName(QStringLiteral("agentConversation"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    d_->canvas = new QWidget;
    d_->canvas->setObjectName(QStringLiteral("agentConversationCanvas"));
    auto* canvasLayout = new QVBoxLayout(d_->canvas);
    canvasLayout->setSizeConstraint(QLayout::SetNoConstraint);
    canvasLayout->setContentsMargins(16, 18, 16, 24);
    auto* axis = new QHBoxLayout;
    axis->setContentsMargins(0, 0, 0, 0);
    d_->column = new QWidget(d_->canvas);
    d_->column->setObjectName(QStringLiteral("agentReadingColumn"));
    d_->column->setMinimumWidth(0);
    d_->column->setMaximumWidth(760);
    d_->column->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    d_->flow = new QVBoxLayout(d_->column);
    d_->flow->setSizeConstraint(QLayout::SetNoConstraint);
    d_->flow->setContentsMargins(0, 0, 0, 0);
    d_->flow->setSpacing(6);
    d_->flow->setAlignment(Qt::AlignTop);
    axis->addStretch();
    axis->addWidget(d_->column, 1);
    axis->addStretch();
    canvasLayout->addLayout(axis);
    canvasLayout->addStretch(1);
    d_->empty = new QWidget(d_->column);
    d_->empty->setObjectName(QStringLiteral("agentEmptyState"));
    auto* emptyLayout = new QVBoxLayout(d_->empty);
    emptyLayout->setContentsMargins(0, 96, 0, 48);
    emptyLayout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("想对地图做些什么？"), d_->empty);
    title->setObjectName(QStringLiteral("agentEmptyTitle"));
    title->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(title);
    // 常用任务：点一下填入输入框，不直接发送。
    emptyLayout->addSpacing(14);
    auto* suggestions = new QHBoxLayout;
    suggestions->setSpacing(8);
    suggestions->addStretch();
    for (const auto& text : {QStringLiteral("检查触发器里的错误"), QStringLiteral("列出使用某个单位的触发器"),
             QStringLiteral("优化高频单位事件")}) {
        auto* chip = new QPushButton(text, d_->empty);
        chip->setObjectName(QStringLiteral("agentSuggestion"));
        chip->setCursor(Qt::PointingHandCursor);
        QObject::connect(chip, &QPushButton::clicked, this, [this, text] { emit SuggestionChosen(text); });
        suggestions->addWidget(chip);
    }
    suggestions->addStretch();
    emptyLayout->addLayout(suggestions);
    d_->flow->addWidget(d_->empty);
    setWidget(d_->canvas);
    d_->diagnosticClock.start();
    d_->diagnosticTimer.setSingleShot(true);
    d_->diagnosticTimer.setTimerType(Qt::PreciseTimer);
    d_->diagnosticTimer.setInterval(250);
    connect(&d_->diagnosticTimer, &QTimer::timeout, this, &AgentConversationView::InspectLayoutDiagnostics);
    d_->column->installEventFilter(this);
    d_->canvas->installEventFilter(this);
    QObject::connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        // 隐藏或放大后暂时不需要滚动，自动 clamp 到 0 不是用户回到底部。
        if (!d_->scrollInternal && verticalScrollBar()->maximum() > 0) {
            d_->follow = value >= verticalScrollBar()->maximum() - 12;
        }
    });
    QObject::connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
        if (!d_->follow) return;
        d_->scrollInternal = true;
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        d_->scrollInternal = false;
    });
}

AgentConversationView::~AgentConversationView() = default;

void AgentConversationView::BeginMessage(const QString& speaker, bool user) {
    d_->active = new MessageRow(speaker, user, d_->column);
    d_->messages.append(d_->active);
    connect(d_->active->body->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
        this, [this] { ScheduleLayoutDiagnostics(); });
    d_->Add(d_->active);
    emit ContentsChanged();
}

void AgentConversationView::AppendText(const QString& text) {
    if (text.isEmpty()) return;
    if (d_->active == nullptr) BeginMessage(QStringLiteral("助手"));
    d_->active->body->Append(text);
    emit ContentsChanged();
}

void AgentConversationView::ReplaceText(const QString& text) {
    if (d_->active == nullptr) BeginMessage(QStringLiteral("助手"));
    d_->active->body->SetText(text);
    emit ContentsChanged();
}

void AgentConversationView::EndMessage() {
    if (d_->active != nullptr) d_->active->body->Settle();
    d_->active = nullptr;
}

void AgentConversationView::UpdateTool(const QJsonObject& event, const QString& state, const QString& duration) {
    const QString id = event.value(QStringLiteral("id")).toString();
    auto found = d_->tools.find(id);
    if (found == d_->tools.end()) {
        auto* row = new DisclosureRow(false, d_->column);
        row->navigate = [this](QString kind, QString objectId) { emit NavigateRequested(kind, objectId); };
        row->SetReduced(d_->reduced);
        d_->Add(row);
        found = d_->tools.insert(id, row);
    }
    found.value()->Update(event, state, duration);
    emit ContentsChanged();
}

void AgentConversationView::AppendChanges(const QJsonArray& items) {
    for (const auto& value : items) {
        if (!value.isObject()) continue;
        auto* row = new DisclosureRow(true, d_->column);
        row->navigate = [this](QString kind, QString objectId) { emit NavigateRequested(kind, objectId); };
        row->SetReduced(d_->reduced);
        row->Update(value.toObject(), QStringLiteral("改动"), QString());
        d_->changes.append(row);
        d_->Add(row);
    }
    emit ContentsChanged();
}

void AgentConversationView::SetReducedMotion(bool reduced) {
    d_->reduced = reduced;
    for (auto* row : d_->tools) row->SetReduced(reduced);
    for (auto* row : d_->changes) row->SetReduced(reduced);
}

void AgentConversationView::SetContentWidth(int width) {
    d_->contentWidth = std::max(120, width);
    d_->column->setMinimumWidth(0);
    d_->column->setMaximumWidth(std::max(120, std::min(d_->contentWidth, viewport()->width() - 32)));
}

void AgentConversationView::Clear() {
    d_->diagnosticTimer.stop();
    d_->active = nullptr;
    d_->messages.clear();
    d_->tools.clear();
    d_->changes.clear();
    while (d_->flow->count() > 1) {
        QLayoutItem* item = d_->flow->takeAt(1);
        delete item->widget();
        delete item;
    }
    d_->empty->show();
    d_->follow = true;
    emit ContentsChanged();
}

QString AgentConversationView::toPlainText() const {
    QStringList parts;
    for (int i = 1; i < d_->flow->count(); ++i) {
        QWidget* widget = d_->flow->itemAt(i)->widget();
        if (auto* row = dynamic_cast<MessageRow*>(widget)) parts.append(row->body->Source());
        else if (auto* disclosure = dynamic_cast<DisclosureRow*>(widget)) parts.append(disclosure->PlainText());
    }
    return parts.join(QStringLiteral("\n\n"));
}

QTextDocument* AgentConversationView::ActiveDocument() const { return d_->active == nullptr ? nullptr : d_->active->body->document(); }
int AgentConversationView::ToolCount() const { return static_cast<int>(d_->tools.size()); }
int AgentConversationView::ChangeCount() const { return static_cast<int>(d_->changes.size()); }

void AgentConversationView::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    d_->canvas->setMinimumWidth(0);
    SetContentWidth(d_->contentWidth);
    ScheduleLayoutDiagnostics();
}

void AgentConversationView::SetLayoutDiagnosticPath(const QString& path) {
    d_->diagnostics = AgentLayoutDiagnostics(path);
    ScheduleLayoutDiagnostics();
}

QString AgentConversationView::LayoutDiagnosticError() const { return d_->diagnostics.LastError(); }

void AgentConversationView::ScheduleLayoutDiagnostics() {
    // 不在每个delta枚举控件，也不反复重启导致持续流式时永远没有采样。
    if (d_->diagnostics.LastError().isEmpty() && !d_->diagnosticTimer.isActive()) d_->diagnosticTimer.start(250);
}

bool AgentConversationView::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == d_->column || watched == d_->canvas)
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Show))
        ScheduleLayoutDiagnostics();
    return QScrollArea::eventFilter(watched, event);
}

void AgentConversationView::InspectLayoutDiagnostics() {
    if (!isVisible()) return;
    AgentLayoutSnapshot snapshot;
    snapshot.viewportSize = viewport()->size(); snapshot.columnSize = d_->column->size();
    snapshot.devicePixelRatio = devicePixelRatioF();
    const int first = std::max(1, d_->flow->count() - 512);
    snapshot.omittedRows = first - 1;
    // 布局顺序是唯一行序来源；隐藏行与末尾stretch不参与相邻间隙计算。
    for (int i = first; i < d_->flow->count(); ++i) {
        auto* widget = d_->flow->itemAt(i)->widget();
        if (!widget || !widget->isVisible()) continue;
        AgentLayoutRowSnapshot row;
        row.index = i - 1; row.geometry = widget->geometry();
        row.minimumSize = widget->minimumSize(); row.sizeHint = widget->sizeHint();
        if (auto* message = dynamic_cast<MessageRow*>(widget)) {
            row.kind = widget->objectName() == QStringLiteral("agentUserTurn")
                ? AgentLayoutRowKind::User : AgentLayoutRowKind::Assistant;
            row.hasDocument = true;
            row.bodyGeometry = QRect(message->body->mapTo(d_->column, QPoint()), message->body->size());
            row.documentHeight = message->body->document()->size().height();
        } else if (dynamic_cast<DisclosureRow*>(widget)) {
            row.kind = widget->objectName() == QStringLiteral("agentInlineChanges")
                ? AgentLayoutRowKind::Change : AgentLayoutRowKind::Tool;
        }
        snapshot.rows.append(row);
    }
    const auto result = d_->diagnostics.Observe(snapshot, d_->diagnosticClock.elapsed());
    if (result == AgentLayoutDiagnostics::Observation::RateLimited) {
        // 不保存这份过渡几何；窗口结束只重采最新布局，稳定的最终异常也能留下现场。
        d_->diagnosticTimer.start(d_->diagnostics.RetryAfterMs(d_->diagnosticClock.elapsed()));
    }
    if (result == AgentLayoutDiagnostics::Observation::WriteFailed
        || result == AgentLayoutDiagnostics::Observation::BudgetExhausted)
        emit LayoutDiagnosticFailed(d_->diagnostics.LastError());
}

} // namespace War3Map::Agent
