#include "MapDataCommands.h"

#include "EditorProject.h"

#include <QUndoStack>
#include <QSignalBlocker>

#include <algorithm>
#include <array>
#include <exception>

namespace War3Map::QtProject {

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description)
    : QUndoCommand(std::move(text)), project_(project), receipt_(std::move(receipt)), description_(std::move(description)) {}

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description, QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent), project_(project), receipt_(std::move(receipt)),
      description_(std::move(description)) {}

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description, std::shared_ptr<HistoryInstallation> installation)
    : MapDataEditCommand(project, std::move(receipt), std::move(text), std::move(description)) {
    installation_ = std::move(installation);
}

void MapDataEditCommand::undo() {
    if (!TryApply(false)) RefuseMapDataReplay(project_, *this, std::span(&receipt_, 1), false);
}

void MapDataEditCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    if (!TryApply(true)) RefuseMapDataReplay(project_, *this, std::span(&receipt_, 1), true);
}

bool MapDataEditCommand::TryApply(bool forward) {
    if (installation_ && installation_->active) return true;
    if (!project_.mapEdits_) return true;
    const auto status = project_.mapEdits_->Replay(receipt_, forward);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics); return false; }
    (void)project_.ObserveMapDataEdit(description_);
    return true;
}

MapDataEditGroupCommand::MapDataEditGroupCommand(EditorProject& project, QString text)
    : QUndoCommand(std::move(text)), project_(project) {}

void MapDataEditGroupCommand::undo() { (void)ApplyChildren(false); }

void MapDataEditGroupCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    (void)ApplyChildren(true);
}

bool MapDataEditGroupCommand::ApplyChildren(bool forward) {
    std::vector<MapDataEditCommand*> children;
    for (int i = 0; i < childCount(); ++i)
        if (auto* entry = dynamic_cast<MapDataEditCommand*>(const_cast<QUndoCommand*>(child(i)))) children.push_back(entry);
    if (!forward) std::reverse(children.begin(), children.end());
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (children[i]->TryApply(forward)) continue;
        // 刚重放的子事务按相反顺序恢复；它们的字节就是刚写入的，逐条核对必然一致。
        bool restored = true;
        for (std::size_t j = i; j-- > 0;) restored = children[j]->TryApply(!forward) && restored;
        if (!restored)
            project_.AddDiagnostics(Status::Failure("MapData.Undo", "MAP_UNDO_GROUP_ROLLBACK",
                "组合撤销步骤被拒绝后未能完整恢复已重放的部分；请检查地图数据。").diagnostics);
        std::vector<MapData::MapDataEditSession::EditReceipt> receipts;
        receipts.reserve(children.size());
        for (const auto* child : children) receipts.push_back(child->Receipt());
        RefuseMapDataReplay(project_, *this, receipts, forward);
        return false;
    }
    return true;
}

void RefuseMapDataReplay(EditorProject& project, QUndoCommand& command,
    std::span<const MapData::MapDataEditSession::EditReceipt> receipts, bool forward) {
    // 该步状态保持不变：撤销被拒时仍是已应用，重做被拒时仍未应用。移出外层栈后，
    // 会话改用私有分区记录它，不让其他命令的后进先出核对受它牵连。
    if (project.mapEdits_)
        for (const auto& receipt : receipts) (void)project.mapEdits_->DetachEditLane(receipt);
    const auto text = command.text().toStdString();
    project.AddDiagnostics(Status::Failure("MapData.Undo", forward ? "MAP_REDO_REFUSED" : "MAP_UNDO_REFUSED",
        std::string(forward ? "无法重做“" : "无法撤销“") + text
            + "”：其他页面之后修改了同一份地图数据。地图保持不变，该步已从撤销列表移除。").diagnostics);
    command.setObsolete(true);
}

void PushMapDataEdit(EditorProject& project, MapData::MapDataEditLane lane, const QString& text,
    const std::string& description) {
    if (!project.mapEdits_) return;
    const auto receipt = project.mapEdits_->LastEdit();
    auto* stack = project.MapDataUndoStack(lane);
    if (!stack) return;
    const auto assigned = project.mapEdits_->AssignEditLane(receipt, lane);
    if (!assigned.Succeeded()) {
        // 不能登记到发起页面的分区（如含地形的命令）时放入世界分区，仍可撤销。
        project.AddDiagnostics(assigned.diagnostics);
        lane = MapData::MapDataEditLane::World;
        (void)project.mapEdits_->AssignEditLane(receipt, lane);
        stack = project.MapDataUndoStack(lane);
        if (!stack) return;
    }
    stack->push(new MapDataEditCommand(project, receipt, text, description));
}

void PushMapDataEdit(EditorProject& project, const QString& text, const std::string& description) {
    if (!project.mapEdits_) return;
    PushMapDataEdit(project, project.mapEdits_->EditLane(project.mapEdits_->LastEdit()), text, description);
}

Status InstallRestoredMapDataHistory(EditorProject& project,
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
    std::size_t currentIndex, std::optional<std::size_t> savedIndex) {
    constexpr std::size_t maximumCommands = 128;
    const auto stacks = project.MapDataUndoStacks();
    const bool stacksReady = std::all_of(stacks.begin(), stacks.end(), [](const auto* stack) { return stack != nullptr; });
    if (!project.mapEdits_ || !stacksReady || receipts.size() > maximumCommands
        || currentIndex > receipts.size() || (savedIndex && *savedIndex > receipts.size())
        || std::any_of(stacks.begin(), stacks.end(), [&](const auto* stack) {
            return stack && stack->undoLimit() > 0 && receipts.size() > static_cast<std::size_t>(stack->undoLimit());
        })) {
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_INVALID",
            "恢复的地形历史数量、当前位置或保存位置超出当前撤销栈范围。");
    }
    for (const auto& receipt : receipts) {
        if (receipt.SessionIdentity() != project.mapEdits_->SessionIdentity())
            return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_IDENTITY",
                "恢复的地形撤销回执不属于当前实际会话，未安装历史。");
    }
    const QSignalBlocker worldSignals(stacks[0]), settingsSignals(stacks[1]), soundSignals(stacks[2]);
    std::shared_ptr<MapDataEditCommand::HistoryInstallation> installation;
    std::array<std::vector<std::unique_ptr<MapDataEditCommand>>, MapData::MapDataEditLaneCount> commands;
    std::array<std::size_t, MapData::MapDataEditLaneCount> current{}, saved{};
    try {
        installation = std::make_shared<MapDataEditCommand::HistoryInstallation>();
        for (std::size_t i = 0; i < receipts.size(); ++i) {
            // 工程字节不记录分区：按事务触及的成员推断，并登记到会话以便之后的导出沿用。
            const auto lane = project.mapEdits_->EditLane(receipts[i]);
            const auto assigned = project.mapEdits_->AssignEditLane(receipts[i], lane);
            if (!assigned.Succeeded()) return assigned;
            const auto index = static_cast<std::size_t>(lane);
            if (i < currentIndex) ++current[index];
            if (savedIndex && i < *savedIndex) ++saved[index];
            commands[index].push_back(std::unique_ptr<MapDataEditCommand>(new MapDataEditCommand(project,
                std::move(receipts[i]), EditorProject::tr("恢复的地形编辑"), "恢复工程地形历史", installation)));
        }
    } catch (const std::exception&) {
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_BUDGET",
            "恢复地形撤销栈超出可用预算，当前草稿保留。");
    }
    try {
        for (std::size_t lane = 0; lane < stacks.size(); ++lane) {
            auto* stack = stacks[lane];
            stack->clear();
            for (auto& command : commands[lane]) stack->push(command.release());
            if (savedIndex) {
                stack->setIndex(static_cast<int>(saved[lane]));
                stack->setClean();
            } else stack->resetClean();
            stack->setIndex(static_cast<int>(current[lane]));
        }
    } catch (const std::exception&) {
        for (auto* stack : stacks) stack->clear();
        installation->active = false;
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_BUDGET",
            "安装地形撤销栈超出可用预算，当前草稿保留且未执行回执。");
    }
    installation->active = false;
    return Status::Ok();
}

void NotifyRestoredMapDataHistoryState(EditorProject& project) {
    for (auto* stack : project.MapDataUndoStacks()) {
        if (!stack) continue;
        // index观察者会同步读取工程；只能由完整恢复后的发布边界调用。
        emit stack->indexChanged(stack->index());
        emit stack->cleanChanged(stack->isClean());
        emit stack->canUndoChanged(stack->canUndo());
        emit stack->canRedoChanged(stack->canRedo());
        emit stack->undoTextChanged(stack->undoText());
        emit stack->redoTextChanged(stack->redoText());
    }
}

} // namespace War3Map::QtProject
