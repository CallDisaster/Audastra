#include "TriggerModels.h"
#include "TriggerModelsDetail.h"

#include "Icons/IconProvider.h"
#include "Icons/SvgIcons.h"

#include <QGuiApplication>
#include <QPointer>
#include <limits>

using namespace TriggerModelsDetail;

namespace {

bool HasGuiApplication() {
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

} // namespace

// ECA 逻辑树模型：事件 / 条件 / 动作分组与嵌套函数体（放置位置与拖放见 TriggerEcaModelSlots.cpp）。

TriggerEcaModel::TriggerEcaModel(QObject* parent)
    : QAbstractItemModel(parent) {
}

void TriggerEcaModel::SetDocument(
    const War3Map::Trigger::TriggerDocument* document,
    const War3Map::Trigger::UiCatalog* catalog) {
    if (presentationTopologyEpoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++presentationTopologyEpoch_;
    beginResetModel();
    // 重置通知允许同步替换同地址目录；返回后才核验当前 owner 与实际来源 SHA。
    // 只复用目录类型，不缓存文档、变量或参数资格；DisplayName 当场使用当前 Localize。
    const bool reuseTypes = document && catalog && typeSystem_ && catalog_ == catalog
        && !catalog->Sha256().empty() && typeCatalogSha256_ == catalog->Sha256();
    document_ = document;
    catalog_ = catalog;
    if (!reuseTypes) {
        typeSystem_ = document && catalog ? std::make_unique<War3Map::Trigger::TriggerTypeSystem>(*catalog) : nullptr;
        typeCatalogSha256_ = typeSystem_ ? catalog->Sha256() : std::string{};
    }
    presentationOptions_.typeSystem = typeSystem_.get();
    triggerId_ = 0;
    roots_.clear();
    parents_.clear();
    rowsInParent_.clear();
    topLevelGroup_.clear();
    for (auto& bucket : groupBuckets_) {
        bucket.clear();
    }
    presentationCache_.clear();
    endResetModel();
}

void TriggerEcaModel::SetTrigger(War3Map::StableId triggerId) {
    if (presentationTopologyEpoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++presentationTopologyEpoch_;
    beginResetModel();
    triggerId_ = triggerId;
    roots_.clear();
    presentationCache_.clear();
    if (document_ != nullptr) {
        const auto trigger = std::find_if(
            document_->triggers.begin(),
            document_->triggers.end(),
            [triggerId](const War3Map::Trigger::TriggerRecord& value) {
                return value.id == triggerId;
            });
        if (trigger != document_->triggers.end()) {
            roots_ = trigger->ecas;
        }
    }
    RebuildParentIndex();
    endResetModel();
}

bool TriggerEcaModel::IsBoundTo(const War3Map::Trigger::TriggerDocument* document,
    const War3Map::Trigger::UiCatalog* catalog) const noexcept {
    return document && catalog && document_ == document && catalog_ == catalog && typeSystem_
        && !catalog->Sha256().empty() && typeCatalogSha256_ == catalog->Sha256();
}

bool TriggerEcaModel::InvalidateScalarPresentations() {
    // 缓存没有天然行数帽；先准入再分配。最多仅复制 32KiB ordinal，不复制呈现文字。
    constexpr std::size_t maxCachedRows = 4096;
    if (!IsBoundTo(document_, catalog_) || presentationCache_.size() > maxCachedRows
        || presentationTopologyEpoch_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
    std::vector<std::size_t> cachedRows;
    try {
        cachedRows.reserve(presentationCache_.size());
        for (const auto& entry : presentationCache_) cachedRows.push_back(entry.first);
    } catch (...) { return false; }
    const QPointer<TriggerEcaModel> self(this);
    const auto epoch = presentationTopologyEpoch_;
    const auto* document = document_; const auto* catalog = catalog_;
    // 嵌套 call 的变化也可能改变祖先语句；全部 cached presentation 作废。
    presentationCache_.clear();
    for (const auto node : cachedRows) {
        if (!self || presentationTopologyEpoch_ != epoch || document_ != document || catalog_ != catalog) return false;
        if (node >= rowsInParent_.size() || rowsInParent_[node] == War3Map::Trigger::kInvalidNodeIndex) continue;
        const auto row = rowsInParent_[node];
        if (row > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) return false;
        emit dataChanged(createIndex(static_cast<int>(row), 0, static_cast<quintptr>(node + 1)),
            createIndex(static_cast<int>(row), 1, static_cast<quintptr>(node + 1)),
            {Qt::DisplayRole, Qt::ToolTipRole, ArgumentSpansRole});
    }
    return self && presentationTopologyEpoch_ == epoch && document_ == document && catalog_ == catalog;
}

// 事件 / 条件 / 动作 / 函数调用的类别图标：优先原版编辑器图标（本机游戏数据），缺失时用内置 SVG。
QIcon TriggerEcaModel::KindIcon(std::int32_t type, bool disabled) const {
    using Icon = War3Map::QtApp::IconProvider::EditorIcon;
    if (iconProvider_ != nullptr) {
        const Icon kind = disabled ? Icon::FunctionDisabled
            : type == 0 ? Icon::Event : type == 1 ? Icon::Condition : type == 2 ? Icon::Action : Icon::Function;
        const QIcon icon = iconProvider_->IconForEditor(kind);
        if (!icon.isNull()) return icon;
    }
    if (!HasGuiApplication()) {
        return {};
    }
    if (disabled) return War3Map::UiKit::SvgIcon(QStringLiteral("lucide/x"), QColor(0xe5, 0x53, 0x4b));
    switch (type) {
    case 0: return War3Map::UiKit::SvgIcon(QStringLiteral("lucide/flag"), QColor(0x75, 0xd9, 0xff));
    case 1: return War3Map::UiKit::SvgIcon(QStringLiteral("lucide/circle-dot"), QColor(0xc6, 0xa8, 0xff));
    case 2: return War3Map::UiKit::SvgIcon(QStringLiteral("lucide/play"), QColor(0x7e, 0xe0, 0xbd));
    default: return War3Map::UiKit::SvgIcon(QStringLiteral("lucide/settings-2"), QColor(0xff, 0xca, 0x80));
    }
}

void TriggerEcaModel::SetIconProvider(War3Map::QtApp::IconProvider* provider) {
    iconProvider_ = provider;
}

void TriggerEcaModel::SetObjectNameResolver(
    std::function<std::string(std::string_view rawcode)> resolver) {
    if (presentationTopologyEpoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++presentationTopologyEpoch_;
    objectNameResolver_ = std::move(resolver);
    presentationOptions_.objectNameResolver = objectNameResolver_;
    presentationCache_.clear();
}

void TriggerEcaModel::SetPresentationOptions(War3Map::Trigger::PresentationOptions options) {
    if (presentationTopologyEpoch_ != (std::numeric_limits<std::uint64_t>::max)()) ++presentationTopologyEpoch_;
    beginResetModel();
    presentationOptions_ = std::move(options);
    presentationOptions_.typeSystem = typeSystem_.get();
    presentationCache_.clear();
    endResetModel();
}

const War3Map::Trigger::EcaNode* TriggerEcaModel::EcaAt(
    const QModelIndex& modelIndex) const noexcept {
    const std::size_t nodeIndex = NodeIndex(modelIndex);
    return document_ != nullptr && nodeIndex < document_->ecaNodes.size()
        ? &document_->ecaNodes[nodeIndex]
        : nullptr;
}

const War3Map::Trigger::ArgumentNode* TriggerEcaModel::ArgumentAt(
    const QModelIndex& modelIndex,
    std::size_t argumentRow) const noexcept {
    const auto* eca = EcaAt(modelIndex);
    if (document_ == nullptr || eca == nullptr || argumentRow >= eca->arguments.size()) {
        return nullptr;
    }
    const std::uint32_t argumentIndex = eca->arguments[argumentRow];
    return argumentIndex < document_->argumentNodes.size()
        ? &document_->argumentNodes[argumentIndex]
        : nullptr;
}

QModelIndex TriggerEcaModel::IndexForStableId(
    War3Map::StableId ecaId,
    int column) const {
    if (document_ == nullptr || column < 0 || column >= columnCount()) {
        return {};
    }
    const auto found = std::find_if(
        document_->ecaNodes.begin(), document_->ecaNodes.end(),
        [ecaId](const War3Map::Trigger::EcaNode& value) { return value.id == ecaId; });
    if (found == document_->ecaNodes.end()) {
        return {};
    }
    const std::size_t nodeIndex = static_cast<std::size_t>(found - document_->ecaNodes.begin());
    if (nodeIndex >= rowsInParent_.size() ||
        rowsInParent_[nodeIndex] == War3Map::Trigger::kInvalidNodeIndex) {
        return {};
    }
    return createIndex(
        static_cast<int>(rowsInParent_[nodeIndex]),
        column,
        static_cast<quintptr>(nodeIndex + 1));
}

QModelIndex TriggerEcaModel::index(
    int row,
    int column,
    const QModelIndex& parentIndex) const {
    if (document_ == nullptr || row < 0 || column < 0 || column >= columnCount()) {
        return {};
    }
    // 顶层：事件/条件/动作 三个分组头。
    if (!parentIndex.isValid()) {
        if (triggerId_ == 0 || row >= 3) {
            return {};
        }
        return createIndex(
            row, column,
            static_cast<quintptr>(kTriggerIndexTag | static_cast<quintptr>(row)));
    }
    if (parentIndex.column() != 0) {
        return {};
    }
    std::uint32_t nodeIndex = War3Map::Trigger::kInvalidNodeIndex;
    if (IsTopGroupHandle(parentIndex)) {
        const int group = GroupOfHandle(parentIndex);
        if (group < 0 || group >= 3 ||
            row >= static_cast<int>(groupBuckets_[static_cast<std::size_t>(group)].size())) {
            return {};
        }
        nodeIndex = groupBuckets_[static_cast<std::size_t>(group)][static_cast<std::size_t>(row)];
    } else if (IsRoleHeaderHandle(parentIndex)) {
        // 角色分组头：子级为该分支（childId）下的具体条件/动作节点。
        const std::uint32_t parentNode = RoleParentOfHandle(parentIndex);
        const int slot = RoleSlotOfHandle(parentIndex);
        if (parentNode >= document_->ecaNodes.size()) {
            return {};
        }
        const auto groups = RoleGroupsFor(parentNode);
        if (slot < 0 || slot >= static_cast<int>(groups.size()) ||
            row >= static_cast<int>(groups[static_cast<std::size_t>(slot)].children.size())) {
            return {};
        }
        nodeIndex = groups[static_cast<std::size_t>(slot)]
                        .children[static_cast<std::size_t>(row)];
    } else {
        // 函数体节点：子级为按 childId 划分的角色分组头（条件/则/否则/动作）。
        const std::size_t parentNodeIndex = NodeIndex(parentIndex);
        if (parentNodeIndex >= document_->ecaNodes.size()) {
            return {};
        }
        const auto groups =
            RoleGroupsFor(static_cast<std::uint32_t>(parentNodeIndex));
        if (row >= static_cast<int>(groups.size())) {
            return {};
        }
        return createIndex(
            row,
            column,
            MakeRoleHeaderHandle(static_cast<std::uint32_t>(parentNodeIndex), row));
    }
    if (nodeIndex >= document_->ecaNodes.size()) {
        return {};
    }
    return createIndex(row, column, static_cast<quintptr>(nodeIndex + 1));
}

QModelIndex TriggerEcaModel::parent(const QModelIndex& child) const {
    if (IsTopGroupHandle(child)) {
        return {}; // 分组头的父级是根。
    }
    if (IsRoleHeaderHandle(child)) {
        // 角色分组头的父级是其所属函数体节点。
        const std::uint32_t parentNode = RoleParentOfHandle(child);
        if (parentNode >= rowsInParent_.size() ||
            rowsInParent_[parentNode] == War3Map::Trigger::kInvalidNodeIndex) {
            return {};
        }
        return createIndex(
            static_cast<int>(rowsInParent_[parentNode]),
            0,
            static_cast<quintptr>(parentNode + 1));
    }
    const std::size_t childIndex = NodeIndex(child);
    if (childIndex >= parents_.size()) {
        return {};
    }
    // 顶层节点：父级是其所属分组头。
    if (childIndex < topLevelGroup_.size() && topLevelGroup_[childIndex] != kNoGroup) {
        const int group = topLevelGroup_[childIndex];
        return createIndex(
            group, 0,
            static_cast<quintptr>(kTriggerIndexTag | static_cast<quintptr>(group)));
    }
    const std::uint32_t parentIndex = parents_[childIndex];
    if (parentIndex == War3Map::Trigger::kInvalidNodeIndex || document_ == nullptr ||
        parentIndex >= document_->ecaNodes.size()) {
        return {};
    }
    // 嵌套子节点：父级是其分支号（childId）对应的角色分组头。
    const std::int32_t branch = document_->ecaNodes[childIndex].childId.value_or(0);
    const auto groups = RoleGroupsFor(parentIndex);
    for (std::size_t slot = 0; slot < groups.size(); ++slot) {
        if (groups[slot].branch == branch) {
            return createIndex(
                static_cast<int>(slot),
                0,
                MakeRoleHeaderHandle(parentIndex, static_cast<int>(slot)));
        }
    }
    return {};
}

int TriggerEcaModel::rowCount(const QModelIndex& parentIndex) const {
    if (document_ == nullptr) {
        return 0;
    }
    if (!parentIndex.isValid()) {
        return triggerId_ == 0 ? 0 : 3; // 事件/条件/动作 三个分组头
    }
    if (parentIndex.column() != 0) {
        return 0;
    }
    if (IsTopGroupHandle(parentIndex)) {
        const int group = GroupOfHandle(parentIndex);
        return (group >= 0 && group < 3)
            ? static_cast<int>(groupBuckets_[static_cast<std::size_t>(group)].size())
            : 0;
    }
    if (IsRoleHeaderHandle(parentIndex)) {
        const std::uint32_t parentNode = RoleParentOfHandle(parentIndex);
        const int slot = RoleSlotOfHandle(parentIndex);
        if (parentNode >= document_->ecaNodes.size()) {
            return 0;
        }
        const auto groups = RoleGroupsFor(parentNode);
        return (slot >= 0 && slot < static_cast<int>(groups.size()))
            ? static_cast<int>(groups[static_cast<std::size_t>(slot)].children.size())
            : 0;
    }
    // 函数体节点：子级为角色分组头数量。
    const std::size_t nodeIndex = NodeIndex(parentIndex);
    return nodeIndex < document_->ecaNodes.size()
        ? static_cast<int>(RoleGroupsFor(static_cast<std::uint32_t>(nodeIndex)).size())
        : 0;
}

int TriggerEcaModel::columnCount(const QModelIndex&) const {
    return 2;
}

QVariant TriggerEcaModel::data(const QModelIndex& modelIndex, int role) const {
    // 顶层分组头（事件/条件/动作）：先于 ECA 节点处理。
    if (IsTopGroupHandle(modelIndex)) {
        const int group = GroupOfHandle(modelIndex);
        if (group < 0 || group >= 3) {
            return {};
        }
        const int count =
            static_cast<int>(groupBuckets_[static_cast<std::size_t>(group)].size());
        switch (role) {
        case Qt::DisplayRole:
            if (modelIndex.column() == 0) {
                return count > 0 ? tr("%1 (%2)").arg(GroupName(group)).arg(count)
                                 : GroupName(group);
            }
            return {};
        case Qt::DecorationRole:
            return modelIndex.column() == 0 ? QVariant(KindIcon(group, false)) : QVariant{};
        case Qt::FontRole: {
            QFont font;
            font.setBold(true);
            return font;
        }
        case Qt::ForegroundRole:
            return QColor(QStringLiteral("#e6edf3"));
        default:
            return {};
        }
    }
    // 函数体角色分组头（条件/则/否则/动作）：标签 + 计数 + 加粗。
    if (IsRoleHeaderHandle(modelIndex)) {
        if (document_ == nullptr) {
            return {};
        }
        const std::uint32_t parentNode = RoleParentOfHandle(modelIndex);
        const int slot = RoleSlotOfHandle(modelIndex);
        if (parentNode >= document_->ecaNodes.size()) {
            return {};
        }
        const auto groups = RoleGroupsFor(parentNode);
        if (slot < 0 || slot >= static_cast<int>(groups.size())) {
            return {};
        }
        const RoleGroup& group = groups[static_cast<std::size_t>(slot)];
        const std::int32_t childType = group.type;
        switch (role) {
        case Qt::DisplayRole:
            if (modelIndex.column() == 0) {
                const QString label = BranchLabel(document_->ecaNodes[parentNode].name.text, group.branch, childType, catalog_);
                return group.children.empty() ? label : tr("%1 (%2)").arg(label).arg(group.children.size());
            }
            return {};
        case Qt::DecorationRole:
            return modelIndex.column() == 0 ? QVariant(KindIcon(childType, false)) : QVariant{};
        case Qt::FontRole: {
            QFont font;
            font.setBold(true);
            return font;
        }
        case Qt::ForegroundRole:
            return QColor(QStringLiteral("#9aa4b2"));
        default:
            return {};
        }
    }
    const auto* eca = EcaAt(modelIndex);
    if (eca == nullptr) {
        return {};
    }
    if (role == StableIdRole) {
        return QVariant::fromValue<qulonglong>(eca->id);
    }
    if (role == NodeIndexRole) {
        return QVariant::fromValue<qulonglong>(NodeIndex(modelIndex));
    }
    if (role == ArgumentSpansRole) {
        const auto& presentation = Presentation(NodeIndex(modelIndex));
        QVariantList spans;
        for (const auto& span : presentation.arguments) {
            QStringList candidates;
            for (const auto& candidate : span.candidates) candidates.push_back(Utf8(candidate));
            // 字节偏移换算为 QString（UTF-16）偏移。
            const auto begin = QString::fromUtf8(presentation.text.data(), static_cast<qsizetype>(span.begin)).size();
            const auto length = QString::fromUtf8(presentation.text.data() + span.begin, static_cast<qsizetype>(span.length)).size();
            spans.append(QVariantMap{{"begin", begin}, {"length", length}, {"position", static_cast<qulonglong>(span.position)},
                {"empty", span.empty}, {"call", span.call}, {"type", Utf8(span.uiType)},
                {"raw", Utf8(span.rawValue)}, {"reference", Utf8(span.reference)}, {"issue", Utf8(span.issue)}, {"candidates", candidates},
                {"badge", !presentationOptions_.showRawValues && !span.reference.empty()},
                {"state", Utf8(span.state)}, {"icon", !presentationOptions_.showRawValues && iconProvider_ && !span.iconPath.empty()
                    ? QVariant(iconProvider_->IconForTexture(Utf8(span.iconPath))) : QVariant{}}});
        }
        return spans;
    }
    if (role == Qt::FontRole && IsCommentEca(*eca)) {
        QFont font;
        font.setItalic(true); // 注释语句按原版绿色斜体
        return font;
    }
    if (role == Qt::ForegroundRole && eca->enabled == 0) {
        return QColor(QStringLiteral("#788794"));
    }
    if (role == Qt::ForegroundRole && IsCommentEca(*eca)) {
        return QColor(QStringLiteral("#5fbf77"));
    }
    if (role == Qt::ForegroundRole && modelIndex.column() == 1) {
        switch (eca->type) {
        case 0: return QColor(QStringLiteral("#75d9ff"));
        case 1: return QColor(QStringLiteral("#c6a8ff"));
        case 2: return QColor(QStringLiteral("#7ee0bd"));
        case 3: return QColor(QStringLiteral("#ffca80"));
        default: break;
        }
    }
    if (role == Qt::ToolTipRole) {
        const auto& presentation = Presentation(NodeIndex(modelIndex));
        return tr("自然语言：%1\n原始函数名：%2\n稳定 ID：%3\nUI 层：%4\n签名：%5")
            .arg(Utf8(presentation.text))
            .arg(Utf8(eca->name.text))
            .arg(eca->id)
            .arg(Utf8(eca->uiLayer))
            .arg(Utf8(eca->uiSignature));
    }
    if (role == Qt::DecorationRole && modelIndex.column() == 0) {
        // 禁用节点：红齿轮（优先于真实/回退图标，与 WE 一致）。
        if (eca->enabled == 0) {
            return KindIcon(eca->type, true);
        }
        // 优先真实游戏图标（经函数名→分类→纹理），未命中时回退到程序化图标。
        if (iconProvider_ != nullptr && iconProvider_->IsReady()) {
            QIcon real = iconProvider_->IconForFunction(Utf8(eca->name.text));
            if (!real.isNull()) {
                return real;
            }
        }
        return KindIcon(eca->type, false);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    // WE 式布局：第 0 列为自然语言（随树缩进），第 1 列为窄的"类型"标签。
    switch (modelIndex.column()) {
    case 0:
        return Utf8(Presentation(NodeIndex(modelIndex)).text);
    case 1:
        return EcaKindName(eca->type);
    default:
        return {};
    }
}

QVariant TriggerEcaModel::headerData(
    int section,
    Qt::Orientation orientation,
    int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case 0: return tr("内容");
    case 1: return tr("类型");
    default: return {};
    }
}

Qt::ItemFlags TriggerEcaModel::flags(const QModelIndex& modelIndex) const {
    if (!modelIndex.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    if (EcaAt(modelIndex) != nullptr) {
        result |= Qt::ItemIsDragEnabled;
    }
    return result;
}

std::size_t TriggerEcaModel::NodeIndex(const QModelIndex& modelIndex) const noexcept {
    // 分组头/角色分组头句柄（高位置 1）不对应任何 ECA 节点。
    return !modelIndex.isValid() || modelIndex.internalId() == 0 ||
            (modelIndex.internalId() & kTriggerIndexTag) != 0
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(modelIndex.internalId() - 1);
}

const War3Map::Trigger::EcaPresentation& TriggerEcaModel::Presentation(
    std::size_t nodeIndex) const {
    const auto existing = presentationCache_.find(nodeIndex);
    if (existing != presentationCache_.end()) {
        return existing->second;
    }
    War3Map::Trigger::EcaPresentation value;
    if (document_ == nullptr || nodeIndex >= document_->ecaNodes.size()) {
        value.text = "<ECA 结构无效>";
    } else if (catalog_ == nullptr) {
        const auto& eca = document_->ecaNodes[nodeIndex];
        value.text = "[未加载 UI Catalog] " + eca.name.text;
        value.rawFunctionName = eca.name.text;
    } else {
        if (presentationOptions_.objectNameResolver || presentationOptions_.referenceResolver || presentationOptions_.showRawValues) {
            value = War3Map::Trigger::PresentEca(
                *document_,
                static_cast<std::uint32_t>(nodeIndex),
                *catalog_,
                presentationOptions_);
        } else {
            value = War3Map::Trigger::PresentEca(
                *document_,
                static_cast<std::uint32_t>(nodeIndex),
                *catalog_);
        }
    }
    return presentationCache_.emplace(nodeIndex, std::move(value)).first->second;
}

std::vector<TriggerEcaModel::RoleGroup> TriggerEcaModel::RoleGroupsFor(
    std::uint32_t nodeIndex) const {
    std::vector<RoleGroup> groups;
    if (document_ == nullptr || nodeIndex >= document_->ecaNodes.size()) {
        return groups;
    }
    // 按 childId（分支号）分组：组内保持原始顺序，组按分支号升序
    //（If：0 条件 → 1 Then → 2 Else；循环/枚举体为 0）。声明的空分支同样列出。
    std::map<std::int32_t, RoleGroup> byBranch;
    for (const BranchSpec& spec : DeclaredBranches(document_->ecaNodes[nodeIndex].name.text, catalog_)) {
        byBranch[spec.branch] = RoleGroup{spec.branch, spec.type, {}};
    }
    for (const std::uint32_t child : document_->ecaNodes[nodeIndex].children) {
        if (child >= document_->ecaNodes.size()) {
            continue;
        }
        const std::int32_t branch = document_->ecaNodes[child].childId.value_or(0);
        const auto [entry, inserted] = byBranch.try_emplace(branch, RoleGroup{branch, document_->ecaNodes[child].type, {}});
        entry->second.children.push_back(child);
    }
    groups.reserve(byBranch.size());
    for (auto& entry : byBranch) {
        groups.push_back(std::move(entry.second));
    }
    return groups;
}

void TriggerEcaModel::RebuildParentIndex() {
    parents_.clear();
    rowsInParent_.clear();
    topLevelGroup_.clear();
    for (auto& bucket : groupBuckets_) {
        bucket.clear();
    }
    if (document_ == nullptr) {
        return;
    }
    const std::size_t nodeCount = document_->ecaNodes.size();
    parents_.assign(nodeCount, War3Map::Trigger::kInvalidNodeIndex);
    rowsInParent_.assign(nodeCount, War3Map::Trigger::kInvalidNodeIndex);
    topLevelGroup_.assign(nodeCount, kNoGroup);
    std::vector<bool> visited(nodeCount, false);

    // 顶层 ECA 按类型分入事件/条件/动作三个桁（行号为桁内位置）。
    std::vector<std::uint32_t> pending;
    pending.reserve(roots_.size());
    for (const std::uint32_t root : roots_) {
        if (root >= nodeCount || visited[root]) {
            continue;
        }
        const int group = TypeToGroup(document_->ecaNodes[root].type);
        rowsInParent_[root] = static_cast<std::uint32_t>(groupBuckets_[group].size());
        topLevelGroup_[root] = static_cast<std::uint8_t>(group);
        groupBuckets_[group].push_back(root);
        visited[root] = true;
        pending.push_back(root);
    }
    // 嵌套子节点（If/Loop/ForEach 函数体）：行号为在其角色分组（childId）内的位置。
    while (!pending.empty()) {
        const std::uint32_t parentIndex = pending.back();
        pending.pop_back();
        for (const auto& group : RoleGroupsFor(parentIndex)) {
            for (std::size_t row = 0; row < group.children.size(); ++row) {
                const std::uint32_t child = group.children[row];
                if (child >= nodeCount || visited[child]) {
                    continue;
                }
                visited[child] = true;
                parents_[child] = parentIndex;
                rowsInParent_[child] = static_cast<std::uint32_t>(row);
                pending.push_back(child);
            }
        }
    }
}
