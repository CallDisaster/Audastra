#include "ProjectQuerySources.h"

#include "EditorProject.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

#include <QDir>

namespace War3Map::QtProject {
namespace {
bool ObjectSourceMissing(const EditorProject& project, ObjectData::ObjectType type) {
    const std::string native(ObjectData::NativeFileName(type)), lni(ObjectData::LniFileName(type));
    const std::array<std::string, 4> candidates{native, "map/" + native, lni, "table/" + lni};
    if (project.archiveSession_) {
        // 匿名块不能证明没有物编；存在但未解析的成员也不能当成空覆盖。
        if (project.archiveSession_->HasOpaqueMembers()) return false;
        for (const auto& name : candidates) if (project.archiveSession_->HasMember(name)) return false;
    } else if (!project.sourceArchivePath_.empty() || !project.directorySnapshot_) return false;
    if (!project.directorySnapshot_) return project.archiveSession_ != nullptr;
    std::error_code error;
    if (project.projectRoot_.empty() || !std::filesystem::equivalent(project.projectRoot_,
        project.directorySnapshot_->Snapshot().root, error) || error) return false;
    // 归档提取的 LNI 投影也可能解析失败；同时核对当前工作区快照，不能仅看原图。
    for (const auto& file : project.directorySnapshot_->Snapshot().files) {
        const auto relative = QDir::fromNativeSeparators(QString::fromStdWString(file.relativePath.wstring()));
        for (const auto& name : candidates) {
            if (relative.compare(QString::fromStdString(name), Qt::CaseInsensitive) == 0) return false;
        }
    }
    return true;
}
}

std::array<Services::ProjectSearchObjectSource, 7> CaptureProjectObjectSources(const EditorProject& project) {
    std::array<Services::ProjectSearchObjectSource, 7> sources;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto& source = sources[index];
        source.type = static_cast<ObjectData::ObjectType>(index);
        if (const auto* document = project.objectDocuments_[index].get()) {
            source.records = document->Objects();
            source.state = document->IsStructurallySafe() ? Services::ProjectSearchState::Ready : Services::ProjectSearchState::Partial;
        } else if (const auto* native = project.nativeObjectDocuments_[index].get()) {
            source.records = native->Objects();
            source.origin = Services::ProjectSearchObjectOrigin::Native;
            source.state = native->IsStructurallySafe() ? Services::ProjectSearchState::Ready : Services::ProjectSearchState::Partial;
        } else if (ObjectSourceMissing(project, source.type)) {
            source.state = Services::ProjectSearchState::Missing;
        }
    }
    return sources;
}

Result<ProjectMapQuerySnapshot> CaptureProjectMapQuery(const EditorProject& project, std::string documentId,
    std::uint64_t generation) {
    auto captured = CaptureProjectMapDataQuery(project, std::move(documentId), generation);
    if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
    return Result<ProjectMapQuerySnapshot>::Success({*captured.value->OwnedDocument(), std::move(*captured.value)});
}

Result<Services::MapDataQuerySnapshot> CaptureProjectMapDataQuery(const EditorProject& project,
    std::string documentId, std::uint64_t generation, std::shared_ptr<const MapDocument> owned) {
    if (!project.mapDocument_) return Result<Services::MapDataQuerySnapshot>::Failure("Project.Query", "QUERY_NO_MAP", "没有已加载地图文档。");
    // owned只能来自调用方已核对身份的同一次同步捕获；查询服务持有它，不能借用栈上文档。
    if (!owned) owned = std::make_shared<const MapDocument>(
        project.mapEdits_ ? project.mapEdits_->Snapshot() : *project.mapDocument_);
    std::string sourceSha;
    if (project.archiveSession_) sourceSha = project.archiveSession_->SourceSha256();
    else if (project.readOnlyArchive_) sourceSha = project.readOnlyArchive_->SourceSha256();
    else {
        std::string digest;
        for (const auto& member : project.mapDocument_->Members()) {
            digest += member.memberName + ":" + std::to_string(member.sources.size()) + ":";
            for (const auto& input : member.sources) {
                auto hashed = Sha256(input.data);
                if (!hashed.Succeeded()) return {std::nullopt, std::move(hashed.diagnostics)};
                digest += *hashed.value;
            }
        }
        auto hashed = Sha256Text(digest);
        if (!hashed.Succeeded()) return {std::nullopt, std::move(hashed.diagnostics)};
        sourceSha = std::move(*hashed.value);
    }
    Services::MapDataQueryBinding binding{std::move(documentId), generation,
        project.mapEdits_ ? project.mapEdits_->Revision() : 0, std::move(sourceSha)};
    return Services::CaptureMapDataSnapshot(std::move(owned), std::move(binding),
        project.archiveSession_ && project.archiveSession_->HasOpaqueMembers());
}
}
