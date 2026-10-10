#pragma once

#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QAbstractItemModel>
#include <QIcon>
#include <QStringList>

class QMimeData;

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::QtApp {
class IconProvider;
} // namespace War3Map::QtApp

// 目录中的一项：类别或触发器。未分类的合成组没有对应项。
struct TriggerDirectoryItem {
    bool category = false;
    War3Map::StableId triggerId = 0;
    std::int32_t categoryId = 0;
};

// 拖放请求：移动触发器到类别（插在 beforeTriggerId 之前，0 = 末尾），或把类别移到 beforeCategoryId 之前。
struct TriggerDirectoryMove {
    TriggerDirectoryItem source;
    std::int32_t targetCategoryId = 0;
    War3Map::StableId beforeTriggerId = 0;
    std::optional<std::int32_t> beforeCategoryId;
};

// 触发器目录（原版“类别 / 触发器”）：两列（名称、状态标记），支持就地重命名与拖放排序。
// 模型只读文档；重命名与移动交给模块处理（经撤销栈），模型随后整体刷新。
class TriggerExplorerModel final : public QAbstractItemModel {
public:
    enum Role {
        NodeKindRole = Qt::UserRole + 1,
        StableIdRole,
        SourceIndexRole,
    };

    enum class NodeKind : std::uint8_t {
        Category,
        Trigger,
    };

    using RenameHandler = std::function<bool(const TriggerDirectoryItem& item, const QString& name)>;
    using MoveHandler = std::function<void(const TriggerDirectoryMove& move)>;

    explicit TriggerExplorerModel(QObject* parent = nullptr);

    // 完整重绑：总是重置模型（打开地图、换基线等）。
    void SetDocument(const War3Map::Trigger::TriggerDocument* document);
    // 编辑后的刷新：同一文档且分类、触发器顺序与归属都未变时只通知变化的行，
    // 保留视图展开、选择与滚动；否则同 SetDocument。返回是否重置了模型。
    bool RefreshDocument(const War3Map::Trigger::TriggerDocument* document);
    // 原版编辑器图标（来自本机游戏数据）；为空或缺图时用内置 SVG。
    void SetIconProvider(War3Map::QtApp::IconProvider* provider) { iconProvider_ = provider; }
    // 展开的类别显示打开的文件夹图标（视图展开 / 折叠时同步）。
    void SetExpandedCategories(std::set<std::int32_t> expanded);
    void SetRenameHandler(RenameHandler handler) { renameHandler_ = std::move(handler); }
    void SetMoveHandler(MoveHandler handler) { moveHandler_ = std::move(handler); }
    [[nodiscard]] const War3Map::Trigger::TriggerRecord* TriggerAt(
        const QModelIndex& index) const noexcept;
    // 分类节点的 categoryId；非分类节点返回 nullopt。
    [[nodiscard]] std::optional<std::int32_t> CategoryIdAt(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] std::optional<TriggerDirectoryItem> ItemAt(const QModelIndex& index) const noexcept;
    // 按稳定 ID 找触发器的模型索引（找不到返回无效索引）。
    [[nodiscard]] QModelIndex IndexForTriggerId(
        War3Map::StableId triggerId,
        int column = 0) const;
    [[nodiscard]] QModelIndex IndexForCategoryId(std::int32_t categoryId, int column = 0) const;

    [[nodiscard]] QModelIndex index(
        int row,
        int column,
        const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) override;
    // 拖放语义（供测试直接调用）：parent/row 与 QAbstractItemView 的放置位置一致。
    [[nodiscard]] std::optional<TriggerDirectoryMove> ResolveDrop(
        const TriggerDirectoryItem& source, int row, const QModelIndex& parent) const;

private:
    struct CategoryGroup {
        std::size_t sourceCategoryIndex = 0;
        std::int32_t categoryId = 0;
        bool synthetic = false;
        std::vector<std::size_t> triggerIndices;
    };

    [[nodiscard]] static bool IsTriggerIndex(const QModelIndex& index) noexcept;
    [[nodiscard]] static std::size_t DecodedIndex(const QModelIndex& index) noexcept;
    [[nodiscard]] std::optional<TriggerDirectoryItem> DecodeMime(const QMimeData* data) const;

    const War3Map::Trigger::TriggerDocument* document_ = nullptr;
    std::vector<CategoryGroup> groups_;
    std::vector<std::size_t> triggerGroupRows_;
    // 含禁用语句的触发器（原版“部分禁用”图标）。
    std::vector<char> partial_;
    // 上次重置时的目录结构：分类编号顺序、触发器身份及所属分类顺序。
    std::vector<std::int32_t> structureCategories_;
    std::vector<std::pair<War3Map::StableId, std::int32_t>> structureTriggers_;
    // 上次通知视图时各行显示用到的字段；结构未变时只通知真正变化的行。
    struct ShownTrigger {
        std::string name;
        std::int32_t type = 0, enabled = 0, initiallyClosed = 0, customTextFlag = 0;
        char partial = 0;
        bool operator==(const ShownTrigger&) const = default;
    };
    std::vector<std::string> shownCategoryNames_;
    std::vector<ShownTrigger> shownTriggers_;
    [[nodiscard]] ShownTrigger Shown(std::size_t triggerIndex) const;
    void ComputePartial();
    std::set<std::int32_t> expanded_;
    War3Map::QtApp::IconProvider* iconProvider_ = nullptr;
    RenameHandler renameHandler_;
    MoveHandler moveHandler_;
};

// 语句可以放入的位置：触发器根的某一组（事件 / 条件 / 动作），或某个块语句的一个分支。
struct EcaSlot {
    War3Map::StableId parentEcaId = 0; // 0 = 触发器根
    std::optional<std::int32_t> childId;
    std::int32_t type = 2;             // 该位置接受的语句类型：0 事件，1 条件，2 动作
    QString label;
};

// 拖放 / 移动请求：把 ecaId 放到 slot 中 beforeEcaId 之前（0 = 末尾）。
struct EcaMoveRequest {
    War3Map::StableId ecaId = 0;
    EcaSlot slot;
    War3Map::StableId beforeEcaId = 0;
};

class TriggerEcaModel final : public QAbstractItemModel {
public:
    enum Role {
        StableIdRole = Qt::UserRole + 1,
        NodeIndexRole,
        // 参数记号：QVariantList，每项 {begin, length（UTF-16）, position, empty, call, type}。
        ArgumentSpansRole,
    };
    using MoveHandler = std::function<void(const EcaMoveRequest& request)>;

    explicit TriggerEcaModel(QObject* parent = nullptr);

    void SetDocument(
        const War3Map::Trigger::TriggerDocument* document,
        const War3Map::Trigger::UiCatalog* catalog = nullptr);
    void SetTrigger(War3Map::StableId triggerId);
    [[nodiscard]] bool IsBoundTo(const War3Map::Trigger::TriggerDocument* document,
        const War3Map::Trigger::UiCatalog* catalog) const noexcept;
    // 只用于已密封 scalar replay：保留结构/索引，整批丢弃呈现缓存并发实际 dataChanged。
    [[nodiscard]] bool InvalidateScalarPresentations();
    [[nodiscard]] std::uint64_t PresentationEpoch() const noexcept { return presentationTopologyEpoch_; }
    [[nodiscard]] War3Map::StableId BoundTriggerId() const noexcept { return triggerId_; }
    // 图标服务（真实游戏图标）。指针生命周期由 MainWindow 持有，可为空。
    void SetIconProvider(War3Map::QtApp::IconProvider* provider);
    // 物体名称解析器（rawcode → 显示名）：设置后参数中的四字码显示为物体名称。
    void SetObjectNameResolver(
        std::function<std::string(std::string_view rawcode)> resolver);
    void SetPresentationOptions(War3Map::Trigger::PresentationOptions options);
    [[nodiscard]] const War3Map::Trigger::TriggerTypeSystem* TypeSystem() const noexcept { return typeSystem_.get(); }
    [[nodiscard]] const War3Map::Trigger::EcaNode* EcaAt(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] const War3Map::Trigger::ArgumentNode* ArgumentAt(
        const QModelIndex& index,
        std::size_t argumentRow) const noexcept;
    [[nodiscard]] QModelIndex IndexForStableId(
        War3Map::StableId ecaId,
        int column = 0) const;
    // 分组头 / 分支头对应的放置位置；语句行返回 nullopt。
    [[nodiscard]] std::optional<EcaSlot> SlotAt(const QModelIndex& index) const;
    // 语句所在的位置（它的父分组或父分支）。
    [[nodiscard]] std::optional<EcaSlot> SlotOf(War3Map::StableId ecaId) const;
    // 块语句的全部分支（含空分支），用于“添加到子块”。
    [[nodiscard]] std::vector<EcaSlot> BranchesOf(War3Map::StableId ecaId) const;
    // 同一位置内的兄弟语句（按显示顺序）。
    [[nodiscard]] std::vector<War3Map::StableId> SiblingsOf(War3Map::StableId ecaId) const;
    void SetMoveHandler(MoveHandler handler) { moveHandler_ = std::move(handler); }
    // 拖放语义（供测试直接调用）：parent/row 与 QAbstractItemView 的放置位置一致。
    [[nodiscard]] std::optional<EcaMoveRequest> ResolveDrop(
        War3Map::StableId ecaId, int row, const QModelIndex& parent) const;

    [[nodiscard]] QModelIndex index(
        int row,
        int column,
        const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) override;

private:
    // 函数体内按 childId（分支号）划分的角色分组：0=If 条件/循环体、1=Then、2=Else。
    // 声明的分支即使没有子节点也存在（children 为空）。
    struct RoleGroup {
        std::int32_t branch = 0;
        std::int32_t type = 2;
        std::vector<std::uint32_t> children;
    };
    [[nodiscard]] EcaSlot SlotForGroup(std::uint32_t parentNode, const RoleGroup& group) const;
    [[nodiscard]] QIcon KindIcon(std::int32_t type, bool disabled) const;
    [[nodiscard]] std::size_t NodeIndexOf(War3Map::StableId ecaId) const;

    [[nodiscard]] std::size_t NodeIndex(const QModelIndex& index) const noexcept;
    [[nodiscard]] const War3Map::Trigger::EcaPresentation& Presentation(
        std::size_t nodeIndex) const;
    [[nodiscard]] std::vector<RoleGroup> RoleGroupsFor(std::uint32_t nodeIndex) const;
    void RebuildParentIndex();

    const War3Map::Trigger::TriggerDocument* document_ = nullptr;
    const War3Map::Trigger::UiCatalog* catalog_ = nullptr;
    War3Map::QtApp::IconProvider* iconProvider_ = nullptr;
    War3Map::StableId triggerId_ = 0;
    std::vector<std::uint32_t> roots_;
    std::vector<std::uint32_t> parents_;
    std::vector<std::uint32_t> rowsInParent_;
    // 事件/条件/动作 三个顶层分组桁（按类型划分顶层 ECA）。
    std::array<std::vector<std::uint32_t>, 3> groupBuckets_;
    // 每个节点：若为顶层节点则为其分组 0/1/2，否则 0xFF（嵌套子节点）。
    std::vector<std::uint8_t> topLevelGroup_;
    mutable std::unordered_map<std::size_t, War3Map::Trigger::EcaPresentation>
        presentationCache_;
    std::function<std::string(std::string_view)> objectNameResolver_;
    War3Map::Trigger::PresentationOptions presentationOptions_;
    std::unique_ptr<War3Map::Trigger::TriggerTypeSystem> typeSystem_;
    std::string typeCatalogSha256_; // 只绑定目录；SetDocument 仍重建实际文档/结构和显示缓存。
    std::uint64_t presentationTopologyEpoch_ = 0;
    MoveHandler moveHandler_;
};
