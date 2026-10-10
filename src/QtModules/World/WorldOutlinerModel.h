#pragma once

#include "War3MapCore/Services/WorldSelection.h"

#include <QAbstractItemModel>

#include <functional>
#include <memory>

namespace War3Map::Modules {

// QTreeView 的有界薄适配；typed token/version 由核心给出，绝不在模型里修改地图。
class WorldOutlinerModel final : public QAbstractItemModel {
public:
    using PageReader = std::function<Result<Services::WorldObjectPage>(MapDataKind,
        const Services::WorldObjectFilter&, const std::optional<Services::WorldObjectCursor>&,
        const Services::WorldObjectQueryOptions&)>;
    using FlagWriter = std::function<Result<Services::WorldSelectionSnapshot>(
        const Services::WorldSelectionVersion&, std::span<const Services::WorldEditorFlagEdit>)>;
    using DiagnosticReader = std::function<QString(const Services::WorldSelectionVersion&,
        const Services::WorldObjectId&)>;
    explicit WorldOutlinerModel(QObject* parent = nullptr);
    ~WorldOutlinerModel() override;
    // 清除旧行/选择令牌/游标/回调；新 provider 必须查询宿主当前 overlay。
    void SetSource(PageReader reader, FlagWriter writer = {});
    // 按悬停行惰性读取当前场景；SetSource 清除旧 provider，pose tick 不重建目录。
    void SetDiagnosticReader(DiagnosticReader reader);
    void SetFilter(Services::WorldObjectFilter filter);
    void Refresh();
    // 已由共享服务验证的同文档快照：只同步选择及 editorRevision，不重置展开/滚动。
    [[nodiscard]] bool ApplySnapshot(const Services::WorldSelectionSnapshot& snapshot);
    [[nodiscard]] const Services::WorldObjectRow* RowAt(const QModelIndex& index) const;
    [[nodiscard]] const std::optional<Services::WorldSelectionVersion>& Version() const noexcept { return version_; }
    [[nodiscard]] QModelIndex Find(const Services::WorldObjectId& id) const;
    [[nodiscard]] QString StatusText() const;
    [[nodiscard]] bool Partial() const noexcept { return partial_; }
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

private:
    struct Node;
    [[nodiscard]] Node* At(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex IndexOf(const Node* node, int column = 0) const;
    [[nodiscard]] Node* Group(Services::WorldCategory category, std::optional<std::int32_t> owner);
    void ResetRows();
    PageReader reader_;
    FlagWriter writer_;
    DiagnosticReader diagnosticReader_;
    Services::WorldObjectFilter filter_;
    std::optional<Services::WorldSelectionVersion> version_;
    std::unique_ptr<Node> root_;
    std::optional<Services::WorldObjectCursor> cursor_;
    std::size_t source_ = 0, loaded_ = 0;
    bool partial_ = false, fetching_ = false;
    QString status_;
};

} // namespace War3Map::Modules
