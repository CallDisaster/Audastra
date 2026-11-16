#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "GameUiNodeTree.h"
#include "Icons/SvgIcons.h"

#include <QDir>
#include <QEvent>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <functional>
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

// 节点及其全部子孙（含贴图、文字与层）。
void CollectSubtree(const FrameTree& tree, std::uint32_t node, std::vector<std::uint32_t>& out) {
    out.push_back(node);
    for (const std::uint32_t child : tree.Nodes()[node].children) CollectSubtree(tree, child, out);
}

} // namespace

void GameUiModule::PopulateTree() {
    if (!nodes_) return;
    populating_ = true;
    nodes_->clear();
    if (tree_) {
        const std::unordered_set<std::uint32_t> stateOnly(drawList_.stateOnlyNodes.begin(), drawList_.stateOnlyNodes.end());
        const QColor dim = nodes_->palette().color(QPalette::PlaceholderText);
        std::vector<QTreeWidgetItem*> items(tree_->Nodes().size(), nullptr);
        for (const auto& node : tree_->Nodes()) {
            if (node.id == kScreenNode) continue;
            QTreeWidgetItem* parent = node.parent == kScreenNode ? nullptr : items[node.parent];
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(nodes_);
            item->setText(kNameColumn, DisplayName(node));
            item->setText(kTypeColumn, TypeText(node));
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
            if (node.id == selected_) nodes_->setCurrentItem(item);
        }
        for (int i = 0; i < nodes_->topLevelItemCount(); ++i) nodes_->topLevelItem(i)->setExpanded(true);
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
            item->text(kTypeColumn).contains(needle, Qt::CaseInsensitive) ||
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
            parentShown = NodeVisible(parent->data(kNameColumn, kNodeRole).toUInt());
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
                        if (index.column() == kVisibleColumn) SetNodeVisible(node, !NodeVisible(node));
                        else SetNodeSelectable(node, !NodeSelectable(node));
                    }
                }
                return true;  // 不改选中行
            }
        }
    }
    return QObject::eventFilter(watched, event);
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
    for (int i = 0; i < nodes_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = nodes_->topLevelItem(i);
        const auto node = item->data(kNameColumn, kNodeRole).toUInt();
        if (node >= tree_->Nodes().size()) continue;
        const auto& link = tree_->Nodes()[node].uiFrame;
        QFont font = item->font(kNameColumn);
        font.setBold(link && link->frame.empty() && Text(link->description) == current);
        item->setFont(kNameColumn, font);
    }
}

} // namespace War3Map::Modules
