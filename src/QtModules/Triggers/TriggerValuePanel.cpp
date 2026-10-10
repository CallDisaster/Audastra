#include "TriggerValuePanel.h"

#include "TriggerUiText.h"
#include "Widgets/ElidedLabel.h"
#include "Icons/SvgIcons.h"

#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <QScreen>
#include <QSettings>
#include <QSizeGrip>
#include <QTabBar>
#include <QVBoxLayout>

using namespace TriggerUiText;
namespace Trigger = War3Map::Trigger;

namespace {

constexpr int kKindRole = Qt::UserRole + 1;  // 0 预设，1 变量，2 函数，3 直接值，4 局部变量（读取函数）
constexpr int kValueRole = Qt::UserRole + 2;
constexpr int kCallRole = Qt::UserRole + 3;  // kind 4：读取函数名
constexpr int kMapSnapshotRole = Qt::UserRole + 4;

QListWidgetItem* Item(QListWidget* list, const QString& text, const QString& detail, int kind, const QString& value) {
    auto* item = new QListWidgetItem(detail.isEmpty() ? text : QStringLiteral("%1　%2").arg(text, detail), list);
    item->setData(kKindRole, kind);
    item->setData(kValueRole, value);
    return item;
}

} // namespace

TriggerValuePanel::TriggerValuePanel(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    War3Map::StableId ecaId, std::size_t position, ObjectLister objects,
    TriggerFunctionList::CategoryIcon categoryIcon, QWidget* parent,
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjects, QString mapObjectFailure)
    : QFrame(parent, Qt::Popup),
      document_(document),
      catalog_(catalog),
      mapObjects_(std::move(mapObjects)),
      mapObjectFailure_(std::move(mapObjectFailure)),
      types_(std::make_unique<Trigger::TriggerTypeSystem>(catalog, mapObjects_.get())),
      functions_(catalog, std::move(categoryIcon)),
      objects_(std::move(objects)) {
    setObjectName("triggerValuePanel");
    setFrameShape(QFrame::StyledPanel);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(420, 320);

    // 参数、声明类型与解析后的期望类型；局部变量按所在触发器收集。
    QString label;
    for (const auto& node : document_.ecaNodes) {
        if (node.id == ecaId) eca_ = &node;
    }
    if (eca_ != nullptr && position < eca_->arguments.size() && eca_->arguments[position] < document_.argumentNodes.size()) {
        argument_ = &document_.argumentNodes[eca_->arguments[position]];
        if (const auto* definition = catalog_.Find(eca_->type, eca_->name.text)) {
            declared_ = Trigger::TriggerTypeSystem::DeclaredType(*definition, position);
            const auto labels = PlaceholderLabels(*definition);
            if (position < labels.size()) label = Utf8(labels[position]);
        }
        for (const auto& trigger : document_.triggers) {
            if (trigger.id == eca_->sourceTriggerId) locals_ = types_->CollectLocals(document_, trigger);
        }
        type_ = types_->ExpectedType(document_, *eca_, position, locals_);
        writableTarget_ = declared_ == "AnyGlobal" || (eca_->name.text == "SetVariable" && position == 0);
        if (argument_->callEca.has_value() && *argument_->callEca < document_.ecaNodes.size()) {
            currentCall_ = &document_.ecaNodes[*argument_->callEca];
        }
    }
    if (objects_ && !type_.empty()) objectItems_ = objects_(type_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    const QString typeText = type_ == "Null" ? tr("类型待定（先设置前面的类型参数）")
        : type_.empty() ? tr("未知类型") : Utf8(types_->DisplayName(type_));
    if (label.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0 ||
        (currentCall_ == nullptr && (argument_ == nullptr || argument_->value.text.empty()))) label = tr("未设置");
    auto* title = new War3Map::UiKit::ElidedLabel(QStringLiteral("%1 · %2").arg(label.isEmpty() ? tr("参数") : label, typeText), this);
    title->setObjectName("valuePanelTitle");
    title->setToolTip(Utf8(type_));
    layout->addWidget(title);
    // 当前值不占用列表或提示区；标题与参数各一行，长文本只在悬停时展开。
    auto* current = new QFrame(this);
    current->setObjectName("valuePanelCurrentCard");
    auto* currentLayout = new QVBoxLayout(current);
    currentLayout->setContentsMargins(12, 8, 12, 8);
    currentLayout->setSpacing(4);
    auto* currentRow = new QHBoxLayout;
    currentRow->setContentsMargins(0, 0, 0, 0);
    currentRow->setSpacing(8);
    auto* icon = new QLabel(current);
    icon->setObjectName("valuePanelCurrentIcon");
    icon->setFixedSize(16, 16);
    auto* name = new War3Map::UiKit::ElidedLabel(current);
    name->setObjectName("valuePanelCurrentName");
    QString summary;
    if (currentCall_) {
        const auto* definition = catalog_.Find(currentCall_->type, currentCall_->name.text);
        name->setText(definition ? functions_.Title(*definition) : Utf8(currentCall_->name.text));
        const auto currentCategoryIcon = definition ? functions_.Icon(*definition) : QIcon{};
        icon->setPixmap((currentCategoryIcon.isNull() ? War3Map::UiKit::SvgIcon(QStringLiteral("lucide/puzzle"))
            : currentCategoryIcon).pixmap(16, 16));
        summary = Utf8(Trigger::PresentEca(document_, *currentCall_, catalog_).text);
    } else {
        name->setText(argument_ && !argument_->value.text.empty() ? Utf8(argument_->value.text) : tr("未设置"));
        summary = argument_ && argument_->type == 1 ? tr("全局变量 · %1").arg(typeText)
            : argument_ && argument_->type == 0 ? tr("预设 · %1").arg(typeText) : typeText;
        icon->setPixmap(War3Map::UiKit::SvgIcon(argument_ && argument_->type == 1
            ? QStringLiteral("lucide/variable") : QStringLiteral("lucide/hash")).pixmap(16, 16));
    }
    currentRow->addWidget(icon);
    currentRow->addWidget(name, 1);
    editCallButton_ = new QPushButton(tr("编辑参数"), current);
    editCallButton_->setObjectName("valuePanelEditCall");
    editCallButton_->setVisible(currentCall_ != nullptr && !currentCall_->arguments.empty());
    currentRow->addWidget(editCallButton_);
    currentLayout->addLayout(currentRow);
    auto* detail = new War3Map::UiKit::ElidedLabel(summary.simplified(), current);
    detail->setObjectName("valuePanelCurrentSummary");
    detail->setToolTip(summary);
    currentLayout->addWidget(detail);
    layout->addWidget(current);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName("valuePanelTabs");
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    layout->addWidget(tabs_);
    search_ = new QLineEdit(this);
    search_->setObjectName("valuePanelSearch");
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    layout->addWidget(search_);
    list_ = new QListWidget(this);
    list_->setObjectName("valuePanelList");
    list_->setMinimumHeight(96);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->setIconSize(QSize(16, 16));
    list_->installEventFilter(this);
    layout->addWidget(list_, 1);
    note_ = new QLabel(this);
    note_->setObjectName("valuePanelNote");
    note_->setWordWrap(true);
    layout->addWidget(note_);
    create_ = new QPushButton(this);
    create_->setObjectName("valuePanelCreate");
    create_->hide();
    layout->addWidget(create_);
    auto* hint = new War3Map::UiKit::ElidedLabel(tr("Enter 使用 · Ctrl+Tab 换页 · Esc 关闭 · 右下角拖动调整大小"), this);
    hint->setObjectName("paneHint");
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addWidget(hint, 1);
    footer->addWidget(new QSizeGrip(this), 0, Qt::AlignBottom | Qt::AlignRight);
    layout->addLayout(footer);

    if (argument_ == nullptr || argument_->insertIndex != 0) {
        note_->setText(argument_ == nullptr ? tr("找不到这个参数。") : tr("数组下标参数请在参数对话框（F2）中修改。"));
        tabs_->hide();
        search_->hide();
        list_->hide();
        editCallButton_->hide();
        return;
    }
    connect(tabs_, &QTabBar::currentChanged, this, [this](int tab) {
        if (tab < 0 || tab >= static_cast<int>(pages_.size())) return;
        {
            const QSignalBlocker blocker(search_); // 换页清空搜索，避免上一页的输入过滤新页
            search_->clear();
        }
        SelectPage(pages_[static_cast<std::size_t>(tab)]);
    });
    connect(search_, &QLineEdit::textChanged, this, [this]() { Rebuild(); });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { Apply(); });
    connect(create_, &QPushButton::clicked, this, [this]() {
        if (createAction_) createAction_();
    });
    connect(editCallButton_, &QPushButton::clicked, this, [this]() {
        const auto editCall = editCall_;
        const War3Map::StableId id = currentCall_ == nullptr ? 0 : currentCall_->id;
        close();
        if (editCall && id != 0) editCall(id);
    });
    BuildPages();
}

TriggerValuePanel::~TriggerValuePanel() = default;

void TriggerValuePanel::SetReferencePresentation(const Trigger::ReferencePresentation& shown, const QIcon& icon,
    std::optional<QString> stringBody) {
    QString tooltip = tr("原值：%1").arg(Utf8(shown.rawValue));
    if (!shown.reference.empty()) tooltip += tr("\n来自地图字符串表：%1").arg(Utf8(shown.reference));
    if (!shown.issue.empty()) tooltip += QLatin1Char('\n') + Utf8(shown.issue);
    for (const auto& candidate : shown.candidates) tooltip += QStringLiteral("\n• ") + Utf8(candidate);
    if (auto* name = findChild<War3Map::UiKit::ElidedLabel*>("valuePanelCurrentName")) {
        name->setText(Utf8(shown.displayName));
        name->setToolTip(tooltip);
    }
    if (auto* image = findChild<QLabel*>("valuePanelCurrentIcon"); image && !icon.isNull()) image->setPixmap(icon.pixmap(16, 16));
    if (stringBody && page_ == Page::Value && objectItems_.empty()) {
        originalStringBody_ = *stringBody;
        // 多行正文保留原来的换行；Ctrl+Enter 使用，Enter 插入换行。
        if (stringBody->contains(QLatin1Char('\n')) || stringBody->contains(QLatin1Char('\r'))) {
            literalText_ = new QPlainTextEdit(this);
            literalText_->setObjectName("valuePanelStringBody");
            literalText_->setMaximumHeight(150);
            literalText_->setPlainText(*stringBody);
            initialStringInput_ = literalText_->toPlainText();
            literalText_->setToolTip(tooltip + tr("\nCtrl+Enter 使用；Enter 换行。"));
            literalText_->installEventFilter(this);
            auto* box = static_cast<QVBoxLayout*>(layout());
            box->insertWidget(box->indexOf(search_), literalText_);
            search_->hide();
            connect(literalText_, &QPlainTextEdit::textChanged, this, &TriggerValuePanel::Rebuild);
            Rebuild();
        } else {
            search_->setText(*stringBody);
            initialStringInput_ = search_->text();
            search_->selectAll();
            search_->setToolTip(tooltip);
        }
        Rebuild();
    }
}

bool TriggerValuePanel::Numeric() const {
    const auto kind = types_->LiteralKind(type_);
    return kind == Trigger::TriggerTypeSystem::Literal::Integer || kind == Trigger::TriggerTypeSystem::Literal::Real;
}

bool TriggerValuePanel::Boolean() const {
    // boolean 无论是否有 YDWE 预设都提供原语；专用派生枚举保持自己的文字与语义。
    return types_->LiteralKind(type_) == Trigger::TriggerTypeSystem::Literal::Boolean &&
        (type_ == "boolean" || PresetsFor(catalog_, type_).empty());
}

bool TriggerValuePanel::TextLike() const {
    const auto* info = types_->Find(type_);
    return type_ == "string" || type_ == "scriptcode" || declared_ == "scriptcode" ||
        (info != nullptr && info->baseType == "string") || type_ == "Null" || type_.empty() || info == nullptr;
}

void TriggerValuePanel::BuildPages() {
    const bool anyGlobal = declared_ == "AnyGlobal";
    const bool script = declared_ == "scriptcode" || type_ == "scriptcode";
    const bool selector = types_->IsTypeSelector(type_);
    if (anyGlobal) {
        pages_ = {Page::Variable};
    } else if (script) {
        pages_ = {Page::Value};
    } else {
        if (Boolean() || !PresetsFor(catalog_, type_).empty()) pages_.push_back(Page::Preset);
        if (!selector) {
            // 变量/函数页按函数库类型定义开放；当前值已用的来源始终保留。
            const auto kinds = types_->AllowedValueKinds(type_);
            const bool currentLocal = currentCall_ != nullptr &&
                (currentCall_->name.text == Trigger::kLocalGet || currentCall_->name.text == Trigger::kLoopLocalGet);
            if (kinds.variable || argument_->type == 1 || currentLocal) pages_.push_back(Page::Variable);
            if (kinds.function || currentCall_ != nullptr) pages_.push_back(Page::Function);
            if (!objectItems_.empty() || Numeric() || TextLike()) pages_.push_back(Page::Value);
        }
        if (pages_.empty()) pages_.push_back(Page::Value);
    }
    for (const Page page : pages_) {
        switch (page) {
        case Page::Preset: tabs_->addTab(tr("预设")); break;
        case Page::Variable: tabs_->addTab(tr("变量")); break;
        case Page::Function: tabs_->addTab(tr("函数")); break;
        case Page::Value:
            tabs_->addTab(!objectItems_.empty() ? tr("物体") : script ? tr("文本") : tr("值"));
            break;
        }
    }
    // 默认页：当前值的来源（局部变量读取在“变量”页）。
    const bool localGetter = currentCall_ != nullptr &&
        (currentCall_->name.text == Trigger::kLocalGet || currentCall_->name.text == Trigger::kLoopLocalGet);
    Page initial = pages_.front();
    const auto has = [this](Page page) { return std::find(pages_.begin(), pages_.end(), page) != pages_.end(); };
    if (localGetter && has(Page::Variable)) initial = Page::Variable;
    else if (currentCall_ != nullptr && has(Page::Function)) initial = Page::Function;
    else if (argument_->type == 0 && has(Page::Preset)) initial = Page::Preset;
    else if (argument_->type == 1 && has(Page::Variable)) initial = Page::Variable;
    else if (has(Page::Value)) initial = Page::Value;
    SelectPage(initial);
    if (page_ == Page::Value && objectItems_.empty() && currentCall_ == nullptr && argument_->type == 3) {
        search_->setText(Utf8(argument_->value.text));
        search_->selectAll();
    }
}

void TriggerValuePanel::SelectPage(Page page) {
    page_ = page;
    if (literalText_) {
        literalText_->setVisible(page == Page::Value);
        search_->setVisible(page != Page::Value);
    }
    const auto at = std::find(pages_.begin(), pages_.end(), page);
    if (at != pages_.end() && tabs_->currentIndex() != at - pages_.begin()) {
        const QSignalBlocker blocker(tabs_);
        tabs_->setCurrentIndex(static_cast<int>(at - pages_.begin()));
    }
    switch (page) {
    case Page::Preset: search_->setPlaceholderText(tr("搜索预设")); break;
    case Page::Variable: search_->setPlaceholderText(tr("搜索变量，或输入名称新建")); break;
    case Page::Function:
        search_->setPlaceholderText(tr("搜索返回“%1”的函数").arg(Utf8(types_->DisplayName(type_))));
        break;
    case Page::Value:
        search_->setPlaceholderText(!objectItems_.empty() ? tr("搜索名称或 ID")
            : Numeric() ? tr("输入十进制数字") : tr("输入文本"));
        break;
    }
    Rebuild();
    if (literalText_ && page_ == Page::Value) literalText_->setFocus();
    else search_->setFocus();
}

void TriggerValuePanel::Rebuild() {
    list_->clear();
    note_->clear();
    create_->hide();
    createAction_ = nullptr;
    const QString query = search_->text().trimmed();
    switch (page_) {
    case Page::Preset:
        if (Boolean()) {
            for (const auto& value : {std::pair{tr("真"), QStringLiteral("true")}, std::pair{tr("假"), QStringLiteral("false")}}) {
                if (query.isEmpty() || value.first.contains(query, Qt::CaseInsensitive) || value.second.contains(query, Qt::CaseInsensitive))
                    Item(list_, value.first, value.second, 3, value.second);
            }
        }
        for (const auto& preset : PresetsFor(catalog_, type_)) {
            const QString name = Utf8(preset.name);
            if (query.isEmpty() || preset.display.contains(query, Qt::CaseInsensitive) || name.contains(query, Qt::CaseInsensitive)) {
                Item(list_, preset.display, name, 0, name);
            }
        }
        break;
    case Page::Variable: RebuildVariables(query); break;
    case Page::Function: RebuildFunctions(query); break;
    case Page::Value: RebuildValues(query); break;
    }
    note_->setVisible(!note_->text().trimmed().isEmpty());
    note_->setToolTip(note_->text());
    // 换行提示的最低高度取决于当前宽度；minimumSize() 只计单行最低提示，
    // 会在列表仍坚持 96px 最低高度时相交。使用列布局的最低 HFW（不是包含
    // 列表偏好高度的 heightForWidth），并计入 QFrame 的外层内容边距。
    layout()->invalidate();
    QSize minimum = QSize(420, 320).expandedTo(layout()->totalMinimumSize());
    const QMargins frameMargins = contentsMargins();
    const int contentWidth = std::max(width(), minimum.width()) - frameMargins.left() - frameMargins.right();
    if (const auto* column = qobject_cast<const QVBoxLayout*>(layout()); column && column->hasHeightForWidth()) {
        const int minimumHeight = column->minimumHeightForWidth(contentWidth);
        if (minimumHeight >= 0) minimum.setHeight(std::max(minimum.height(), minimumHeight + frameMargins.top() + frameMargins.bottom()));
    }
    setMinimumSize(minimum);
    layout()->activate();
    SelectCurrent();
}

void TriggerValuePanel::RebuildVariables(const QString& query) {
    const auto matches = [&query](const QString& text) { return query.isEmpty() || text.contains(query, Qt::CaseInsensitive); };
    const bool anyGlobal = declared_ == "AnyGlobal";
    std::size_t arrays = 0;
    for (const auto& variable : document_.variables) {
        const std::string& type = variable.typeName.text;
        if (!anyGlobal && !types_->Accepts(type_, type)) continue;
        if (variable.isArray != 0) {
            ++arrays;
            continue;
        }
        const QString name = Utf8(variable.name.text);
        if (matches(name)) Item(list_, name, type == type_ ? QString{} : Utf8(types_->DisplayName(type)), 1, name);
    }
    // 预放单位/物品是读取来源，不是用户可赋值的全局变量或数组。
    if (!writableTarget_ && mapObjects_) {
        std::size_t shown = 0;
        for (const auto& object : mapObjects_->References()) {
            if (!types_->Accepts(type_, object.type)) continue;
            const QString symbol = Utf8(object.symbol);
            const QString detail = tr("预放%1 · %2 · 编号 %3 · 只读")
                .arg(Utf8(types_->DisplayName(object.type)), Utf8(object.rawcode)).arg(object.creationId);
            if (!matches(symbol) && !matches(detail)) continue;
            const auto resolved = Trigger::ResolveVariableReference(document_, object.symbol, mapObjects_.get());
            if (!resolved.Succeeded() || resolved.value->kind != Trigger::VariableReferenceKind::PreplacedObject) continue;
            if (++shown > 400) {
                note_->setText(tr("预放对象结果较多，只列出前 400 个；输入类型 ID 或创建编号缩小范围。"));
                break;
            }
            auto* item = Item(list_, symbol, detail, 1, symbol);
            item->setData(kMapSnapshotRole, Utf8(mapObjects_->Identity()));
            item->setToolTip(tr("%1\n来源：%2\n分类来源：%3")
                .arg(detail, Utf8(object.memberName), Utf8(object.classificationSource)));
        }
    }
    // YDWE 局部变量（本触发器）：选择后生成 loc_ / loop_ 读取函数。
    const bool hasGetter = catalog_.Find(Trigger::UiDefinitionKind::Call, Trigger::kLocalGet) != nullptr;
    const bool hasLoop = catalog_.Find(Trigger::UiDefinitionKind::Call, Trigger::kLoopLocalGet) != nullptr;
    if (!anyGlobal) {
        for (const auto& local : locals_) {
            if (local.isArray || (local.loop ? !hasLoop : !hasGetter) || !types_->Accepts(type_, local.type)) continue;
            const QString shown = (local.loop ? QStringLiteral("loop_") : QStringLiteral("loc_")) + Utf8(local.name);
            if (!matches(shown)) continue;
            auto* item = Item(list_, shown, tr("局部 · %1").arg(Utf8(types_->DisplayName(local.type))), 4, Utf8(local.name));
            item->setData(kCallRole, Utf8(local.loop ? Trigger::kLoopLocalGet : Trigger::kLocalGet));
        }
    }
    if (list_->count() == 0) note_->setText(query.isEmpty() ? tr("没有该类型的变量。") : tr("没有匹配的变量。"));
    if (arrays != 0) {
        note_->setText(note_->text() + tr(" 另有 %1 个数组变量，需要下标，请在参数对话框（F2）中设置。").arg(arrays));
    }
    if (!writableTarget_ && !mapObjectFailure_.isEmpty() &&
        (types_->Accepts(type_, "unit") || types_->Accepts(type_, "item"))) {
        note_->setText(note_->text() + tr(" 预放对象不可用：%1").arg(mapObjectFailure_));
    }
    const bool exists = std::any_of(document_.variables.begin(), document_.variables.end(),
        [&query](const auto& variable) { return Utf8(variable.name.text) == query; });
    const bool concrete = !type_.empty() && type_ != "Null" && type_ != "scriptcode" && !types_->IsTypeSelector(type_);
    if (!query.isEmpty() && !exists && concrete && !anyGlobal) {
        create_->setText(tr("新建“%1”全局变量“%2”并使用").arg(Utf8(types_->DisplayName(type_)), query));
        create_->show();
        createAction_ = [this, name = query.toStdString()]() { Choose({1, name, std::nullopt, {}, type_}); };
    }
}

void TriggerValuePanel::RebuildFunctions(const QString& query) {
    // 同类型在前，兼容类型（基础类型互通）其次，YDWE 的 AnyReturnType 函数最后；组内按 WE 列表标题排序。
    std::vector<const Trigger::UiDefinition*> exact;
    std::vector<const Trigger::UiDefinition*> compatible;
    std::vector<const Trigger::UiDefinition*> generic;
    for (const auto* definition : catalog_.Definitions(Trigger::UiDefinitionKind::Call)) {
        const std::string returns = CallReturnType(*definition);
        if (returns.empty() || !types_->Accepts(type_, returns)) continue;
        if (!functions_.Matches(*definition, query)) continue;
        (returns == type_ ? exact : returns == "AnyReturnType" ? generic : compatible).push_back(definition);
    }
    for (auto* group : {&exact, &compatible, &generic}) {
        functions_.Sort(*group);
        for (const auto* definition : *group) {
            const std::string returns = CallReturnType(*definition);
            const QString detail = returns == type_ ? Utf8(definition->name)
                : QStringLiteral("%1 · %2").arg(Utf8(definition->name),
                    returns == "AnyReturnType" ? tr("任意类型") : Utf8(types_->DisplayName(returns)));
            auto* item = Item(list_, functions_.Title(*definition), detail, 2, Utf8(definition->name));
            item->setIcon(functions_.Icon(*definition));
            item->setToolTip(QStringLiteral("%1\n%2").arg(functions_.Title(*definition), detail));
        }
    }
    if (list_->count() == 0) {
        note_->setText(tr("没有返回该类型的函数。"));
    }
}

void TriggerValuePanel::RebuildValues(const QString& query) {
    const auto matches = [&query](const QString& text) { return query.isEmpty() || text.contains(query, Qt::CaseInsensitive); };
    if (!objectItems_.empty()) {
        int shown = 0;
        for (const auto& object : objectItems_) {
            if (!matches(object.name) && !matches(object.rawcode)) continue;
            if (++shown > 400) {
                note_->setText(tr("结果较多，只列出前 400 个；输入名称或 ID 缩小范围。"));
                break;
            }
            Item(list_, object.name, object.rawcode, 3, object.rawcode);
        }
        return;
    }
    const QString text = literalText_ && page_ == Page::Value ? literalText_->toPlainText() : search_->text();
    if (Numeric()) {
        const auto checked = types_->ValidateLiteral(type_, text.trimmed().toStdString());
        if (checked.Succeeded()) Item(list_, tr("使用“%1”").arg(text.trimmed()), {}, 3, text.trimmed());
        else if (!query.isEmpty()) note_->setText(Utf8(checked.diagnostics.front().message));
        return;
    }
    const auto checked = types_->ValidateLiteral(type_, text.toStdString());
    if (!checked.Succeeded()) { note_->setText(Utf8(checked.diagnostics.front().message)); return; }
    if (!text.isEmpty() || types_->LiteralKind(type_) == Trigger::TriggerTypeSystem::Literal::String)
        Item(list_, tr("使用“%1”").arg(text), {}, 3,
            originalStringBody_ && text == initialStringInput_ ? Utf8(argument_->value.text) : text);
    // YDWE 局部变量名（scriptcode）：列出本触发器已有的变量名，避免拼写不一致。
    const bool localName = eca_ != nullptr && declared_ == "scriptcode" &&
        (eca_->name.text == Trigger::kLocalGet || eca_->name.text == Trigger::kLocalSet ||
            eca_->name.text == Trigger::kLocalGetArray || eca_->name.text == Trigger::kLocalSetArray);
    if (localName) {
        for (const auto& local : locals_) {
            const QString name = Utf8(local.name);
            if (!local.loop && name != text && matches(name)) {
                Item(list_, name, tr("局部 · %1").arg(Utf8(types_->DisplayName(local.type))), 3, name);
            }
        }
    }
}

void TriggerValuePanel::SelectCurrent() {
    // 当前值：局部变量读取按变量名，函数按函数名，其余按参数文本。
    int kind = argument_->type;
    QString value = Utf8(argument_->value.text);
    if (currentCall_ != nullptr) {
        kind = 2;
        value = Utf8(currentCall_->name.text);
        const bool local = currentCall_->name.text == Trigger::kLocalGet || currentCall_->name.text == Trigger::kLoopLocalGet;
        if (local && !currentCall_->arguments.empty() && currentCall_->arguments[0] < document_.argumentNodes.size()) {
            kind = 4;
            value = Utf8(document_.argumentNodes[currentCall_->arguments[0]].value.text);
        }
    }
    for (int row = 0; row < list_->count(); ++row) {
        const auto* item = list_->item(row);
        if (item->data(kValueRole).toString() == value && item->data(kKindRole).toInt() == kind) {
            list_->setCurrentRow(row);
            return;
        }
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
}

void TriggerValuePanel::Apply() {
    if (argument_ == nullptr) {
        return;
    }
    if (auto* item = list_->currentItem()) {
        const int kind = item->data(kKindRole).toInt();
        const std::string value = item->data(kValueRole).toString().toStdString();
        if (kind == 4) {
            Choose({2, {}, item->data(kCallRole).toString().toStdString(), {{3, value}}, std::nullopt});
        } else if (kind == 2) {
            Choose({2, {}, value, {}, std::nullopt});
        } else {
            Choose({kind, value, std::nullopt, {}, std::nullopt,
                item->data(kMapSnapshotRole).toString().toStdString()});
        }
        return;
    }
    if (createAction_) {
        createAction_();
    }
}

void TriggerValuePanel::Choose(const TriggerValueChoice& choice) {
    const auto chosen = chosen_;
    close();
    if (chosen) chosen(choice);
}

bool TriggerValuePanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == literalText_ && !(key->modifiers() & Qt::ControlModifier))
                return QFrame::eventFilter(watched, event);
            Apply();
            return true;
        }
        if (watched == search_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) && list_->count() > 0) {
            const int row = std::clamp(list_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1), 0, list_->count() - 1);
            list_->setCurrentRow(row);
            return true;
        }
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::ControlModifier && tabs_->count() > 0) {
            tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

namespace {
const QString kValuePanelSizeKey = QStringLiteral("triggers/valuePanel/size");
}

void TriggerValuePanel::hideEvent(QHideEvent* event) {
    QSettings().setValue(kValuePanelSizeKey, size());
    QFrame::hideEvent(event);
}

void TriggerValuePanel::ShowAt(const QRect& anchor) {
    adjustSize();
    if (const QSize saved = QSettings().value(kValuePanelSizeKey).toSize(); saved.isValid()) {
        resize(saved.expandedTo(minimumSize()));
    }
    QPoint origin = anchor.bottomLeft() + QPoint(0, 4);
    if (const QScreen* screen = QGuiApplication::screenAt(anchor.center())) {
        const QRect available = screen->availableGeometry();
        if (origin.y() + height() > available.bottom()) origin.setY(anchor.top() - height() - 4); // 下方不够：翻到上方
        origin.setX(std::clamp(origin.x(), available.left(), std::max(available.left(), available.right() - width())));
    }
    move(origin);
    show();
    if (literalText_ && page_ == Page::Value) literalText_->setFocus();
    else search_->setFocus();
}
