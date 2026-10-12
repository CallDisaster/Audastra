#include "ObjectContextProvider.h"
#include "EditorProject.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Workspace.h"

#include <QPointer>
#include <array>
#include <memory>
#include <optional>

namespace War3Map::QtProject {
namespace {
using namespace ObjectData;
using ContextResult = Result<Services::ObjectCommandContext>;
class ObjectContextState {
public:
    explicit ObjectContextState(EditorProject& project) : project_(&project),
        generation_(project.editRevision_.Generation()), revision_(project.editRevision_.Revision()) {}

    ContextResult Context(ObjectType type) {
        if (!project_) return ContextResult::Failure("Object.Live", "OBJECT_MAP_UNAVAILABLE", "地图已关闭。");
        auto& project = *project_;
        if (generation_ != project.editRevision_.Generation() || revision_ != project.editRevision_.Revision())
            return ContextResult::Failure("Object.Live", "OBJECT_CONTEXT_STALE", "物编上下文属于旧工程修订，请重新查询。");
        if (!project.standardCatalog_ || !project.objectFieldSchema_)
            return ContextResult::Failure("Object.Live", "OBJECT_METADATA_UNAVAILABLE",
                "当前地图没有完整的标准目录/字段元数据；先在设置中选择真实游戏来源。");
        const auto index = static_cast<std::size_t>(type);
        if (index >= project.objectDocuments_.size())
            return ContextResult::Failure("Object.Live", "OBJECT_KIND_INVALID", "物编类别无效。");
        if (!project.objectDocuments_[index] && !empty_[index]) {
            if (project.nativeObjectDocuments_[index])
                return ContextResult::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                    "原生物编存在但未完整转换，不能用空文档替代。");
            if (!project.sourceArchivePath_.empty()) {
                const auto archive = Mpq::MpqArchive::OpenReadOnly(project.sourceArchivePath_);
                if (!archive.Succeeded()) return {{}, archive.diagnostics};
                if (archive.value->HasFile(std::string(NativeFileName(type))))
                    return ContextResult::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                        "地图中已有此类物编文件，但没有可编辑文档；原文件保持不变。");
                const auto inventory = archive.value->ListFilesStrict();
                if (!inventory.Succeeded()) return {{}, inventory.diagnostics};
            } else if (project.directorySnapshot_) {
                std::error_code error;
                for (const auto& path : {project.projectRoot_ / "table" / LniFileName(type),
                    project.projectRoot_ / LniFileName(type), project.projectRoot_ / "map" / NativeFileName(type),
                    project.projectRoot_ / NativeFileName(type)}) {
                    const bool exists = std::filesystem::exists(path, error);
                    if (error || exists)
                        return ContextResult::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                            "此类物编来源存在或无法确认，不能创建猜测的空文档。");
                }
            } else return ContextResult::Failure("Object.Live", "OBJECT_MAP_UNAVAILABLE", "先打开地图。");
            const auto parsed = LniObjectCodec(type).Parse({});
            const auto* empty = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
            if (!empty) return ContextResult::Failure("Object.Live", "OBJECT_EMPTY_DOCUMENT_FAILED", "无法建立空物编快照。");
            empty_[index] = *empty;
        }
        Services::ObjectCommandContext context;
        context.type = type; context.standard = project.standardCatalog_.get(); context.schema = project.objectFieldSchema_.get();
        context.document = project.objectDocuments_[index] ? project.objectDocuments_[index].get() : &*empty_[index];
        for (std::size_t i = 0; i < context.documents.size(); ++i) context.documents[i] = project.objectDocuments_[i].get();
        context.documents[index] = context.document;
        if (!nameMap_ && project.MapEdits()) {
            // 名称仅需 WTS；新建单个摆放不复制整份单位、地物与其它地图成员。
            auto names = project.MapEdits()->Snapshot(MapDataKind::Strings);
            if (!names.Succeeded()) return {{}, names.diagnostics};
            nameMap_ = std::make_shared<MapDocument>(std::move(*names.value));
        }
        context.nameMap = nameMap_ ? nameMap_.get() : project.mapDocument_.get();
        return ContextResult::Success(context);
    }
private:
    QPointer<EditorProject> project_;
    std::uint64_t generation_, revision_;
    std::array<std::optional<LniObjectDocument>, 7> empty_;
    std::shared_ptr<MapDocument> nameMap_;
};
} // namespace

ObjectContextProvider MakeObjectContextProvider(EditorProject& project) {
    auto state = std::make_shared<ObjectContextState>(project);
    return [state = std::move(state)](ObjectData::ObjectType type) { return state->Context(type); };
}

} // namespace War3Map::QtProject
