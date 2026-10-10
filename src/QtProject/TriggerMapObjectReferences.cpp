#include "EditorProject.h"

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include <algorithm>
#include <array>

namespace War3Map::QtProject {

Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>
EditorProject::CaptureTriggerMapObjectReferences() const {
    using Output = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>;
    constexpr std::array kinds{ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item};
    if (!archiveSession_) {
        const bool objectChanges = std::any_of(kinds.begin(), kinds.end(), [this](auto type) {
            return objectDirty_[static_cast<std::size_t>(type)];
        });
        if (objectChanges || (mapEdits_ && mapEdits_->IsDirty())) {
            return Output::Failure("Trigger.MapObjects", "MAP_OBJECT_DIRECTORY_OVERLAY_UNSUPPORTED",
                "当前目录有未保存的地图或物编覆盖，预放引用尚不能冻结这些来源；普通参数仍可编辑。");
        }
        if (projectRoot_.empty()) {
            return Output::Failure("Trigger.MapObjects", "MAP_OBJECT_SOURCE_MISSING",
                "当前触发器没有可捕获的地图来源，无法提供预放单位或物品引用。");
        }
        // 共用目录捕获会明确拒绝尚未投影的 LNI 分类，不能退回原版表猜测。
        return Services::CaptureDirectoryMapObjectReferences(projectRoot_, war3GameRoot_);
    }

    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
    if (mapEdits_) {
        // 只复制实例及其布局上下文，避免为参数面板复制无关的地形、声音等成员。
        const auto current = mapEdits_->Snapshot(MapDataKind::Units, 128U * 1024U * 1024U);
        if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
        for (const auto kind : {MapDataKind::Settings, MapDataKind::Units}) {
            const auto& member = current.value->Member(kind);
            if (member.state == MapDataState::Missing) removals.push_back(member.memberName);
            else {
                // 原字节交由同一捕获服务解析；歧义、未知布局不在适配层放行。
                for (const auto& source : member.sources) replacements.push_back(source);
            }
        }
    }

    std::vector<Diagnostic> diagnostics;
    for (const auto type : kinds) {
        const auto index = static_cast<std::size_t>(type);
        if (!objectDirty_[index]) continue;
        if (!objectDocuments_[index] || !nativeObjectDocuments_[index]) {
            return Output::Failure("Trigger.MapObjects", "MAP_OBJECT_NATIVE_BASELINE_MISSING",
                "预放引用的物编类别已修改，但缺少可验证的 LNI 文档或原生基线。");
        }
        if (objectDocuments_[index]->Objects().empty() &&
            !archiveSession_->HasMember(ObjectData::NativeFileName(type))) continue;
        auto native = *nativeObjectDocuments_[index];
        const auto applied = ObjectData::ApplyLniChangesToNative(native, *objectDocuments_[index],
            objectFieldSchema_.get(), standardCatalog_.get());
        diagnostics.insert(diagnostics.end(), applied.diagnostics.begin(), applied.diagnostics.end());
        if (!applied.Succeeded()) return {{}, std::move(diagnostics)};
        auto encoded = ObjectData::NativeObjectCodec(type).Serialize(native);
        diagnostics.insert(diagnostics.end(), encoded.diagnostics.begin(), encoded.diagnostics.end());
        if (!encoded.Succeeded() || !encoded.value) return {{}, std::move(diagnostics)};
        replacements.push_back({std::string(ObjectData::NativeFileName(type)), std::move(*encoded.value)});
    }
    auto captured = Services::CaptureArchiveMapObjectReferences(*archiveSession_, war3GameRoot_, replacements, removals);
    captured.diagnostics.insert(captured.diagnostics.begin(), diagnostics.begin(), diagnostics.end());
    return captured;
}

} // namespace War3Map::QtProject
