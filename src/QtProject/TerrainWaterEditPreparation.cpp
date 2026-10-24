#include "TerrainWaterEditPreparation.h"

#include "EditorProject.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <QDir>
#include <algorithm>
#include <exception>

namespace War3Map::QtProject {
namespace {
constexpr std::size_t kMaximumBytes = 64u * 1024u * 1024u;

bool SameSource(const EditorProject& project, const TerrainWaterEditRequest& request) {
    if (project.projectRoot_ != request.projectRoot || project.war3GameRoot_ != request.gameRoot
        || project.sourceArchivePath_ != request.archivePath) return false;
    if (!request.archivePath.empty())
        return project.archiveSession_ && project.archiveSession_ == request.archive
            && project.archiveSession_->SourcePath() == request.sourcePath
            && project.archiveSession_->SourceSha256() == request.sourceSha256;
    return request.directory && project.directorySnapshot_
        && project.directorySnapshot_->Snapshot().root == request.sourcePath
        && project.directorySnapshot_->Snapshot().aggregateSha256 == request.sourceSha256;
}

Status VerifySourcePath(const TerrainWaterEditRequest& request) {
    const auto& openedPath = request.archivePath.empty() ? request.projectRoot : request.archivePath;
    std::error_code error;
    const bool same = !openedPath.empty() && !request.sourcePath.empty()
        && std::filesystem::equivalent(openedPath, request.sourcePath, error);
    if (error || !same)
        return Status::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_CHANGED",
            "当前打开路径与地形编辑来源快照不再指向同一文件或目录，请重新打开地图。");
    return Status::Ok();
}

Result<std::vector<std::byte>> ReadDirectoryTerrain(const WorkspaceSnapshot& source, std::size_t maximumBytes) {
    const WorkspaceFile* selected = nullptr;
    for (const auto& file : source.files) {
        const auto path = QDir::fromNativeSeparators(QString::fromStdWString(file.relativePath.wstring())).toCaseFolded();
        if (path != QStringLiteral("war3map.w3e") && path != QStringLiteral("map/war3map.w3e")) continue;
        if (selected) return Result<std::vector<std::byte>>::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_AMBIGUOUS",
            "目录存在多个地形来源，请先解决来源冲突。");
        selected = &file;
    }
    if (!selected) return Result<std::vector<std::byte>>::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_MISSING",
        "目录来源快照缺少war3map.w3e。");
    if (selected->size > maximumBytes) return Result<std::vector<std::byte>>::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_BUDGET",
        "地形来源超过本次水刷读取预算。");
    auto bytes = ReadVerifiedFile(source.root / selected->relativePath, selected->sha256, maximumBytes);
    if (bytes.Succeeded() && bytes.value->size() != selected->size)
        return Result<std::vector<std::byte>>::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_CHANGED", "地形来源长度已变化。");
    return bytes;
}
} // namespace

Status VerifyTerrainWaterEditRequestSourcePath(const TerrainWaterEditRequest& request) {
    return VerifySourcePath(request);
}

Result<TerrainWaterEditRequest> CaptureTerrainWaterEditRequest(const EditorProject& project,
    bool enforceWaterLimits, std::size_t maximumBytes, bool captureDoodads) {
    const auto fail = [](const char* code, const char* message) {
        return Result<TerrainWaterEditRequest>::Failure("Terrain.Water", code, message);
    };
    if (!maximumBytes || maximumBytes > kMaximumBytes)
        return fail("TERRAIN_WATER_CAPTURE_BUDGET", "水刷来源预算无效。");
    if (!project.mapEdits_ || !project.mapEdits_->Terrain())
        return fail("TERRAIN_WATER_SESSION_REQUIRED", "请先打开具有可编辑地形的地图。");
    if (project.HasPendingAssetEdits())
        return fail("TERRAIN_WATER_PENDING_RESOURCES", "存在未保存的资源修改；请先保存或撤销资源修改，再使用浅水或深水笔刷。");
    if (project.sourceChangeSuspected_ || project.sourceChangeConfirmed_)
        return fail("TERRAIN_WATER_SOURCE_CHANGED", "地图来源可能已变化，请先检查来源或重新打开地图。");
    try {
        TerrainWaterEditRequest request;
        request.projectRoot = project.projectRoot_;
        request.archivePath = project.sourceArchivePath_;
        request.gameRoot = project.war3GameRoot_;
        request.sessionIdentity = project.mapEdits_->SessionIdentity();
        request.revision = project.mapEdits_->Revision();
        request.terrainSourceRevision = project.mapEdits_->TerrainSourceRevision();
        request.projectGeneration = project.editRevision_.Generation();
        request.projectRevision = project.editRevision_.Revision();
        request.terrain = project.mapEdits_->Terrain();
        request.enforceWaterLimits = enforceWaterLimits;
        request.maximumBytes = maximumBytes;
        if (!request.archivePath.empty()) {
            if (!project.archiveSession_)
                return fail("TERRAIN_WATER_SOURCE_REQUIRED", "当前地图没有匹配的可编辑归档来源。");
            request.sourcePath = project.archiveSession_->SourcePath();
            request.sourceSha256 = project.archiveSession_->SourceSha256();
            request.archive = project.archiveSession_;
        } else {
            if (!project.directorySnapshot_)
                return fail("TERRAIN_WATER_SOURCE_REQUIRED", "当前目录没有匹配的来源快照。");
            const auto& snapshot = project.directorySnapshot_->Snapshot();
            if (snapshot.files.size() > maximumBytes / sizeof(WorkspaceFile))
                return fail("TERRAIN_WATER_CAPTURE_BUDGET", "目录来源清单超过水刷捕获预算。");
            std::size_t remaining = maximumBytes - snapshot.files.size() * sizeof(WorkspaceFile);
            for (const auto& file : snapshot.files) {
                const auto pathBytes = file.relativePath.native().size() * sizeof(std::filesystem::path::value_type);
                if (pathBytes > remaining || file.sha256.size() > remaining - pathBytes)
                    return fail("TERRAIN_WATER_CAPTURE_BUDGET", "目录来源清单超过水刷捕获预算。");
                remaining -= pathBytes + file.sha256.size();
            }
            request.directory = snapshot;
            request.sourcePath = snapshot.root;
            request.sourceSha256 = snapshot.aggregateSha256;
        }
        if (!captureDoodads) return Result<TerrainWaterEditRequest>::Success(std::move(request));
        const auto size = project.mapEdits_->SnapshotByteSize(MapDataKind::Doodads);
        if (!size.Succeeded()) return {std::nullopt, size.diagnostics};
        if (*size.value > maximumBytes)
            return fail("TERRAIN_WATER_CAPTURE_BUDGET", "装饰物快照超过水刷捕获预算。");
        auto document = project.mapEdits_->Snapshot(MapDataKind::Doodads, maximumBytes);
        if (!document.Succeeded()) return {std::nullopt, std::move(document.diagnostics)};
        const auto& member = document.value->Member(MapDataKind::Doodads);
        if (member.state == MapDataState::Ready && document.value->Doodads())
            request.doodads = *document.value->Doodads();
        else if (member.state != MapDataState::Missing || !member.sources.empty())
            return fail("TERRAIN_WATER_DOODADS_UNAVAILABLE", "装饰物来源不完整或有歧义，不能按空装饰物处理水刷。");
        request.doodadsCaptured = true;
        return Result<TerrainWaterEditRequest>::Success(std::move(request));
    } catch (const std::exception&) {
        return fail("TERRAIN_WATER_CAPTURE_ALLOCATION", "水刷来源快照无法完成分配，本次未修改地形。");
    }
}

bool TerrainWaterEditRequestCurrent(const EditorProject& project, const TerrainWaterEditRequest& request,
    bool enforceWaterLimits) {
    return project.mapEdits_ && !project.HasPendingAssetEdits()
        && !project.sourceChangeSuspected_ && !project.sourceChangeConfirmed_
        && request.enforceWaterLimits == enforceWaterLimits
        && project.mapEdits_->SessionIdentity() == request.sessionIdentity
        && project.mapEdits_->Revision() == request.revision
        && project.mapEdits_->TerrainSourceRevision() == request.terrainSourceRevision
        && project.mapEdits_->Terrain() == request.terrain
        && project.editRevision_.Generation() == request.projectGeneration
        && project.editRevision_.Revision() == request.projectRevision && SameSource(project, request);
}

Result<TerrainWaterEditCapture> LoadTerrainWaterEditResources(std::shared_ptr<const TerrainWaterEditRequest> request) {
    using Capture = Result<TerrainWaterEditCapture>;
    if (!request || !request->terrain || !request->doodadsCaptured || !request->maximumBytes || request->maximumBytes > kMaximumBytes)
        return Capture::Failure("Terrain.Water", "TERRAIN_WATER_CAPTURE_INVALID", "水刷来源请求无效。");
    try {
        const auto path = VerifyTerrainWaterEditRequestSourcePath(*request);
        if (!path.Succeeded()) return {std::nullopt, path.diagnostics};
        const auto archive = request->archive;
        Result<std::vector<std::byte>> bytes;
        if (!request->archivePath.empty()) {
            if (!archive || archive->SourcePath() != request->sourcePath || archive->SourceSha256() != request->sourceSha256)
                return Capture::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_CHANGED", "地图归档已变化，请重新打开地图。");
            const std::array<std::string, 1> names{"war3map.w3e"};
            ArchiveExactMemberLimits limits;
            limits.totalBytes = request->maximumBytes;
            limits.memberBytes = request->maximumBytes;
            auto captured = archive->CaptureExactMembers(names, limits);
            if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
            if (captured.value->empty() || !captured.value->front().present)
                return Capture::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_MISSING", "地图归档缺少war3map.w3e。");
            bytes = Result<std::vector<std::byte>>::Success(std::move(captured.value->front().bytes));
        } else if (request->directory) bytes = ReadDirectoryTerrain(*request->directory, request->maximumBytes);
        else return Capture::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_REQUIRED", "水刷缺少真实来源。");
        if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
        auto terrain = MapData::ParseMapTerrain(*bytes.value);
        if (!terrain.Succeeded()) return {std::nullopt, std::move(terrain.diagnostics)};
        auto resources = archive
            ? Services::TerrainPathingSave::CaptureWaterBrushRules(*archive, *terrain.value->Snapshot(),
                *request->terrain, request->doodads, request->gameRoot, request->enforceWaterLimits, request->maximumBytes)
            : Services::TerrainPathingSave::CaptureWaterBrushRules(*request->directory, *terrain.value->Snapshot(),
                *request->terrain, request->doodads, request->gameRoot, request->enforceWaterLimits, request->maximumBytes);
        if (!resources.Succeeded()) return {std::nullopt, std::move(resources.diagnostics)};
        const auto input = resources.value->VerifyWaterBrushInput(*terrain.value->Snapshot(), *request->terrain);
        if (!input.Succeeded()) return {std::nullopt, input.diagnostics};
        const auto verified = archive ? resources.value->VerifySourceInputs(*archive, request->maximumBytes)
                                      : resources.value->VerifySourceInputs(*request->directory, request->maximumBytes);
        if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
        return Capture::Success({std::move(request), std::move(*resources.value)});
    } catch (const std::exception&) {
        return Capture::Failure("Terrain.Water", "TERRAIN_WATER_CAPTURE_FAILED", "水刷资源读取或分配失败，本次未修改地形。");
    }
}

Status VerifyTerrainWaterEditResources(const TerrainWaterEditCapture& capture) {
    if (!capture.request || !capture.resources.WaterBrushRules())
        return Status::Failure("Terrain.Water", "TERRAIN_WATER_CAPTURE_INVALID", "水刷缺少捕获的真实规则。");
    try {
        const auto& request = *capture.request;
        const auto path = VerifyTerrainWaterEditRequestSourcePath(request);
        if (!path.Succeeded()) return path;
        if (request.archive) return capture.resources.VerifySourceInputs(*request.archive, request.maximumBytes);
        if (request.directory) return capture.resources.VerifySourceInputs(*request.directory, request.maximumBytes);
        return Status::Failure("Terrain.Water", "TERRAIN_WATER_SOURCE_REQUIRED", "水刷缺少真实来源。");
    } catch (const std::exception&) {
        return Status::Failure("Terrain.Water", "TERRAIN_WATER_VERIFY_FAILED", "水刷来源复核失败，本笔未提交。");
    }
}
} // namespace War3Map::QtProject
