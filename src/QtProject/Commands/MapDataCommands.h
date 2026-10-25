#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"

#include <QString>
#include <QUndoCommand>

#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace War3Map::QtProject {
class EditorProject;

// 公共地图数据（设置、实例、区域、镜头、声音、世界视图摆放）共用的撤销命令。
// 推入时编辑已经应用；undo / redo 按事务回执重放，失败写入诊断，不重新推导逆操作。
class MapDataEditCommand final : public QUndoCommand {
public:
    MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt, QString text,
        std::string description);
    void undo() override { Apply(false); }
    void redo() override;

private:
    struct HistoryInstallation { bool active = true; };
    MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
        QString text, std::string description, std::shared_ptr<HistoryInstallation> installation);
    friend Status InstallRestoredMapDataHistory(EditorProject& project,
        std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
        std::size_t currentIndex, std::optional<std::size_t> savedIndex);
    void Apply(bool forward);
    EditorProject& project_;
    MapData::MapDataEditSession::EditReceipt receipt_;
    std::string description_;
    bool firstRedo_ = true;
    std::shared_ptr<HistoryInstallation> installation_;
};

// 把 mapEdits_ 最近一次已应用的编辑推入共用撤销栈（调用方已更新编辑状态）。
void PushMapDataEdit(EditorProject& project, const QString& text, const std::string& description);

// Services已把会话恢复到真实current位置；安装Qt逻辑栈时不得额外执行任何回执。
// 保存位置在同一线性栈内才设置clean；安装结束后所有撤销/重做恢复正常Replay。
[[nodiscard]] Status InstallRestoredMapDataHistory(EditorProject& project,
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
    std::size_t currentIndex, std::optional<std::size_t> savedIndex);

} // namespace War3Map::QtProject
