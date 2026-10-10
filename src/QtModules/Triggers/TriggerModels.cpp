#include "TriggerModels.h"
#include "TriggerModelsDetail.h"

#include "Icons/IconProvider.h"
#include "Icons/SvgIcons.h"

#include <QGuiApplication>
#include <QMimeData>
#include <QPalette>

using namespace TriggerModelsDetail;

// 触发器目录模型（类别 / 触发器树）：名称列可就地重命名，状态列显示“禁用”“关”，支持拖放排序。
namespace {

constexpr auto kDirectoryMime = "application/x-war3-trigger-directory";

// 纯模型测试只有 QCoreApplication：此时不提供图标与调色板颜色。
bool HasGui() {
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

} // namespace

TriggerExplorerModel::TriggerExplorerModel(QObject* parent)
    : QAbstractItemModel(parent) {
}

void TriggerExplorerModel::ComputePartial() {
    // 含禁用语句：遍历各触发器的语句子树（十万级节点约 1 ms）。
    partial_.assign(document_ ? document_->triggers.size() : 0, 0);
    if (!document_) return;
    std::vector<std::uint32_t> pending;
    for (std::size_t index = 0; index < document_->triggers.size(); ++index) {
        pending.assign(document_->triggers[index].ecas.begin(), document_->triggers[index].ecas.end());
        std::size_t guard = 0;
        while (!pending.empty() && ++guard <= document_->ecaNodes.size()) {
            const std::uint32_t node = pending.back();
            pending.pop_back();
            if (node >= document_->ecaNodes.size()) continue;
            const auto& eca = document_->ecaNodes[node];
            if (eca.enabled == 0) {
                partial_[index] = 1;
                break;
            }
            pending.insert(pending.end(), eca.children.begin(), eca.children.end());
        }
    }
}

bool TriggerExplorerModel::RefreshDocument(
    const War3Map::Trigger::TriggerDocument* document) {
    if (document != nullptr && document == document_
        && document->categories.size() == structureCategories_.size()
        && document->triggers.size() == structureTriggers_.size()) {
        bool same = true;
        for (std::size_t index = 0; same && index < structureCategories_.size(); ++index)
            same = document->categories[index].categoryId == structureCategories_[index];
        for (std::size_t index = 0; same && index < structureTriggers_.size(); ++index)
            same = document->triggers[index].id == structureTriggers_[index].first
                && document->triggers[index].categoryId == structureTriggers_[index].second;
        if (same) {
            // 结构未变：分组与行号仍然有效，只通知名称、图标或状态真正变化的行。
            // 整树 dataChanged 会让筛选代理逐行重新过滤，大图上比重置还慢。
            ComputePartial();
            const int lastColumn = columnCount() - 1;
            for (std::size_t group = 0; group < groups_.size(); ++group) {
                const auto& entry = groups_[group];
                if (!entry.synthetic) {
                    const auto& name = document_->categories[entry.sourceCategoryIndex].name.text;
                    if (shownCategoryNames_[entry.sourceCategoryIndex] != name) {
                        shownCategoryNames_[entry.sourceCategoryIndex] = name;
                        emit dataChanged(index(static_cast<int>(group), 0), index(static_cast<int>(group), lastColumn));
                    }
                }
                for (std::size_t row = 0; row < entry.triggerIndices.size(); ++row) {
                    const std::size_t triggerIndex = entry.triggerIndices[row];
                    auto shown = Shown(triggerIndex);
                    if (shown == shownTriggers_[triggerIndex]) continue;
                    shownTriggers_[triggerIndex] = std::move(shown);
                    const auto parentIndex = index(static_cast<int>(group), 0);
                    emit dataChanged(index(static_cast<int>(row), 0, parentIndex),
                        index(static_cast<int>(row), lastColumn, parentIndex));
                }
            }
            return false;
        }
    }
    SetDocument(document);
    return true;
}

void TriggerExplorerModel::SetDocument(
    const War3Map::Trigger::TriggerDocument* document) {
    beginResetModel();
    document_ = document;
    groups_.clear();
    triggerGroupRows_.clear();
    structureCategories_.clear();
    structureTriggers_.clear();
    if (document_ != nullptr) {
        groups_.reserve(document_->categories.size() + 1);
        std::unordered_map<std::int32_t, std::size_t> categoryRows;
        for (std::size_t index = 0; index < document_->categories.size(); ++index) {
            const auto& category = document_->categories[index];
            categoryRows.emplace(category.categoryId, groups_.size());
            groups_.push_back(CategoryGroup{index, category.categoryId, false, {}});
            structureCategories_.push_back(category.categoryId);
        }

        triggerGroupRows_.assign(
            document_->triggers.size(),
            std::numeric_limits<std::size_t>::max());
        std::size_t orphanGroup = std::numeric_limits<std::size_t>::max();
        for (std::size_t index = 0; index < document_->triggers.size(); ++index) {
            structureTriggers_.emplace_back(document_->triggers[index].id, document_->triggers[index].categoryId);
            const auto category = categoryRows.find(document_->triggers[index].categoryId);
            if (category != categoryRows.end()) {
                groups_[category->second].triggerIndices.push_back(index);
                triggerGroupRows_[index] = category->second;
                continue;
            }
            if (orphanGroup == std::numeric_limits<std::size_t>::max()) {
                orphanGroup = groups_.size();
                groups_.push_back(CategoryGroup{0, 0, true, {}});
            }
            groups_[orphanGroup].triggerIndices.push_back(index);
            triggerGroupRows_[index] = orphanGroup;
        }
    }
    ComputePartial();
    shownCategoryNames_.clear();
    shownTriggers_.clear();
    if (document_ != nullptr) {
        for (const auto& category : document_->categories) shownCategoryNames_.push_back(category.name.text);
        for (std::size_t index = 0; index < document_->triggers.size(); ++index) shownTriggers_.push_back(Shown(index));
    }
    endResetModel();
}

TriggerExplorerModel::ShownTrigger TriggerExplorerModel::Shown(std::size_t triggerIndex) const {
    const auto& trigger = document_->triggers[triggerIndex];
    return {trigger.name.text, trigger.type, trigger.enabled, trigger.initiallyClosed, trigger.customTextFlag,
        triggerIndex < partial_.size() ? partial_[triggerIndex] : char{0}};
}
void TriggerExplorerModel::SetExpandedCategories(std::set<std::int32_t> expanded) {
    expanded_ = std::move(expanded);
    if (!groups_.empty()) {
        emit dataChanged(index(0, 0), index(static_cast<int>(groups_.size()) - 1, 0), {Qt::DecorationRole});
    }
}

const War3Map::Trigger::TriggerRecord* TriggerExplorerModel::TriggerAt(
    const QModelIndex& index) const noexcept {
    if (document_ == nullptr || !IsTriggerIndex(index)) {
        return nullptr;
    }
    const std::size_t triggerIndex = DecodedIndex(index);
    return triggerIndex < document_->triggers.size()
        ? &document_->triggers[triggerIndex]
        : nullptr;
}

std::optional<std::int32_t> TriggerExplorerModel::CategoryIdAt(
    const QModelIndex& index) const noexcept {
    if (document_ == nullptr || !index.isValid() || IsTriggerIndex(index)) {
        return std::nullopt;
    }
    const std::size_t groupIndex = DecodedIndex(index);
    if (groupIndex >= groups_.size() || groups_[groupIndex].synthetic) {
        return std::nullopt;
    }
    return groups_[groupIndex].categoryId;
}

std::optional<TriggerDirectoryItem> TriggerExplorerModel::ItemAt(const QModelIndex& index) const noexcept {
    if (const auto* trigger = TriggerAt(index)) {
        return TriggerDirectoryItem{false, trigger->id, trigger->categoryId};
    }
    if (const auto categoryId = CategoryIdAt(index)) {
        return TriggerDirectoryItem{true, 0, *categoryId};
    }
    return std::nullopt;
}

QModelIndex TriggerExplorerModel::IndexForCategoryId(std::int32_t categoryId, int column) const {
    for (std::size_t row = 0; row < groups_.size(); ++row) {
        if (!groups_[row].synthetic && groups_[row].categoryId == categoryId) {
            return createIndex(static_cast<int>(row), column, static_cast<quintptr>(row + 1));
        }
    }
    return {};
}

QModelIndex TriggerExplorerModel::IndexForTriggerId(
    War3Map::StableId triggerId,
    int column) const {
    if (document_ == nullptr || column < 0 || column >= columnCount()) {
        return {};
    }
    for (std::size_t triggerIndex = 0; triggerIndex < document_->triggers.size();
         ++triggerIndex) {
        if (document_->triggers[triggerIndex].id != triggerId ||
            triggerIndex >= triggerGroupRows_.size()) {
            continue;
        }
        const std::size_t groupRow = triggerGroupRows_[triggerIndex];
        if (groupRow >= groups_.size()) {
            return {};
        }
        const auto& indices = groups_[groupRow].triggerIndices;
        const auto it = std::find(indices.begin(), indices.end(), triggerIndex);
        if (it == indices.end()) {
            return {};
        }
        return createIndex(
            static_cast<int>(it - indices.begin()),
            column,
            kTriggerIndexTag | static_cast<quintptr>(triggerIndex + 1));
    }
    return {};
}

QModelIndex TriggerExplorerModel::index(
    int row,
    int column,
    const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= columnCount()) {
        return {};
    }
    if (!parentIndex.isValid()) {
        if (row >= static_cast<int>(groups_.size())) {
            return {};
        }
        return createIndex(row, column, static_cast<quintptr>(row + 1));
    }
    if (IsTriggerIndex(parentIndex) || parentIndex.column() != 0) {
        return {};
    }
    const std::size_t groupIndex = DecodedIndex(parentIndex);
    if (groupIndex >= groups_.size() ||
        row >= static_cast<int>(groups_[groupIndex].triggerIndices.size())) {
        return {};
    }
    const std::size_t triggerIndex = groups_[groupIndex].triggerIndices[static_cast<std::size_t>(row)];
    return createIndex(
        row,
        column,
        kTriggerIndexTag | static_cast<quintptr>(triggerIndex + 1));
}

QModelIndex TriggerExplorerModel::parent(const QModelIndex& child) const {
    if (!IsTriggerIndex(child)) {
        return {};
    }
    const std::size_t triggerIndex = DecodedIndex(child);
    if (triggerIndex >= triggerGroupRows_.size()) {
        return {};
    }
    const std::size_t groupRow = triggerGroupRows_[triggerIndex];
    if (groupRow >= groups_.size()) {
        return {};
    }
    return createIndex(
        static_cast<int>(groupRow),
        0,
        static_cast<quintptr>(groupRow + 1));
}

int TriggerExplorerModel::rowCount(const QModelIndex& parentIndex) const {
    if (!parentIndex.isValid()) {
        return static_cast<int>(groups_.size());
    }
    if (IsTriggerIndex(parentIndex) || parentIndex.column() != 0) {
        return 0;
    }
    const std::size_t groupIndex = DecodedIndex(parentIndex);
    return groupIndex < groups_.size()
        ? static_cast<int>(groups_[groupIndex].triggerIndices.size())
        : 0;
}

int TriggerExplorerModel::columnCount(const QModelIndex&) const {
    return 2;
}

QVariant TriggerExplorerModel::data(const QModelIndex& modelIndex, int role) const {
    if (!modelIndex.isValid() || document_ == nullptr) {
        return {};
    }
    if (!HasGui() && (role == Qt::DecorationRole || role == Qt::ForegroundRole)) {
        return {};
    }
    const QColor muted = role == Qt::ForegroundRole ? QGuiApplication::palette().color(QPalette::PlaceholderText) : QColor{};
    if (!IsTriggerIndex(modelIndex)) {
        const std::size_t groupIndex = DecodedIndex(modelIndex);
        if (groupIndex >= groups_.size()) {
            return {};
        }
        const CategoryGroup& group = groups_[groupIndex];
        const QString name = group.synthetic
            ? tr("未分类或无效分类")
            : Utf8(document_->categories[group.sourceCategoryIndex].name.text);
        switch (role) {
        case NodeKindRole:
            return static_cast<int>(NodeKind::Category);
        case Qt::DisplayRole:
            return modelIndex.column() == 0 ? QVariant(name) : QVariant(QString::number(group.triggerIndices.size()));
        case Qt::EditRole:
            return modelIndex.column() == 0 ? QVariant(name) : QVariant{};
        case Qt::DecorationRole: {
            if (modelIndex.column() != 0) return {};
            // 类别用系统文件夹图标（原版 WE 的 16 色文件夹图标过于陈旧）。
            return War3Map::UiKit::SystemFolderIcon();
        }
        case Qt::ForegroundRole:
            return modelIndex.column() == 1 ? QVariant(muted) : QVariant{};
        case Qt::TextAlignmentRole:
            return modelIndex.column() == 1 ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter)) : QVariant{};
        case Qt::ToolTipRole:
            return tr("%1 · %2 个触发器").arg(name).arg(group.triggerIndices.size());
        default:
            return {};
        }
    }

    const std::size_t triggerIndex = DecodedIndex(modelIndex);
    if (triggerIndex >= document_->triggers.size()) {
        return {};
    }
    const auto& trigger = document_->triggers[triggerIndex];
    const bool comment = trigger.type != 0; // WTG 的 isComment：原版的“注释”条目，只有一段说明文字
    const bool off = !comment && trigger.enabled == 0;
    const bool closed = !comment && !off && trigger.initiallyClosed != 0;
    const bool script = !comment && trigger.customTextFlag != 0;
    const bool partial = !comment && triggerIndex < partial_.size() && partial_[triggerIndex] != 0;
    switch (role) {
    case NodeKindRole:
        return static_cast<int>(NodeKind::Trigger);
    case StableIdRole:
        return QVariant::fromValue<qulonglong>(trigger.id);
    case SourceIndexRole:
        return QVariant::fromValue<qulonglong>(triggerIndex);
    case Qt::DisplayRole:
        if (modelIndex.column() == 0) return Utf8(trigger.name.text);
        return comment ? tr("注释") : off ? tr("禁用") : closed ? tr("关") : QString{};
    case Qt::EditRole:
        return modelIndex.column() == 0 ? QVariant(Utf8(trigger.name.text)) : QVariant{};
    case Qt::DecorationRole: {
        if (modelIndex.column() != 0) return {};
        // 与原版一致：注释 / 未允许（红叉）/ 初始关闭（灰）/ 含禁用语句（红角）及其组合。
        using Icon = War3Map::QtApp::IconProvider::EditorIcon;
        const Icon kind = comment ? Icon::Comment
            : off ? (closed ? Icon::TriggerUnusedDisabled : Icon::TriggerDisabled)
            : trigger.initiallyClosed != 0 ? (partial ? Icon::TriggerPartialUnused : Icon::TriggerUnused)
            : partial ? Icon::TriggerPartial : Icon::Trigger;
        if (iconProvider_ != nullptr) {
            const QIcon icon = iconProvider_->IconForEditor(kind);
            if (!icon.isNull()) return icon;
        }
        return War3Map::UiKit::SvgIcon(comment ? QStringLiteral("lucide/pencil")
            : script ? QStringLiteral("lucide/file-code-2") : QStringLiteral("lucide/file"));
    }
    case Qt::FontRole:
        if (off && modelIndex.column() == 0) {
            QFont font;
            font.setStrikeOut(true);
            return font;
        }
        return {};
    case Qt::ForegroundRole:
        if (comment) return QColor(0x6c, 0xc3, 0x6c); // 原版注释条目为绿色
        return off || modelIndex.column() == 1 ? QVariant(muted) : QVariant{};
    case Qt::TextAlignmentRole:
        return modelIndex.column() == 1 ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter)) : QVariant{};
    case Qt::ToolTipRole:
        if (comment) return tr("%1\n注释（只有说明文字，不生成代码）").arg(Utf8(trigger.name.text));
        return tr("%1%2%3\n%4 · 顶层语句 %5")
            .arg(Utf8(trigger.name.text),
                off ? tr(" · 已禁用（不生成代码）") : closed ? tr(" · 初始关闭") : QString{},
                partial ? tr(" · 含禁用的语句") : QString{},
                script ? tr("自定义脚本") : tr("GUI 触发器"))
            .arg(trigger.ecas.size());
    default:
        return {};
    }
}

QVariant TriggerExplorerModel::headerData(
    int section,
    Qt::Orientation orientation,
    int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case 0: return tr("触发器");
    case 1: return tr("状态");
    default: return {};
    }
}

Qt::ItemFlags TriggerExplorerModel::flags(const QModelIndex& modelIndex) const {
    if (!modelIndex.isValid()) {
        return Qt::ItemIsDropEnabled; // 根：类别排到最后
    }
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (ItemAt(modelIndex)) {
        result |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
        if (modelIndex.column() == 0) result |= Qt::ItemIsEditable;
    }
    return result;
}

bool TriggerExplorerModel::setData(const QModelIndex& modelIndex, const QVariant& value, int role) {
    if (role != Qt::EditRole || modelIndex.column() != 0 || !renameHandler_) {
        return false;
    }
    const auto item = ItemAt(modelIndex);
    return item && renameHandler_(*item, value.toString());
}

QStringList TriggerExplorerModel::mimeTypes() const {
    return {QString::fromLatin1(kDirectoryMime)};
}

QMimeData* TriggerExplorerModel::mimeData(const QModelIndexList& indexes) const {
    for (const QModelIndex& modelIndex : indexes) {
        const auto item = ItemAt(modelIndex);
        if (!item) continue;
        auto* data = new QMimeData;
        const QString text = item->category
            ? QStringLiteral("category:%1").arg(item->categoryId)
            : QStringLiteral("trigger:%1").arg(item->triggerId);
        data->setData(QString::fromLatin1(kDirectoryMime), text.toLatin1());
        return data;
    }
    return nullptr;
}

Qt::DropActions TriggerExplorerModel::supportedDragActions() const {
    return Qt::MoveAction;
}

Qt::DropActions TriggerExplorerModel::supportedDropActions() const {
    return Qt::MoveAction;
}

std::optional<TriggerDirectoryItem> TriggerExplorerModel::DecodeMime(const QMimeData* data) const {
    if (data == nullptr || document_ == nullptr) {
        return std::nullopt;
    }
    const QString text = QString::fromLatin1(data->data(QString::fromLatin1(kDirectoryMime)));
    const qsizetype colon = text.indexOf(':');
    if (colon <= 0) {
        return std::nullopt;
    }
    bool ok = false;
    if (text.left(colon) == QLatin1String("category")) {
        const int categoryId = text.mid(colon + 1).toInt(&ok);
        if (ok && IndexForCategoryId(categoryId).isValid()) return TriggerDirectoryItem{true, 0, categoryId};
    } else if (text.left(colon) == QLatin1String("trigger")) {
        const War3Map::StableId id = text.mid(colon + 1).toULongLong(&ok);
        if (ok) {
            for (const auto& trigger : document_->triggers) {
                if (trigger.id == id) return TriggerDirectoryItem{false, id, trigger.categoryId};
            }
        }
    }
    return std::nullopt;
}

std::optional<TriggerDirectoryMove> TriggerExplorerModel::ResolveDrop(
    const TriggerDirectoryItem& source, int row, const QModelIndex& parentIndex) const {
    const QModelIndex target = parentIndex.isValid() ? parentIndex.siblingAtColumn(0) : QModelIndex{};
    if (source.category) {
        // 类别只在顶层排序：放在某一类别（或其中的触发器）之前，或放到最后。
        std::optional<std::int32_t> before;
        if (!target.isValid()) {
            if (row >= 0 && row < static_cast<int>(groups_.size()) && !groups_[static_cast<std::size_t>(row)].synthetic) {
                before = groups_[static_cast<std::size_t>(row)].categoryId;
            }
        } else if (const auto item = ItemAt(target)) {
            before = item->categoryId;
        } else {
            return std::nullopt;
        }
        if (before && *before == source.categoryId) {
            return std::nullopt;
        }
        return TriggerDirectoryMove{source, source.categoryId, 0, before};
    }
    if (!target.isValid()) {
        return std::nullopt; // 触发器不能放在类别之间
    }
    if (const auto* trigger = TriggerAt(target)) {
        if (trigger->id == source.triggerId) return std::nullopt;
        return TriggerDirectoryMove{source, trigger->categoryId, trigger->id, std::nullopt};
    }
    const auto categoryId = CategoryIdAt(target);
    if (!categoryId) {
        return std::nullopt;
    }
    War3Map::StableId before = 0;
    const auto& indices = groups_[DecodedIndex(target)].triggerIndices;
    if (row >= 0 && row < static_cast<int>(indices.size())) {
        before = document_->triggers[indices[static_cast<std::size_t>(row)]].id;
        if (before == source.triggerId) return std::nullopt;
    }
    return TriggerDirectoryMove{source, *categoryId, before, std::nullopt};
}

bool TriggerExplorerModel::canDropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) const {
    if (action != Qt::MoveAction) {
        return false;
    }
    const auto source = DecodeMime(data);
    return source && ResolveDrop(*source, row, parentIndex).has_value();
}

bool TriggerExplorerModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) {
    if (action != Qt::MoveAction || !moveHandler_) {
        return false;
    }
    const auto source = DecodeMime(data);
    const auto move = source ? ResolveDrop(*source, row, parentIndex) : std::nullopt;
    if (!move) {
        return false;
    }
    // 处理方排队执行：此刻视图仍在拖放事件中，不能同步重置模型。
    moveHandler_(*move);
    return true;
}

bool TriggerExplorerModel::IsTriggerIndex(const QModelIndex& index) noexcept {
    return index.isValid() && (index.internalId() & kTriggerIndexTag) != 0;
}

std::size_t TriggerExplorerModel::DecodedIndex(const QModelIndex& index) noexcept {
    const quintptr encoded = index.internalId() & ~kTriggerIndexTag;
    return encoded == 0
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(encoded - 1);
}

