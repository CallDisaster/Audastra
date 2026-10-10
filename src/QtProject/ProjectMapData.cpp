#include "EditorProject.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"
#include <QDir>
#include <QUndoStack>

// 地图数据（地图信息、放置物、区域、镜头、声音等）：按当前来源重新读取只读文档。
namespace War3Map::QtProject {

void EditorProject::LoadMapDocument(bool afterMapSave) {
    if (!afterMapSave && mapDataUndoStack_) mapDataUndoStack_->clear();
    const auto report = [this](const Diagnostic& diagnostic, bool error) {
        AddDiagnostic(error ? tr("错误") : tr("警告"), tr("地图数据"),
            QString::fromUtf8(diagnostic.code), QString::fromUtf8(diagnostic.message));
    };
    mapDocument_.reset();
    if (archiveSession_) {
        mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*archiveSession_));
    } else if (!sourceArchivePath_.empty()) {
        auto read = MapDocument::OpenReadOnlyArchive(sourceArchivePath_);
        for (const auto& diagnostic : read.diagnostics) report(diagnostic, true);
        if (read.Succeeded()) mapDocument_ = std::make_unique<MapDocument>(std::move(*read.value));
    } else if (directorySnapshot_) {
        auto read = MapDocument::FromDirectory(*directorySnapshot_);
        for (const auto& diagnostic : read.diagnostics) report(diagnostic, true);
        if (read.Succeeded()) mapDocument_ = std::make_unique<MapDocument>(std::move(*read.value));
    }
    if (mapDocument_) {
        if (afterMapSave && mapEdits_) mapEdits_->CommitSaved(*mapDocument_);
        else {
            mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*mapDocument_);
            // 地形不在 MapDocument 中：从同一来源附上；读取或解析失败时地形只读显示，不可编辑。
            auto terrain = ReadMapMember("war3map.w3e");
            if (terrain.Succeeded() && terrain.value) {
                const auto attached = mapEdits_->AttachTerrain(*terrain.value);
                for (const auto& diagnostic : attached.diagnostics) report(diagnostic, false);
                if (attached.Succeeded()) {
                    auto pathing = ReadMapMember("war3map.wpm");
                    if (pathing.Succeeded() && pathing.value) {
                        const auto status = mapEdits_->AttachPathing(*pathing.value);
                        for (const auto& diagnostic : status.diagnostics) report(diagnostic, false);
                    } else for (const auto& diagnostic : pathing.diagnostics) report(diagnostic, false);
                }
            } else for (const auto& diagnostic : terrain.diagnostics) report(diagnostic, false);
        }
        for (const auto& diagnostic : mapDocument_->Diagnostics()) {
            report(diagnostic, diagnostic.severity == DiagnosticSeverity::Error);
        }
    } else mapEdits_.reset();
    emit MapDocumentChanged();
}

Result<std::vector<std::byte>> EditorProject::ReadMapMember(const std::string& name) const {
    const auto fail = [](std::string message) {
        return Result<std::vector<std::byte>>::Failure("MapData", "MAP_MEMBER_UNAVAILABLE", std::move(message));
    };
    if (archiveSession_) {
        if (!archiveSession_->HasMember(name)) return fail("当前地图没有 " + name + "。");
        return archiveSession_->ReadMember(name);
    }
    if (!sourceArchivePath_.empty()) return fail("地图归档会话不可用，不能从另一来源推测 " + name + "。");
    if (!directorySnapshot_) return fail("当前工作区缺少可信来源快照，不能读取 " + name + "。");
    std::error_code error;
    if (projectRoot_.empty() || !std::filesystem::equivalent(projectRoot_, directorySnapshot_->Snapshot().root, error) || error)
        return fail("当前工作区路径与来源快照不一致，不能读取 " + name + "。");
    return ReadWorkspaceMember(*directorySnapshot_, name);
}

Result<std::vector<std::byte>> EditorProject::ReadWorkspaceMember(const DirectoryWorkspace& workspace, const std::string& name) {
    const auto fail = [](std::string message) {
        return Result<std::vector<std::byte>>::Failure("MapData", "MAP_MEMBER_UNAVAILABLE", std::move(message));
    };
    const auto expected = QString::fromStdString(name).toCaseFolded();
    const auto nested = QStringLiteral("map/") + expected;
    const WorkspaceFile* selected = nullptr;
    for (const auto& file : workspace.Snapshot().files) {
        const auto relative = QDir::fromNativeSeparators(QString::fromStdWString(file.relativePath.wstring())).toCaseFolded();
        if (relative != expected && relative != nested) continue;
        if (selected) return fail("目录同时存在多个 " + name + " 候选，来源不明确。");
        selected = &file;
    }
    if (!selected) return fail("当前地图来源快照没有 " + name + "。");
    const std::uint64_t maxBytes = name == "war3map.wpm" ? 64ULL * 1024 * 1024 + 16 : 64ULL * 1024 * 1024;
    if (selected->size > maxBytes) return fail(name + " 超过 64 MiB 读取上限。");
    auto bytes = ReadVerifiedFile(workspace.Snapshot().root / selected->relativePath, selected->sha256, maxBytes);
    if (!bytes.Succeeded() || !bytes.value) return bytes;
    if (bytes.value->size() != selected->size) return fail(name + " 的长度与工作区快照不同。");
    return bytes;
}

} // namespace War3Map::QtProject
