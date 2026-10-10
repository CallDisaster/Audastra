#include "MapDataCommands.h"

#include "EditorProject.h"

#include <QUndoStack>

namespace War3Map::QtProject {

MapDataEditCommand::MapDataEditCommand(EditorProject& project, MapData::MapDataEditSession::EditReceipt receipt,
    QString text, std::string description)
    : QUndoCommand(std::move(text)), project_(project), receipt_(std::move(receipt)), description_(std::move(description)) {}

void MapDataEditCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    Apply(true);
}

void MapDataEditCommand::Apply(bool forward) {
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

} // namespace War3Map::QtProject
