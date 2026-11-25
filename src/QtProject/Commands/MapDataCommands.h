#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"

#include <QString>
#include <QUndoCommand>

#include <string>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace War3Map::QtProject {
class EditorProject;

// 公共地图数据撤销命令。世界页（地形、摆放、区域、镜头）、地图设置页和声音页各有一个撤销栈，
// 共用同一编辑会话。推入时编辑已经应用；undo / redo 按事务回执重放，不重新推导逆操作。
// 会话拒绝重放（另一页面已改过同一数据）时地图不变，该步从撤销栈移除并写入诊断。
class MapDataEditCommand final : public QUndoCommand {
public:
    MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt, QString text,
        std::string description);
    // 作为组合撤销步骤（MapDataEditGroupCommand）的子命令：由组按相反顺序撤销并负责拒绝时回滚。
    MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt, QString text,
        std::string description, QUndoCommand* parent);
    void undo() override;
    void redo() override;
    // 只重放回执；返回false表示会话拒绝，地图未改动且已写入诊断。
    [[nodiscard]] bool TryApply(bool forward);
    [[nodiscard]] const MapData::MapDataEditSession::EditReceipt& Receipt() const noexcept { return receipt_; }

private:
    struct HistoryInstallation { bool active = true; };
    MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
        QString text, std::string description, std::shared_ptr<HistoryInstallation> installation);
    friend Status InstallRestoredMapDataHistory(EditorProject& project,
        std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
        std::size_t currentIndex, std::optional<std::size_t> savedIndex);
    EditorProject& project_;
    MapData::MapDataEditSession::EditReceipt receipt_;
    std::string description_;
    bool firstRedo_ = true;
    std::shared_ptr<HistoryInstallation> installation_;
};

// 多个会话事务合成的一步撤销；子命令按相反顺序撤销。任一子事务被拒绝时，
// 已重放的子事务立即反向恢复，整步移出撤销栈，地图保持按下撤销前的状态。
class MapDataEditGroupCommand final : public QUndoCommand {
public:
    MapDataEditGroupCommand(EditorProject& project, QString text);
    void undo() override;
    void redo() override;

private:
    [[nodiscard]] bool ApplyChildren(bool forward);
    EditorProject& project_;
    bool firstRedo_ = true;
};

// 会话拒绝重放后的统一处理：写入说明，把命令移出外层撤销栈，并让会话不再按该栈核对它。
// 供地图设置页等自带撤销命令的模块共用。
void RefuseMapDataReplay(EditorProject& project, QUndoCommand& command,
    std::span<const MapData::MapDataEditSession::EditReceipt> receipts, bool forward);

// 把 mapEdits_ 最近一次已应用的编辑登记到指定分区并推入该分区的撤销栈（调用方已更新编辑状态）。
void PushMapDataEdit(EditorProject& project, MapData::MapDataEditLane lane, const QString& text,
    const std::string& description);
// 未指明分区的入口（修改面板、MCP 字段编辑等）：按事务实际触及的成员归入世界、设置或声音分区。
void PushMapDataEdit(EditorProject& project, const QString& text, const std::string& description);

// Services已把会话恢复到真实current位置；安装Qt逻辑栈时不得额外执行任何回执。
// 回执按会话推断的分区分发到三个撤销栈，各栈的当前与保存位置取线性历史中对应前缀的数量。
[[nodiscard]] Status InstallRestoredMapDataHistory(EditorProject& project,
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
    std::size_t currentIndex, std::optional<std::size_t> savedIndex);

// 安装本身保持静默；调用方完成全部文档、导入和修订基线恢复后，再发布一次最终栈状态。
// 仅通知界面和观察者，不重放命令、不修改Core修订；调用方已有的信号屏蔽仍受尊重。
void NotifyRestoredMapDataHistoryState(EditorProject& project);

} // namespace War3Map::QtProject
