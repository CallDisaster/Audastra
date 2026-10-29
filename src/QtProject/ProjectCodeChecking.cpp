#include "EditorProject.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include <QSettings>
#include <QTimer>
#include <chrono>
#include <utility>

namespace War3Map::QtProject {
namespace {
std::string PathKey(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct CodeCheckRoots {
    std::filesystem::path tool, ui, luaRuntime, luaRuntimeYdwe;
};
CodeCheckRoots CaptureRoots(const EditorProject& project) {
    // 与F9读取相同的持久化键；作者未保存的设置草稿不会成为本轮来源。
    const QSettings settings;
    return {project.UiToolRoot(), project.ResolveUiDataRoot(),
        std::filesystem::path(settings.value(QStringLiteral("mapTest/runtimeRoot")).toString().trimmed().toStdWString()),
        std::filesystem::path(settings.value(QStringLiteral("ui/dataRoot")).toString().trimmed().toStdWString())};
}
std::string BuildSourceStamp(const EditorProject& project, std::string reference, const CodeCheckRoots& roots) {
    std::string stamp = std::move(reference) + '|' + std::to_string(project.editRevision_.Generation()) + ':'
        + std::to_string(project.editRevision_.Revision()) + '|' + PathKey(project.sourceArchivePath_) + '|'
        + (project.archiveSession_ ? project.archiveSession_->SourceSha256() : "") + '|' + PathKey(roots.tool) + '|'
        + PathKey(roots.ui) + '|' + PathKey(project.war3GameRoot_) + '|'
        + std::to_string(project.sourceChangeSuspected_) + std::to_string(project.sourceChangeConfirmed_)
        + "|F9-runtime:" + PathKey(roots.luaRuntime) + "|F9-YDWE:" + PathKey(roots.luaRuntimeYdwe);
    for (const auto& declaration : project.scriptSourceEncodings_)
        stamp += '|' + PathKey(declaration.path) + ':' + std::to_string(static_cast<int>(declaration.encoding));
    for (const auto& access : project.scriptSourceIncludeAccess_)
        stamp += '|' + PathKey(access.path) + ':' + std::to_string(static_cast<int>(access.kind)) + ':'
            + std::to_string(static_cast<int>(access.encoding)) + ':' + std::to_string(access.required);
    return stamp;
}
}

std::string EditorProject::CodeCheckSourceStamp() const {
    return BuildSourceStamp(*this, ReferenceStamp(), CaptureRoots(*this));
}

std::shared_ptr<Services::CodeCheckProjectCache> EditorProject::SharedCodeCheckCache() {
    if (!codeCheckCache_) codeCheckCache_ = std::make_shared<Services::CodeCheckProjectCache>();
    ScheduleCodeCheckCacheRelease();
    return codeCheckCache_;
}

void EditorProject::ScheduleCodeCheckCacheRelease() {
    const auto touch = ++codeCheckCacheTouch_;
    QTimer::singleShot(std::chrono::minutes(3), this, [this, touch] {
        if (touch == codeCheckCacheTouch_ && codeCheckCache_ && !codeCheckCache_->TryRelease()) ScheduleCodeCheckCacheRelease();
    });
}

bool EditorProject::IsCodeCheckBindingCurrent(std::string_view binding) const {
    return !binding.empty() && codeCheckSnapshot_ && codeCheckSnapshot_->binding == binding
        && codeCheckStamp_ == CodeCheckSourceStamp();
}

void EditorProject::AdoptCodeCheckIndex(std::shared_ptr<const Services::CodeCheckResult> index) {
    if (index && index->completed && !index->cancelled && IsCodeCheckBindingCurrent(index->contextBinding))
        codeCheckIndex_ = std::move(index);
}

std::shared_ptr<const Services::CodeCheckResult> EditorProject::CurrentCodeCheckIndex() const {
    return codeCheckIndex_ && IsCodeCheckBindingCurrent(codeCheckIndex_->contextBinding) ? codeCheckIndex_ : nullptr;
}

std::shared_ptr<const Services::CodeCheckProjectSnapshot> EditorProject::CaptureCodeCheckSnapshot() const {
    const auto roots = CaptureRoots(*this);
    auto stamp = BuildSourceStamp(*this, ReferenceStamp(), roots);
    if (codeCheckSnapshot_ && codeCheckStamp_ == stamp) return codeCheckSnapshot_;
    auto snapshot = std::make_shared<Services::CodeCheckProjectSnapshot>();
    auto identity = Sha256Text(stamp);
    snapshot->binding = identity.Succeeded() && identity.value ? *identity.value : std::string{};
    snapshot->revision = editRevision_.Revision(); snapshot->sourcePath = sourceArchivePath_;
    // 来源监视已发现变化时不能把旧SHA再作为完整上下文资格。
    snapshot->sourceSha256 = archiveSession_ && !sourceChangeConfirmed_ && !sourceChangeSuspected_
        ? archiveSession_->SourceSha256() : std::string{};
    snapshot->toolRoot = roots.tool; snapshot->uiRoot = roots.ui; snapshot->gameRoot = war3GameRoot_;
    snapshot->luaRuntimeRoot = roots.luaRuntime; snapshot->luaRuntimeYdweRoot = roots.luaRuntimeYdwe;
    if (triggerDocument_) snapshot->triggers = std::make_shared<const Trigger::TriggerDocument>(*triggerDocument_);
    if (uiCatalog_) snapshot->catalog = std::make_shared<const Trigger::UiCatalog>(*uiCatalog_);
    if (mapEdits_) {
        snapshot->map = mapEdits_->Snapshot();
        if (const auto terrain = mapEdits_->Terrain()) snapshot->terrain = Script::TerrainExtent{
            static_cast<std::int32_t>(terrain->header.width), static_cast<std::int32_t>(terrain->header.height),
            terrain->header.offset[0], terrain->header.offset[1]};
    } else if (mapDocument_) snapshot->map = *mapDocument_;
    for (std::size_t index = 0; index < 3; ++index) {
        if (nativeObjectDocuments_[index]) snapshot->nativeObjects[index] =
            std::make_shared<const ObjectData::NativeObjectDocument>(*nativeObjectDocuments_[index]);
        if (objectDocuments_[index]) snapshot->editableObjects[index] =
            std::make_shared<const ObjectData::LniObjectDocument>(*objectDocuments_[index]);
    }
    if (objectFieldSchema_) snapshot->objectSchema = std::make_shared<const ObjectData::ObjectFieldSchema>(*objectFieldSchema_);
    snapshot->sourceEncodings = scriptSourceEncodings_; snapshot->includeAccess = scriptSourceIncludeAccess_;
    codeCheckStamp_ = std::move(stamp); codeCheckSnapshot_ = std::move(snapshot);
    return codeCheckSnapshot_;
}
} // namespace War3Map::QtProject
