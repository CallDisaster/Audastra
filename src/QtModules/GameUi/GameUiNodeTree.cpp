#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiNodeTree.h"
#include "Icons/SvgIcons.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"

#include <QAbstractItemDelegate>
#include <QDir>
#include <QDropEvent>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTimer>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <string_view>
#include <unordered_set>

// 界面工作区左侧节点树：填充、搜索、只显示一项，以及每行的“可见”“可选”。
// 可见决定画不画、点不点得中；可选只决定画布上点不点得中。两项按节点身份记住，按地图存进本机视图状态。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;
using namespace War3Map::Modules::GameUiNodeTree;

// 本机最多记住多少张地图的可见、可选。
constexpr qsizetype kMapViewLimit = 30;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

QString DisplayName(const FrameNode& node) {
    if (!node.codeLabel.empty()) return Text(node.codeLabel);
    if (!node.name.empty()) return Text(node.name);
    switch (node.kind) {
    case DefinitionKind::Texture: return QObject::tr("（贴图）");
    case DefinitionKind::String: return QObject::tr("（文字）");
    case DefinitionKind::Layer: return QObject::tr("层 %1").arg(Text(node.frameType));
    default: return QObject::tr("（未命名）");
    }
}

// 自己的界面与控件：有显示名时写“显示名（编号）”，阵列实例再加 [序号]。
QString OwnDisplayName(const FrameNode& node, const UiDescription* description, const UiFrame* frame) {
    const auto& link = *node.uiFrame;
    const QString id = Text(link.frame.empty() ? link.description : link.frame);
    const std::string label = !link.frame.empty() ? (frame ? frame->label : std::string()) : description ? description->label : std::string();
    QString text = label.empty() || Text(label) == id ? id : QStringLiteral("%1（%2）").arg(Text(label), id);
    if (link.instance) text += QStringLiteral(" [%1]").arg(*link.instance);
    return text;
}

struct TypeEntry {
    std::string_view type;
    const char* label;
    const char* icon;
};

// 游戏框架类型的中文名与图标。
constexpr std::array<TypeEntry, 33> kTypes{{
    {"FRAME", QT_TR_NOOP("容器"), "lucide/square"},
    {"SIMPLEFRAME", QT_TR_NOOP("简单容器"), "lucide/square"},
    {"CONTROL", QT_TR_NOOP("控件"), "lucide/square"},
    {"DIALOG", QT_TR_NOOP("对话框"), "lucide/app-window"},
    {"BACKDROP", QT_TR_NOOP("背景图"), "lucide/image"},
    {"TEXT", QT_TR_NOOP("文字"), "lucide/type"},
    {"SIMPLETEXT", QT_TR_NOOP("文字"), "lucide/type"},
    {"TIMERTEXT", QT_TR_NOOP("计时文字"), "lucide/type"},
    {"TEXTAREA", QT_TR_NOOP("文字区域"), "lucide/type"},
    {"CHATDISPLAY", QT_TR_NOOP("聊天显示"), "lucide/type"},
    {"GLUETEXTBUTTON", QT_TR_NOOP("文字按钮"), "lucide/mouse-pointer-2"},
    {"GLUEBUTTON", QT_TR_NOOP("按钮"), "lucide/mouse-pointer-2"},
    {"BUTTON", QT_TR_NOOP("按钮"), "lucide/mouse-pointer-2"},
    {"TEXTBUTTON", QT_TR_NOOP("文字按钮"), "lucide/mouse-pointer-2"},
    {"SIMPLEBUTTON", QT_TR_NOOP("简单按钮"), "lucide/mouse-pointer-2"},
    {"SCRIPTDIALOGBUTTON", QT_TR_NOOP("对话框按钮"), "lucide/mouse-pointer-2"},
    {"INVOKEBUTTON", QT_TR_NOOP("按钮"), "lucide/mouse-pointer-2"},
    {"CHECKBOX", QT_TR_NOOP("复选框"), "lucide/check"},
    {"GLUECHECKBOX", QT_TR_NOOP("复选框"), "lucide/check"},
    {"SIMPLECHECKBOX", QT_TR_NOOP("复选框"), "lucide/check"},
    {"EDITBOX", QT_TR_NOOP("输入框"), "lucide/pencil"},
    {"GLUEEDITBOX", QT_TR_NOOP("输入框"), "lucide/pencil"},
    {"SLIDER", QT_TR_NOOP("滑块"), "lucide/sliders-horizontal"},
    {"SCROLLBAR", QT_TR_NOOP("滚动条"), "lucide/grip-vertical"},
    {"POPUPMENU", QT_TR_NOOP("下拉菜单"), "lucide/chevron-down"},
    {"GLUEPOPUPMENU", QT_TR_NOOP("下拉菜单"), "lucide/chevron-down"},
    {"MENU", QT_TR_NOOP("菜单"), "lucide/list-tree"},
    {"LISTBOX", QT_TR_NOOP("列表"), "lucide/list-tree"},
    {"MODEL", QT_TR_NOOP("模型"), "lucide/box"},
    {"SPRITE", QT_TR_NOOP("模型"), "lucide/box"},
    {"HIGHLIGHT", QT_TR_NOOP("高亮"), "lucide/sparkles"},
    {"STATUSBAR", QT_TR_NOOP("进度条"), "lucide/sliders-horizontal"},
    {"SIMPLESTATUSBAR", QT_TR_NOOP("进度条"), "lucide/sliders-horizontal"},
}};

const TypeEntry* FindType(const std::string& type) {
    std::string upper(type);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    const auto found = std::find_if(kTypes.begin(), kTypes.end(), [&](const TypeEntry& entry) { return entry.type == upper; });
    return found == kTypes.end() ? nullptr : &*found;
}

const char* OwnIcon(UiFrameType type) {
    switch (type) {
    case UiFrameType::Panel: return "lucide/square";
    case UiFrameType::Backdrop: return "lucide/image";
    case UiFrameType::Text: return "lucide/type";
    case UiFrameType::Bar: return "lucide/sliders-horizontal";
    case UiFrameType::Button: return "lucide/mouse-pointer-2";
    case UiFrameType::Template: return "lucide/puzzle";
    case UiFrameType::Model: return "lucide/box";
    }
    return "lucide/square";
}

// 节点及其全部子孙（含贴图、文字与层）。
void CollectSubtree(const FrameTree& tree, std::uint32_t node, std::vector<std::uint32_t>& out) {
    out.push_back(node);
    for (const std::uint32_t child : tree.Nodes()[node].children) CollectSubtree(tree, child, out);
}

} // namespace

namespace GameUiNodeTree {

QString FrameKindLabel(UiFrameType type) {
    for (const auto& candidate : UiFrameKinds()) {
        if (candidate.type == type) return Text(std::string(candidate.label));
    }
    return Text(std::string(UiFrameTypeName(type)));
}

QString TypeLabel(const FrameNode& node, const UiFrame* own) {
    if (node.uiFrame && node.uiFrame->frame.empty()) return QObject::tr("界面");
    if (own) return FrameKindLabel(own->type);
    if (!node.codeLabel.empty()) return QObject::tr("游戏代码区域");
    switch (node.kind) {
    case DefinitionKind::Texture: return QObject::tr("贴图");
    case DefinitionKind::String: return QObject::tr("文字");
    case DefinitionKind::Layer: return QObject::tr("层");
    default: break;
    }
    const TypeEntry* entry = FindType(node.frameType);
    return entry ? QObject::tr(entry->label) : TypeText(node);
}

QString TypeIcon(const FrameNode& node, const UiFrame* own) {
    if (node.uiFrame && node.uiFrame->frame.empty()) return QStringLiteral("lucide/layout-dashboard");
    if (own) return QLatin1String(OwnIcon(own->type));
    if (!node.codeLabel.empty()) return QStringLiteral("lucide/code-xml");
    switch (node.kind) {
    case DefinitionKind::Texture: return QStringLiteral("lucide/image");
    case DefinitionKind::String: return QStringLiteral("lucide/type");
    case DefinitionKind::Layer: return QStringLiteral("lucide/layers");
    default: break;
    }
    const TypeEntry* entry = FindType(node.frameType);
    return QLatin1String(entry ? entry->icon : "lucide/square");
}

} // namespace GameUiNodeTree

void GameUiModule::PopulateTree() {
    if (!nodes_) return;
    populating_ = true;
    nodes_->clear();
    ownGroupItem_ = nullptr;
    originGroupItem_ = nullptr;
    renameItem_ = nullptr;
    if (tree_) {
        const std::unordered_set<std::uint32_t> stateOnly(drawList_.stateOnlyNodes.begin(), drawList_.stateOnlyNodes.end());
        const QColor dim = nodes_->palette().color(QPalette::PlaceholderText);
        const bool raw = rawFields_ && rawFields_->isChecked();
        QHash<QString, QIcon> icons;
        const auto icon = [&icons](const QString& name) {
            auto found = icons.find(name);
            if (found == icons.end()) found = icons.insert(name, UiKit::SvgIcon(name));
            return *found;
        };
        // 自己的界面在上，原版界面在下；分组行不能选、不能拖放。
        const auto addGroup = [this](const QString& title, const QString& key) {
            auto* item = new QTreeWidgetItem(nodes_, {title});
            item->setData(kNameColumn, kNodeRole, kNoNode);
            item->setData(kNameColumn, kGroupRole, key);
            item->setFlags(Qt::ItemIsEnabled);
            QFont font = item->font(kNameColumn);
            font.setBold(true);
            item->setFont(kNameColumn, font);
            return item;
        };
        ownGroupItem_ = addGroup(tr("我的界面"), QStringLiteral("own"));
        originGroupItem_ = addGroup(tr("原版界面"), QStringLiteral("origin"));
        std::vector<QTreeWidgetItem*> items(tree_->Nodes().size(), nullptr);
        QTreeWidgetItem* current = nullptr;
        for (const auto& node : tree_->Nodes()) {
            if (node.id == kScreenNode) continue;
            QTreeWidgetItem* parent = node.parent != kScreenNode ? items[node.parent] : OwnNode(node.id) ? ownGroupItem_ : originGroupItem_;
            auto* item = new QTreeWidgetItem(parent);
            const UiDescription* description = node.uiFrame ? Description(Text(node.uiFrame->description)) : nullptr;
            const UiFrame* own = description && !node.uiFrame->frame.empty() ? FindUiFrame(*description, node.uiFrame->frame) : nullptr;
            item->setText(kNameColumn, node.uiFrame ? OwnDisplayName(node, description, own) : DisplayName(node));
            item->setIcon(kNameColumn, icon(TypeIcon(node, own)));
            const QString typeLabel = TypeLabel(node, own);
            item->setData(kTypeColumn, kRawTypeRole, TypeText(node));
            item->setData(kTypeColumn, kTypeLabelRole, typeLabel);
            item->setText(kTypeColumn, raw ? TypeText(node) : typeLabel);
            // 自己的界面可改名（F2）、可放进别的控件；控件可拖动。原版节点不能拖放。
            Qt::ItemFlags flags = item->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            if (node.uiFrame) {
                flags |= Qt::ItemIsEditable | Qt::ItemIsDropEnabled;
                if (!node.uiFrame->frame.empty()) flags |= Qt::ItemIsDragEnabled;
            }
            item->setFlags(flags);
            const bool state = stateOnly.contains(node.id);
            item->setText(kStateColumn, state         ? tr("状态显示")
                                        : node.hidden ? tr("隐藏")
                                        : node.layout == LayoutState::Placed ? QString()
                                                                             : LayoutText(node.layout));
            item->setData(kNameColumn, kNodeRole, node.id);
            item->setData(kNameColumn, kNameRole, Text(node.name));
            if (state || node.hidden || node.layout == LayoutState::Unplaced || node.layout == LayoutState::Cycle) {
                for (const int column : {kNameColumn, kTypeColumn, kStateColumn}) item->setForeground(column, dim);
            }
            items[node.id] = item;
            if (node.id == selected_) current = item;
        }
        for (QTreeWidgetItem* group : {ownGroupItem_, originGroupItem_}) {
            for (int i = 0; i < group->childCount(); ++i) group->child(i)->setExpanded(true);
        }
        ownGroupItem_->setExpanded(true);
        // 原版界面：有自己的界面时默认收起，作者展开或收起后按作者的；选中的节点在里面时展开。
        bool originOpen = originGroupExpanded_.value_or(ownGroupItem_->childCount() == 0);
        for (QTreeWidgetItem* at = current; at && !originOpen; at = at->parent()) originOpen = at == originGroupItem_;
        originGroupItem_->setExpanded(originOpen);
        if (current) nodes_->setCurrentItem(current);
    }
    populating_ = false;
    RefreshNodeToggles();
    SetCurrentDescription(currentDescription_);
    ApplyFilter();
}

void GameUiModule::ApplyFilter() {
    if (!nodes_) return;
    const QString needle = search_ ? search_->text().trimmed() : QString();
    const std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        bool any = false;
        for (int i = 0; i < item->childCount(); ++i) any = visit(item->child(i)) || any;
        const bool match = needle.isEmpty() || item->text(kNameColumn).contains(needle, Qt::CaseInsensitive) ||
            item->data(kTypeColumn, kTypeLabelRole).toString().contains(needle, Qt::CaseInsensitive) ||
            item->data(kTypeColumn, kRawTypeRole).toString().contains(needle, Qt::CaseInsensitive) ||
            item->data(kNameColumn, kNameRole).toString().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!(match || any));
        if (!needle.isEmpty() && any) item->setExpanded(true);
        return match || any;
    };
    for (int i = 0; i < nodes_->topLevelItemCount(); ++i) visit(nodes_->topLevelItem(i));
}

void GameUiModule::ShowOnly(std::uint32_t node) {
    if (!tree_ || node >= tree_->Nodes().size() || node == kScreenNode) return;
    // 同级节点全部隐藏，自身与各级父节点显示；用于游戏里同一时间只显示其中之一的面板。
    for (std::uint32_t current = node; current != kScreenNode && current != kNoNode;) {
        const FrameNode& item = tree_->Nodes()[current];
        hiddenKeys_.remove(NodeKey(current));
        if (current == node) {
            for (const std::uint32_t sibling : tree_->Nodes()[item.parent].children) {
                if (sibling != current) hiddenKeys_.insert(NodeKey(sibling));
            }
        }
        current = item.parent;
    }
    StoreMapView();
    RebuildDrawList();
    PopulateTree();
    StartAssetsJob();
    SelectNode(node);
}

bool GameUiModule::OwnNode(std::uint32_t node) const {
    if (!tree_) return false;
    for (std::uint32_t current = node; current < tree_->Nodes().size() && current != kScreenNode;) {
        const FrameNode& item = tree_->Nodes()[current];
        if (item.uiFrame) return true;
        current = item.parent;
    }
    return false;
}

bool GameUiModule::NodeShown(std::uint32_t node) const {
    if (!tree_ || node >= tree_->Nodes().size()) return false;
    for (std::uint32_t current = node; current < tree_->Nodes().size() && current != kScreenNode;) {
        const FrameNode& item = tree_->Nodes()[current];
        if (hiddenKeys_.contains(NodeKey(current)) || (!showHidden_ && item.hidden)) return false;
        current = item.parent;
    }
    return true;
}

bool GameUiModule::NodeVisible(std::uint32_t node) const {
    return tree_ && node < tree_->Nodes().size() && !hiddenKeys_.contains(NodeKey(node));
}

bool GameUiModule::NodeSelectable(std::uint32_t node) const {
    if (!tree_ || node >= tree_->Nodes().size()) return false;
    if (const auto found = selectOverrides_.constFind(NodeKey(node)); found != selectOverrides_.constEnd()) return *found;
    return OwnNode(node) || originSelectable_;
}

bool GameUiModule::NodePickable(std::uint32_t node) const {
    if (!tree_ || node >= tree_->Nodes().size() || node == kScreenNode) return false;
    const FrameNode& item = tree_->Nodes()[node];
    // 界面的根铺满整个画面：能点中它就再也点不到空白处。
    if (item.uiFrame && item.uiFrame->frame.empty()) return false;
    return NodeSelectable(node) && NodeShown(node);
}

void GameUiModule::SetNodeVisible(std::uint32_t node, bool visible) {
    if (!tree_ || node >= tree_->Nodes().size() || node == kScreenNode) return;
    std::vector<std::uint32_t> subtree;
    CollectSubtree(*tree_, node, subtree);
    for (const std::uint32_t id : subtree) {
        if (visible) hiddenKeys_.remove(NodeKey(id));
        else hiddenKeys_.insert(NodeKey(id));
    }
    ApplyNodeVisibility();
}

void GameUiModule::SetNodeSelectable(std::uint32_t node, bool selectable) {
    if (!tree_ || node >= tree_->Nodes().size() || node == kScreenNode) return;
    std::vector<std::uint32_t> subtree;
    CollectSubtree(*tree_, node, subtree);
    for (const std::uint32_t id : subtree) {
        // 与默认相同时不单独记：之后切换“原版可选”仍对它生效。
        const QString key = NodeKey(id);
        if (selectable == (OwnNode(id) || originSelectable_)) selectOverrides_.remove(key);
        else selectOverrides_.insert(key, selectable);
    }
    StoreMapView();
    UpdatePickable();
    RefreshNodeToggles();
}

void GameUiModule::SetOriginSelectable(bool selectable) {
    originSelectable_ = selectable;
    if (originSelectableButton_ && originSelectableButton_->isChecked() != selectable) {
        const QSignalBlocker block(originSelectableButton_);
        originSelectableButton_->setChecked(selectable);
    }
    // 原版节点各自的设置作废，整体按新的默认。
    if (tree_) {
        for (const auto& node : tree_->Nodes()) {
            if (node.id != kScreenNode && !OwnNode(node.id)) selectOverrides_.remove(NodeKey(node.id));
        }
    }
    StoreMapView();
    UpdatePickable();
    RefreshNodeToggles();
}

void GameUiModule::ApplyNodeVisibility() {
    StoreMapView();
    RebuildDrawList();
    RefreshNodeToggles();
    StartAssetsJob();
}

void GameUiModule::UpdatePickable() {
    if (!canvas_) return;
    std::vector<bool> pickable(tree_ ? tree_->Nodes().size() : 0, false);
    for (std::size_t i = 0; i < pickable.size(); ++i) pickable[i] = NodePickable(static_cast<std::uint32_t>(i));
    canvas_->SetPickable(std::move(pickable));
}

void GameUiModule::RefreshNodeToggles() {
    if (!nodes_ || !tree_) return;
    const QSignalBlocker block(nodes_);
    const QSize size(16, 16);
    const QIcon eye = UiKit::SvgIcon(QStringLiteral("lucide/eye"));
    const QIcon eyeOff = UiKit::SvgIcon(QStringLiteral("lucide/eye-off"));
    // 自身可见、但父节点不可见：照样画不出来，图标变淡。
    const QIcon eyeInherited(eye.pixmap(size, QIcon::Disabled));
    const QIcon pointer = UiKit::SvgIcon(QStringLiteral("lucide/mouse-pointer-2"));
    const QIcon pointerOff(pointer.pixmap(size, QIcon::Disabled));
    for (QTreeWidgetItemIterator it(nodes_); *it; ++it) {
        QTreeWidgetItem* item = *it;
        const auto node = item->data(kNameColumn, kNodeRole).toUInt();
        if (node >= tree_->Nodes().size()) continue;
        const bool visible = NodeVisible(node);
        bool parentShown = true;
        for (QTreeWidgetItem* parent = item->parent(); parent && parentShown; parent = parent->parent()) {
            const auto parentNode = parent->data(kNameColumn, kNodeRole).toUInt();
            if (parentNode < tree_->Nodes().size()) parentShown = NodeVisible(parentNode);
        }
        item->setIcon(kVisibleColumn, !visible ? eyeOff : parentShown ? eye : eyeInherited);
        item->setToolTip(kVisibleColumn, visible ? tr("可见") : tr("不可见"));
        const bool selectable = NodeSelectable(node);
        item->setIcon(kSelectColumn, selectable ? pointer : pointerOff);
        item->setToolTip(kSelectColumn, selectable ? tr("可选") : tr("不可选"));
    }
}

bool GameUiModule::eventFilter(QObject* watched, QEvent* event) {
    if (nodes_ && watched == nodes_->viewport()) {
        const QEvent::Type type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QModelIndex index = nodes_->indexAt(mouse->position().toPoint());
            if (index.isValid() && (index.column() == kVisibleColumn || index.column() == kSelectColumn)) {
                if (type == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
                    if (QTreeWidgetItem* item = nodes_->itemFromIndex(index)) {
                        const auto node = item->data(kNameColumn, kNodeRole).toUInt();
                        if (tree_ && node < tree_->Nodes().size()) {
                            if (index.column() == kVisibleColumn) SetNodeVisible(node, !NodeVisible(node));
                            else SetNodeSelectable(node, !NodeSelectable(node));
                        }
                    }
                }
                return true;  // 不改选中行
            }
        }
        // 拖放：节点树自己判断放到哪里，交给界面修改；不让视图自己挪行。
        if ((type == QEvent::DragMove || type == QEvent::Drop) && tree_) {
            auto* drop = static_cast<QDropEvent*>(event);
            QTreeWidgetItem* target = nodes_->itemAt(drop->position().toPoint());
            QTreeWidgetItem* dragged = nodes_->currentItem();
            const int where = target ? DropPosition(target, drop->position().toPoint()) : 0;
            const auto draggedNode = dragged ? dragged->data(kNameColumn, kNodeRole).toUInt() : kNoNode;
            const auto targetNode = target ? target->data(kNameColumn, kNodeRole).toUInt() : kNoNode;
            const bool ok = drop->source() == nodes_ && CanDropOnNodeTree(draggedNode, targetNode, where);
            if (type == QEvent::DragMove) {
                if (ok) return QObject::eventFilter(watched, event);
                drop->ignore();
                return true;
            }
            drop->setDropAction(Qt::IgnoreAction);
            drop->accept();
            if (ok) {
                // 放下后界面重建会清空节点树，等拖放结束再改。
                QTimer::singleShot(0, this, [this, draggedNode, targetNode, where] {
                    QString error;
                    if (!DropOnNodeTree(draggedNode, targetNode, where, &error) && !error.isEmpty()) ReportAuthoring(error);
                });
            }
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

int GameUiModule::DropPosition(QTreeWidgetItem* target, const QPoint& position) const {
    // 与视图画出的落点一致：上下边缘为放在前后，中间为放进去（不能放进去的按上下半分）。
    const QRect rect = nodes_->visualItemRect(target);
    const int margin = std::clamp(qRound(rect.height() / 5.5), 2, 12);
    if (position.y() - rect.top() < margin) return -1;
    if (rect.bottom() - position.y() < margin) return 1;
    if (target->flags() & Qt::ItemIsDropEnabled) return 0;
    return position.y() < rect.center().y() ? -1 : 1;
}

bool GameUiModule::CanDropOnNodeTree(std::uint32_t dragged, std::uint32_t target, int where) const {
    if (!tree_ || dragged >= tree_->Nodes().size() || target >= tree_->Nodes().size() || dragged == target) return false;
    const auto& nodes = tree_->Nodes();
    const auto& from = nodes[dragged].uiFrame;
    const auto& to = nodes[target].uiFrame;
    if (!from || from->frame.empty() || !to || to->description != from->description) return false;
    if (where != 0 && to->frame.empty()) return false;  // 界面本身的前后不是控件的位置
    // 不能放进自己或自己的子控件。
    for (std::uint32_t at = target; at < nodes.size() && at != kScreenNode; at = nodes[at].parent) {
        if (nodes[at].uiFrame && nodes[at].uiFrame->frame == from->frame && nodes[at].uiFrame->description == from->description) return false;
    }
    return true;
}

bool GameUiModule::DropOnNodeTree(std::uint32_t dragged, std::uint32_t target, int where, QString* error) {
    if (!CanDropOnNodeTree(dragged, target, where)) {
        if (error) *error = tr("只能在同一份界面里移动控件，且不能放进自己或自己的子控件");
        return false;
    }
    const auto& from = *tree_->Nodes()[dragged].uiFrame;
    const auto& to = *tree_->Nodes()[target].uiFrame;
    const QString descriptionId = Text(from.description);
    const UiDescription* description = Description(descriptionId);
    if (!description) return false;
    if (where == 0) return PlaceDescriptionFrame(descriptionId, Text(from.frame), Text(to.frame), std::nullopt, error);
    // 放在目标前后：在目标的父控件下，按去掉自己之后的顺序算位置。
    const std::string parentId = UiFrameParentId(*description, to.frame);
    const UiFrame* parent = parentId.empty() ? nullptr : FindUiFrame(*description, parentId);
    const auto& siblings = parent ? parent->children : description->frames;
    std::size_t index = 0;
    for (const UiFrame& sibling : siblings) {
        if (sibling.id == from.frame) continue;
        if (sibling.id == to.frame) break;
        ++index;
    }
    if (where > 0) ++index;
    return PlaceDescriptionFrame(descriptionId, Text(from.frame), Text(parentId), index, error);
}

void GameUiModule::RenameNodeItem(std::uint32_t node) {
    if (!nodes_ || !tree_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame) return;
    QTreeWidgetItem* item = nullptr;
    for (QTreeWidgetItemIterator it(nodes_); *it && !item; ++it) {
        if ((*it)->data(kNameColumn, kNodeRole).toUInt() == node) item = *it;
    }
    if (!item) return;
    // 编辑框里只放显示名（没有时放编号），改完按显示名保存。
    const auto& link = *tree_->Nodes()[node].uiFrame;
    const UiDescription* description = Description(Text(link.description));
    const UiFrame* frame = description && !link.frame.empty() ? FindUiFrame(*description, link.frame) : nullptr;
    const std::string label = !link.frame.empty() ? (frame ? frame->label : std::string()) : description ? description->label : std::string();
    populating_ = true;
    item->setText(kNameColumn, label.empty() ? Text(link.frame.empty() ? link.description : link.frame) : Text(label));
    populating_ = false;
    renameItem_ = item;
    renameDisplay_ = OwnDisplayName(tree_->Nodes()[node], description, frame);
    nodes_->scrollToItem(item);
    nodes_->editItem(item, kNameColumn);
}

void GameUiModule::OnNodeItemChanged(QTreeWidgetItem* item, int column) {
    if (populating_ || column != kNameColumn || !item || item != renameItem_ || !tree_) return;
    renameItem_ = nullptr;
    const auto node = item->data(kNameColumn, kNodeRole).toUInt();
    if (node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame) return;
    const auto& link = *tree_->Nodes()[node].uiFrame;
    const QString description = Text(link.description);
    const QString frame = Text(link.frame);
    const QString label = item->text(kNameColumn).trimmed();
    // 编辑器还在提交，改名后界面重建会清空节点树，放到下一轮。
    QTimer::singleShot(0, this, [this, description, frame, label] {
        QString error;
        if (!RenameDescriptionNode(description, frame, label, &error)) ReportAuthoring(error);
        PopulateTree();
    });
}

void GameUiModule::OnNodeEditorClosed() {
    // 没改就关掉编辑框：恢复“显示名（编号）”。
    if (!renameItem_) return;
    populating_ = true;
    renameItem_->setText(kNameColumn, renameDisplay_);
    populating_ = false;
    renameItem_ = nullptr;
}

void GameUiModule::RefreshTypeColumn() {
    if (!nodes_) return;
    const bool raw = rawFields_ && rawFields_->isChecked();
    const QSignalBlocker block(nodes_);
    for (QTreeWidgetItemIterator it(nodes_); *it; ++it) {
        const QVariant text = (*it)->data(kTypeColumn, raw ? kRawTypeRole : kTypeLabelRole);
        if (text.isValid()) (*it)->setText(kTypeColumn, text.toString());
    }
}

void GameUiModule::SyncMapView() {
    const auto path = project_.AuthorMapPath();
    const QString key = path.empty() ? QString() : QDir::cleanPath(QString::fromStdWString(path.wstring())).toLower();
    if (key == mapViewKey_) return;
    StoreMapView();
    mapViewKey_ = key;
    hiddenKeys_.clear();
    selectOverrides_.clear();
    bool origin = false;
    for (const auto& value : mapViews_) {
        const QJsonObject entry = value.toObject();
        if (key.isEmpty() || entry.value(QStringLiteral("map")).toString() != key) continue;
        for (const auto& hidden : entry.value(QStringLiteral("hidden")).toArray()) hiddenKeys_.insert(hidden.toString());
        const QJsonObject select = entry.value(QStringLiteral("select")).toObject();
        for (auto it = select.constBegin(); it != select.constEnd(); ++it) selectOverrides_.insert(it.key(), it.value().toBool());
        origin = entry.value(QStringLiteral("originSelectable")).toBool();
        break;
    }
    originSelectable_ = origin;
    if (originSelectableButton_) {
        const QSignalBlocker block(originSelectableButton_);
        originSelectableButton_->setChecked(origin);
    }
}

void GameUiModule::StoreMapView() {
    if (mapViewKey_.isEmpty()) return;
    for (qsizetype i = mapViews_.size() - 1; i >= 0; --i) {
        if (mapViews_[i].toObject().value(QStringLiteral("map")).toString() == mapViewKey_) mapViews_.removeAt(i);
    }
    QJsonObject entry{{QStringLiteral("map"), mapViewKey_}};
    if (!hiddenKeys_.isEmpty()) {
        QStringList hidden(hiddenKeys_.begin(), hiddenKeys_.end());
        hidden.sort();
        entry.insert(QStringLiteral("hidden"), QJsonArray::fromStringList(hidden));
    }
    if (!selectOverrides_.isEmpty()) {
        QJsonObject select;
        for (auto it = selectOverrides_.constBegin(); it != selectOverrides_.constEnd(); ++it) select.insert(it.key(), it.value());
        entry.insert(QStringLiteral("select"), select);
    }
    if (originSelectable_) entry.insert(QStringLiteral("originSelectable"), true);
    if (entry.size() > 1) mapViews_.prepend(entry);
    while (mapViews_.size() > kMapViewLimit) mapViews_.removeLast();
}

QString GameUiModule::CurrentDescription() const {
    if (!definitions_ || definitions_->descriptions.empty()) return {};
    for (const auto& file : definitions_->descriptions) {
        if (Text(file.description.id) == currentDescription_) return currentDescription_;
    }
    return Text(definitions_->descriptions.front().description.id);
}

// 当前界面在节点树里加粗。
void GameUiModule::SetCurrentDescription(const QString& id) {
    if (!id.isEmpty()) currentDescription_ = id;
    if (!nodes_ || !tree_) return;
    const QString current = CurrentDescription();
    const QSignalBlocker block(nodes_);
    if (!ownGroupItem_) return;
    for (int i = 0; i < ownGroupItem_->childCount(); ++i) {
        QTreeWidgetItem* item = ownGroupItem_->child(i);
        const auto node = item->data(kNameColumn, kNodeRole).toUInt();
        if (node >= tree_->Nodes().size()) continue;
        const auto& link = tree_->Nodes()[node].uiFrame;
        QFont font = item->font(kNameColumn);
        font.setBold(link && link->frame.empty() && Text(link->description) == current);
        item->setFont(kNameColumn, font);
    }
}

} // namespace War3Map::Modules
