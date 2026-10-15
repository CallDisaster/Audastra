#pragma once

#include "Modules/EditorModule.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"

#include <QObject>
#include <QPointer>

#include <array>

class QTabWidget;
class QTableWidget;
class QCheckBox;
namespace War3Map::QtProject { class EditorProject; struct AuxiliaryEditBinding; }

namespace War3Map::Modules {

// 阶段 D 的数值工作区：现有放置物/区域/镜头与同一地图数据编辑会话。
class MapEntitiesModule final : public QObject, public Shell::EditorModule {
public:
    explicit MapEntitiesModule(QtProject::EditorProject& project);
    ~MapEntitiesModule() override;
    [[nodiscard]] Shell::ModuleInfo Info() const override;
    [[nodiscard]] QWidget* Widget() override;
    [[nodiscard]] QUndoStack* UndoStack() override;
    [[nodiscard]] bool Reveal(const Shell::NavigationTarget& target) override;
    [[nodiscard]] QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    [[nodiscard]] std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    [[nodiscard]] Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    [[nodiscard]] bool PrepareNavigationLeave() override { return !editingCell_; }
    void Refresh();

    // 供表格编辑和 Qt 回归共用；失败不变更文档或撤销栈。
    [[nodiscard]] Status EditCell(MapDataKind kind, std::size_t row,
        std::string_view field, std::string_view value, const QtProject::AuxiliaryEditBinding* auxiliaryBinding = nullptr);
    [[nodiscard]] Status ApplySerialized(MapDataKind kind, std::span<const std::byte> bytes);
    [[nodiscard]] Status CreateAuxiliary(MapData::RegionRecord record);
    [[nodiscard]] Status CreateAuxiliary(MapData::CameraRecord record);
    [[nodiscard]] Status DeleteAuxiliary(MapDataKind kind, std::size_t row);

private:
    void EditSelected(int category, int row, int column);
    void RefreshVisibleIcons(int category);
    void AddAuxiliary();
    void ShowError(const Status& status);
    QtProject::EditorProject& project_;
    QPointer<QWidget> page_;
    QTabWidget* tabs_ = nullptr;
    QCheckBox* rawFields_ = nullptr;
    std::array<QTableWidget*, 4> tables_{};
    bool editingCell_ = false;
};

} // namespace War3Map::Modules
