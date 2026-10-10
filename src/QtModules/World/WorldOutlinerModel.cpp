#include "WorldOutlinerModel.h"

#include <QStringList>

#include <algorithm>
#include <array>

namespace War3Map::Modules {
namespace {
constexpr std::array<MapDataKind, 4> kSources{MapDataKind::Units, MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras};
constexpr std::size_t kMaxRows = 4096;
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
// 仅路由确定适用的物理成员；对象类型及筛选结果仍由共用服务确认。
std::size_t NextSource(std::size_t index, std::optional<Services::WorldCategory> category) {
    if (!category) return index;
    std::optional<MapDataKind> required;
    switch (*category) {
    case Services::WorldCategory::Unit: case Services::WorldCategory::Item:
    case Services::WorldCategory::StartLocation: required = MapDataKind::Units; break;
    case Services::WorldCategory::Destructable: case Services::WorldCategory::Doodad:
        required = MapDataKind::Doodads; break;
    case Services::WorldCategory::Region: required = MapDataKind::Regions; break;
    case Services::WorldCategory::Camera: required = MapDataKind::Cameras; break;
    default: return index; // 未确认类别保留所有可能来源。
    }
    while (index < kSources.size() && kSources[index] != *required) ++index;
    return index;
}
}

struct WorldOutlinerModel::Node {
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    QString title;
    std::optional<Services::WorldCategory> category;
    std::optional<std::int32_t> owner;
    std::optional<Services::WorldObjectRow> row;
};

WorldOutlinerModel::WorldOutlinerModel(QObject* parent) : QAbstractItemModel(parent), root_(std::make_unique<Node>()) {}
WorldOutlinerModel::~WorldOutlinerModel() = default;
void WorldOutlinerModel::ResetRows() {
    root_ = std::make_unique<Node>(); cursor_.reset(); version_.reset();
    source_ = NextSource(0, filter_.category); loaded_ = 0; partial_ = fetching_ = false; status_.clear();
}
void WorldOutlinerModel::SetSource(PageReader reader, FlagWriter writer) {
    beginResetModel(); reader_ = std::move(reader); writer_ = std::move(writer); diagnosticReader_ = {}; ResetRows(); endResetModel();
    if (reader_) fetchMore({});
}
void WorldOutlinerModel::SetDiagnosticReader(DiagnosticReader reader) {
    diagnosticReader_ = std::move(reader);
}
void WorldOutlinerModel::SetFilter(Services::WorldObjectFilter filter) {
    if (filter == filter_) return;
    filter_ = std::move(filter); Refresh();
}
void WorldOutlinerModel::Refresh() {
    beginResetModel(); ResetRows(); endResetModel();
    if (reader_) fetchMore({});
}
WorldOutlinerModel::Node* WorldOutlinerModel::At(const QModelIndex& index) const {
    if (!index.isValid()) return root_.get();
    return index.model() == this ? static_cast<Node*>(index.internalPointer()) : nullptr;
}
QModelIndex WorldOutlinerModel::IndexOf(const Node* node, int column) const {
    if (!node || !node->parent || column < 0 || column >= 3) return {};
    const auto& siblings = node->parent->children;
    const auto found = std::find_if(siblings.begin(), siblings.end(), [&](const auto& child) { return child.get() == node; });
    if (found == siblings.end()) return {};
    return createIndex(static_cast<int>(found - siblings.begin()), column, const_cast<Node*>(node));
}
int WorldOutlinerModel::rowCount(const QModelIndex& parent) const {
    const auto* node = At(parent);
    return !node || (parent.isValid() && parent.column() != 0) ? 0 : static_cast<int>(node->children.size());
}
int WorldOutlinerModel::columnCount(const QModelIndex&) const { return 3; }
QModelIndex WorldOutlinerModel::index(int row, int column, const QModelIndex& parent) const {
    const auto* node = At(parent);
    if (!node || row < 0 || column < 0 || column >= 3 || (parent.isValid() && parent.column() != 0)
        || static_cast<std::size_t>(row) >= node->children.size()) return {};
    return createIndex(row, column, node->children[static_cast<std::size_t>(row)].get());
}
QModelIndex WorldOutlinerModel::parent(const QModelIndex& index) const {
    const auto* node = index.isValid() ? At(index) : nullptr;
    return node ? IndexOf(node->parent) : QModelIndex{};
}
const Services::WorldObjectRow* WorldOutlinerModel::RowAt(const QModelIndex& index) const {
    const auto* node = index.isValid() ? At(index) : nullptr;
    return node && node->row ? &*node->row : nullptr;
}
QVariant WorldOutlinerModel::data(const QModelIndex& index, int role) const {
    const auto* node = index.isValid() ? At(index) : nullptr;
    if (!node) return {};
    if (role == Qt::DisplayRole && index.column() == 0) {
        if (!node->row) return node->title;
        const auto& row = *node->row;
        const auto& name = row.name.displayBytes ? *row.name.displayBytes : row.name.rawBytes;
        return name.empty() ? tr("记录 %1").arg(static_cast<qulonglong>(row.sourceIndex)) : Text(name);
    }
    if (role == Qt::ToolTipRole) {
        if (!node->row) return tr("当前已加载 %1 项；显隐与锁定只作用于编辑器。").arg(static_cast<qulonglong>(node->children.size()));
        const auto& row = *node->row;
        QStringList parts{Text(row.rowKey), Text(Services::WorldCategoryName(row.category))};
        if (!row.reason.empty()) parts << Text(row.reason);
        if (!row.flags.visible) parts << tr("已隐藏，不参与视口显示或拾取");
        if (row.flags.locked) parts << tr("已锁定，不参与编辑选择");
        if (diagnosticReader_ && version_ && row.id) {
            auto diagnostic = diagnosticReader_(*version_, *row.id);
            if (!diagnostic.isEmpty()) parts << std::move(diagnostic);
        }
        return QStringLiteral("<qt>") + parts.join(QLatin1Char('\n')).toHtmlEscaped().replace(
            QLatin1Char('\n'), QStringLiteral("<br/>")) + QStringLiteral("</qt>");
    }
    if (role == Qt::CheckStateRole && node->row && node->row->id) {
        if (index.column() == 1) return node->row->flags.visible ? Qt::Checked : Qt::Unchecked;
        if (index.column() == 2) return node->row->flags.locked ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::UserRole && node->row) return Text(node->row->rowKey);
    if (role == Qt::TextAlignmentRole && index.column() != 0) return static_cast<int>(Qt::AlignCenter);
    return {};
}
QVariant WorldOutlinerModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    if (section == 0) return tr("场景对象");
    if (section == 1) return tr("显示");
    if (section == 2) return tr("锁定");
    return {};
}
Qt::ItemFlags WorldOutlinerModel::flags(const QModelIndex& index) const {
    const auto* row = RowAt(index);
    if (!index.isValid()) return Qt::NoItemFlags;
    auto result = Qt::ItemFlags(Qt::ItemIsEnabled);
    if (row && row->id) {
        if (row->flags.visible && !row->flags.locked) result |= Qt::ItemIsSelectable;
        if (writer_ && (index.column() == 1 || index.column() == 2)) result |= Qt::ItemIsUserCheckable;
    }
    return result;
}
bool WorldOutlinerModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    auto* node = index.isValid() ? At(index) : nullptr;
    if (!node || !node->row || !node->row->id || !version_ || !writer_ || role != Qt::CheckStateRole
        || (index.column() != 1 && index.column() != 2)) return false;
    const auto state = value.toInt(); if (state != Qt::Checked && state != Qt::Unchecked) return false;
    auto flags = node->row->flags;
    if (index.column() == 1) flags.visible = state == Qt::Checked; else flags.locked = state == Qt::Checked;
    const Services::WorldEditorFlagEdit edit{*node->row->id, flags};
    const auto result = writer_(*version_, std::span(&edit, 1));
    if (!result.Succeeded()) {
        partial_ = true;
        status_ = result.diagnostics.empty() ? tr("编辑器状态修改被拒绝") : Text(result.diagnostics.front().message);
        return false;
    }
    // writer 只提交核心状态；宿主不要在它返回前同步重置此模型，避免使当前 QModelIndex 失效。
    if (!ApplySnapshot(*result.value)) { partial_ = true; status_ = tr("编辑器状态结果已经过期，请刷新目录"); return false; }
    node->row->flags = flags;
    emit dataChanged(IndexOf(node, 0), IndexOf(node, 2));
    return true;
}
bool WorldOutlinerModel::ApplySnapshot(const Services::WorldSelectionSnapshot& snapshot) {
    if (!version_ || snapshot.version.document != version_->document
        || snapshot.version.catalogRevision != version_->catalogRevision
        || snapshot.version.editorRevision < version_->editorRevision) return false;
    version_ = snapshot.version;
    if (cursor_) cursor_->version = *version_;
    const auto update = [&](auto&& self, Node* node) -> void {
        if (node->row && node->row->id) {
            auto& row = *node->row;
            row.selected = std::find(snapshot.selected.begin(), snapshot.selected.end(), *row.id) != snapshot.selected.end();
            row.primary = snapshot.primary == row.id;
        }
        for (auto& child : node->children) self(self, child.get());
    };
    update(update, root_.get()); return true;
}
WorldOutlinerModel::Node* WorldOutlinerModel::Group(Services::WorldCategory category, std::optional<std::int32_t> owner) {
    const auto make = [&](Node* parent, QString title, std::optional<std::int32_t> player) {
        const auto i = static_cast<int>(parent->children.size());
        beginInsertRows(IndexOf(parent), i, i);
        auto child = std::make_unique<Node>(); child->parent = parent; child->title = std::move(title);
        child->category = category; child->owner = player;
        auto* raw = child.get(); parent->children.push_back(std::move(child)); endInsertRows(); return raw;
    };
    auto found = std::find_if(root_->children.begin(), root_->children.end(), [&](const auto& n) { return n->category == category; });
    Node* group = found == root_->children.end() ? make(root_.get(), Text(Services::WorldCategoryName(category)), std::nullopt) : found->get();
    if (category != Services::WorldCategory::Unit) return group;
    const auto player = std::find_if(group->children.begin(), group->children.end(), [&](const auto& n) { return n->owner == owner; });
    return player == group->children.end() ? make(group, owner ? tr("玩家 %1").arg(*owner) : tr("所属玩家未确定"), owner) : player->get();
}
bool WorldOutlinerModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && reader_ && !fetching_ && source_ < kSources.size() && loaded_ < kMaxRows;
}
void WorldOutlinerModel::fetchMore(const QModelIndex& parent) {
    if (!canFetchMore(parent)) return;
    fetching_ = true;
    const Services::WorldObjectQueryOptions options{(std::min)(std::size_t{128}, kMaxRows - loaded_), 4096, 256 * 1024};
    const auto page = reader_(kSources[source_], filter_, cursor_, options);
    if (!page.Succeeded()) {
        partial_ = true; status_ = page.diagnostics.empty() ? tr("地图对象查询失败") : Text(page.diagnostics.front().message);
        source_ = kSources.size(); cursor_.reset(); fetching_ = false; return;
    }
    if ((version_ && page.value->version != *version_) || page.value->member.kind != kSources[source_]
        || page.value->rows.size() > options.limit || page.value->logicalBytes > options.maxLogicalBytes
        || (page.value->next && (page.value->next->version != page.value->version
            || page.value->next->source.binding != page.value->version.document
            || page.value->next->source.kind != kSources[source_] || page.value->next->filter != filter_
            || page.value->next->source.nextIndex <= (cursor_ ? cursor_->source.nextIndex : 0)))) {
        partial_ = true; status_ = tr("工程或编辑器状态已改变，目录页已拒绝");
        source_ = kSources.size(); cursor_.reset(); fetching_ = false; return;
    }
    version_ = page.value->version;
    partial_ = partial_ || page.value->partial;
    if (!page.value->reason.empty()) {
        const auto reason = Text(page.value->member.memberName) + QStringLiteral("：") + Text(page.value->reason);
        if (!status_.contains(reason) && status_.size() < 2048)
            status_ += (status_.isEmpty() ? QString{} : QStringLiteral("；")) + reason;
    }
    for (const auto& row : page.value->rows) {
        auto* group = Group(row.category, row.owner);
        const auto i = static_cast<int>(group->children.size());
        beginInsertRows(IndexOf(group), i, i);
        auto child = std::make_unique<Node>(); child->parent = group; child->row = row;
        group->children.push_back(std::move(child)); ++loaded_; endInsertRows();
    }
    cursor_ = page.value->next;
    if (!cursor_) source_ = NextSource(source_ + 1, filter_.category);
    if (loaded_ == kMaxRows && source_ < kSources.size()) { partial_ = true; status_ = tr("目录已加载4096项，请按类别或名称筛选继续查询"); }
    fetching_ = false;
}
QModelIndex WorldOutlinerModel::Find(const Services::WorldObjectId& id) const {
    const auto find = [&](auto&& self, const Node* node) -> const Node* {
        if (node->row && node->row->id == std::optional(id)) return node;
        for (const auto& child : node->children) if (const auto* result = self(self, child.get())) return result;
        return nullptr;
    };
    return IndexOf(find(find, root_.get()));
}
QString WorldOutlinerModel::StatusText() const {
    auto text = tr("已加载 %1 项").arg(static_cast<qulonglong>(loaded_));
    if (partial_) text += tr(" · 部分结果");
    else if (source_ < kSources.size()) text += tr(" · 可继续加载");
    if (!status_.isEmpty()) text += QStringLiteral(" · ") + status_;
    return text;
}

} // namespace War3Map::Modules
