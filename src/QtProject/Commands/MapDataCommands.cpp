#include "MapDataCommands.h"

#include "EditorProject.h"

#include <QUndoStack>
#include <QSignalBlocker>

#include <exception>

namespace War3Map::QtProject {

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description)
    : QUndoCommand(std::move(text)), project_(project), receipt_(std::move(receipt)), description_(std::move(description)) {}

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description, std::shared_ptr<HistoryInstallation> installation)
    : MapDataEditCommand(project, std::move(receipt), std::move(text), std::move(description)) {
    installation_ = std::move(installation);
}

void MapDataEditCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    Apply(true);
}

void MapDataEditCommand::Apply(bool forward) {
    if (installation_ && installation_->active) return;
    if (!project_.mapEdits_) return;
    const auto status = project_.mapEdits_->Replay(receipt_, forward);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics); return; }
    (void)project_.ObserveMapDataEdit(description_);
}

void PushMapDataEdit(EditorProject& project, const QString& text, const std::string& description) {
    if (!project.mapEdits_) return;
    if (project.mapDataUndoStack_)
        project.mapDataUndoStack_->push(new MapDataEditCommand(project, project.mapEdits_->LastEdit(), text, description));
}

Status InstallRestoredMapDataHistory(EditorProject& project,
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts,
    std::size_t currentIndex, std::optional<std::size_t> savedIndex) {
    constexpr std::size_t maximumCommands = 128;
    if (!project.mapEdits_ || !project.mapDataUndoStack_ || receipts.size() > maximumCommands
        || currentIndex > receipts.size() || (savedIndex && *savedIndex > receipts.size())
        || (project.mapDataUndoStack_->undoLimit() > 0
            && receipts.size() > static_cast<std::size_t>(project.mapDataUndoStack_->undoLimit()))) {
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_INVALID",
            "恢复的地形历史数量、当前位置或保存位置超出当前撤销栈范围。");
    }
    for (const auto& receipt : receipts) {
        if (receipt.SessionIdentity() != project.mapEdits_->SessionIdentity())
            return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_IDENTITY",
                "恢复的地形撤销回执不属于当前实际会话，未安装历史。");
    }
    const QSignalBlocker historySignals(project.mapDataUndoStack_);
    std::shared_ptr<MapDataEditCommand::HistoryInstallation> installation;
    std::vector<std::unique_ptr<MapDataEditCommand>> commands;
    try {
        installation = std::make_shared<MapDataEditCommand::HistoryInstallation>();
        commands.reserve(receipts.size());
        for (auto& receipt : receipts)
            commands.push_back(std::unique_ptr<MapDataEditCommand>(new MapDataEditCommand(project, std::move(receipt),
                EditorProject::tr("恢复的地形编辑"), "恢复工程地形历史", installation)));
    } catch (const std::exception&) {
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_BUDGET",
            "恢复地形撤销栈超出可用预算，当前草稿保留。");
    }
    try {
        project.mapDataUndoStack_->clear();
        for (auto& command : commands) project.mapDataUndoStack_->push(command.release());
        if (savedIndex) {
            project.mapDataUndoStack_->setIndex(static_cast<int>(*savedIndex));
            project.mapDataUndoStack_->setClean();
        } else project.mapDataUndoStack_->resetClean();
        project.mapDataUndoStack_->setIndex(static_cast<int>(currentIndex));
    } catch (const std::exception&) {
        project.mapDataUndoStack_->clear(); installation->active = false;
        return Status::Failure("Terrain.History.Qt", "TERRAIN_HISTORY_STACK_BUDGET",
            "安装地形撤销栈超出可用预算，当前草稿保留且未执行回执。");
    }
    installation->active = false;
    return Status::Ok();
}

} // namespace War3Map::QtProject
