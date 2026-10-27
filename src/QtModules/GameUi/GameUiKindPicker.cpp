#include "GameUiKindPicker.h"

#include "GameUiCanvas.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

constexpr int kKindRole = Qt::UserRole + 1;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// “可换：贴图、文字；可取数值：进度、显示”。
QString SlotsText(const UiFrameKind& kind) {
    QStringList all;
    QStringList bound;
    for (const auto& slot : kind.swappable) {
        all.push_back(Text(slot.label));
        if (slot.bindable) bound.push_back(Text(slot.label));
    }
    QString text = QObject::tr("可换：%1").arg(all.join(QStringLiteral("、")));
    if (!bound.isEmpty()) text += QObject::tr("；可取数值：%1").arg(bound.join(QStringLiteral("、")));
    return text;
}

} // namespace

GameUiKindPicker::GameUiKindPicker(GameUiModule& module, QList<Target> targets, int current, QWidget* parent)
    : QFrame(parent, Qt::Popup), module_(module), targets_(std::move(targets)) {
    setObjectName(QStringLiteral("gameUiKindPicker"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* body = new QHBoxLayout;
    body->setSpacing(8);
    kinds_ = new QTreeWidget(this);
    kinds_->setObjectName(QStringLiteral("gameUiKindList"));
    kinds_->setHeaderHidden(true);
    kinds_->setRootIsDecorated(false);
    kinds_->setIndentation(12);
    kinds_->setFixedWidth(150);
    QTreeWidgetItem* group = nullptr;
    for (const auto& kind : UiFrameKinds()) {
        if (!group || group->text(0) != Text(kind.group)) {
            group = new QTreeWidgetItem(kinds_, {Text(kind.group)});
            group->setFlags(Qt::ItemIsEnabled);
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
            group->setExpanded(true);
        }
        auto* item = new QTreeWidgetItem(group, {Text(kind.label)});
        item->setData(0, kKindRole, Text(kind.id));
    }
    kinds_->expandAll();
    // 类型不多，全部列出不滚动。
    int rows = 0;
    for (QTreeWidgetItemIterator it(kinds_); *it; ++it) ++rows;
    kinds_->setMinimumHeight(rows * kinds_->sizeHintForRow(0) + 2 * kinds_->frameWidth() + 4);
    body->addWidget(kinds_);

    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    canvas_ = new GameUiCanvas(this);
    canvas_->setObjectName(QStringLiteral("gameUiKindPreview"));
    canvas_->setMinimumSize(0, 0);
    canvas_->setFixedSize(300, 170);
    canvas_->setAttribute(Qt::WA_TransparentForMouseEvents);
    canvas_->SetTextures(&module_.Textures());
    canvas_->SetFontFamilies(&module_.FontFamilies());
    canvas_->SetAspect(module_.Aspect());
    right->addWidget(canvas_);
    title_ = new QLabel(this);
    QFont titleFont = title_->font();
    titleFont.setBold(true);
    title_->setFont(titleFont);
    purpose_ = new QLabel(this);
    usage_ = new QLabel(this);
    slots_ = new QLabel(this);
    for (QLabel* label : {purpose_, usage_, slots_}) {
        label->setWordWrap(true);
        label->setFixedWidth(300);
    }
    purpose_->setObjectName(QStringLiteral("gameUiKindPurpose"));
    usage_->setObjectName(QStringLiteral("gameUiKindUsage"));
    slots_->setObjectName(QStringLiteral("gameUiKindSlots"));
    right->addWidget(title_);
    right->addWidget(purpose_);
    right->addWidget(usage_);
    right->addWidget(slots_);
    templateSearch_ = new QLineEdit(this);
    templateSearch_->setObjectName(QStringLiteral("gameUiKindTemplateSearch"));
    templateSearch_->setPlaceholderText(tr("搜索模板"));
    templateSearch_->setClearButtonEnabled(true);
    templates_ = new QListWidget(this);
    templates_->setObjectName(QStringLiteral("gameUiKindTemplates"));
    templates_->setFixedWidth(300);
    templates_->setMinimumHeight(160);
    templates_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    templates_->addItems(module_.FrameTemplates());
    if (templates_->count() > 0) templates_->setCurrentRow(0);
    right->addWidget(templateSearch_);
    right->addWidget(templates_, 1);
    right->addStretch(1);
    body->addLayout(right);
    layout->addLayout(body, 1);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(new QLabel(tr("放进"), this));
    target_ = new QComboBox(this);
    target_->setObjectName(QStringLiteral("gameUiKindTarget"));
    for (const auto& target : targets_) target_->addItem(target.label);
    target_->setCurrentIndex(std::clamp(current, 0, static_cast<int>(targets_.size()) - 1));
    target_->setMinimumWidth(220);
    bottom->addWidget(target_, 1);
    create_ = new QPushButton(tr("创建"), this);
    create_->setObjectName(QStringLiteral("gameUiKindCreate"));
    create_->setDefault(true);
    bottom->addWidget(create_);
    layout->addLayout(bottom);

    connect(kinds_, &QTreeWidget::currentItemChanged, this, &GameUiKindPicker::ShowKind);
    connect(kinds_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, kKindRole).isValid()) Accept();
    });
    connect(templateSearch_, &QLineEdit::textChanged, this, &GameUiKindPicker::FilterTemplates);
    connect(templateSearch_, &QLineEdit::returnPressed, this, &GameUiKindPicker::Accept);
    connect(templates_, &QListWidget::currentItemChanged, this, [this] { ShowKind(); });
    connect(templates_, &QListWidget::itemActivated, this, &GameUiKindPicker::Accept);
    connect(create_, &QPushButton::clicked, this, &GameUiKindPicker::Accept);
    connect(&module_, &GameUiModule::AssetsChanged, canvas_, qOverload<>(&QWidget::update));
    (void)SelectKind(QStringLiteral("backdrop"));
}

QString GameUiKindPicker::Kind() const {
    const QTreeWidgetItem* item = kinds_->currentItem();
    return item ? item->data(0, kKindRole).toString() : QString();
}

bool GameUiKindPicker::SelectKind(const QString& kind) {
    for (int g = 0; g < kinds_->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = kinds_->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            if (group->child(i)->data(0, kKindRole).toString() != kind) continue;
            kinds_->setCurrentItem(group->child(i));
            return true;
        }
    }
    return false;
}

void GameUiKindPicker::ShowKind() {
    const UiFrameKind* kind = FindUiFrameKind(Kind().toStdString());
    const bool isTemplate = kind && kind->type == UiFrameType::Template;
    if (templates_->isVisibleTo(this) != isTemplate) {
        templateSearch_->setVisible(isTemplate);
        templates_->setVisible(isTemplate);
        adjustSize();
    }
    if (!kind) {
        create_->setEnabled(false);
        return;
    }
    title_->setText(Text(kind->label));
    purpose_->setText(Text(kind->purpose));
    usage_->setText(Text(kind->usage));
    slots_->setText(SlotsText(*kind));
    const QListWidgetItem* chosen = templates_->currentItem();
    const QString templateName = isTemplate && chosen && !chosen->isHidden() ? chosen->text() : QString();
    create_->setEnabled(!targets_.isEmpty() && (!isTemplate || !templateName.isEmpty()));
    if (isTemplate && templateName.isEmpty()) {
        preview_ = {};
        canvas_->ClearScene();
        return;
    }
    UiFrame frame = DefaultUiFrame(*kind, "preview", templateName.toStdString());
    // 不定宽高的文字在预览里给一个框，便于放大；进度条画到一半多，底图也看得见。
    if (frame.type == UiFrameType::Text && !frame.width) {
        frame.width = 0.08;
        frame.height = 0.02;
    }
    if (frame.type == UiFrameType::Bar) frame.value.literal = 0.65;
    ShowPreview(frame);
}

void GameUiKindPicker::ShowPreview(const UiFrame& frame) {
    preview_ = module_.PreviewFrame(frame);
    canvas_->SetScene(preview_.tree.get(), preview_.list);
    canvas_->SetFocusRect(preview_.focus);
}

void GameUiKindPicker::FilterTemplates(const QString& text) {
    QListWidgetItem* first = nullptr;
    for (int i = 0; i < templates_->count(); ++i) {
        QListWidgetItem* item = templates_->item(i);
        item->setHidden(!item->text().contains(text.trimmed(), Qt::CaseInsensitive));
        if (!first && !item->isHidden()) first = item;
    }
    templates_->setCurrentItem(first);
    ShowKind();
}

void GameUiKindPicker::Accept() {
    if (!create_->isEnabled() || targets_.isEmpty()) return;
    const QString kind = Kind();
    const QListWidgetItem* chosen = templates_->currentItem();
    const UiFrameKind* found = FindUiFrameKind(kind.toStdString());
    const QString templateName = found && found->type == UiFrameType::Template && chosen ? chosen->text() : QString();
    const Target target = targets_.value(target_->currentIndex());
    close();
    emit Chosen(kind, templateName, target.description, target.frame);
}

} // namespace War3Map::Modules
