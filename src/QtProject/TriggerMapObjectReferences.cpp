#include "EditorProject.h"
#include "ProjectObjectCatalog.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include <algorithm>
#include <array>

namespace War3Map::QtProject {

// 捕获只读这些输入：文档、撤销栈与编辑修订（ReferenceStamp）、来源归档/目录/战役、游戏目录、
// 原版目录、物编修改标记和导入资源。全部相同时复用上次成功的快照。
struct EditorProject::TriggerMapObjectCache {
    struct Key {
        std::string references, archiveSha;
        const void* archive = nullptr;
        const void* readOnly = nullptr;
        const void* directory = nullptr;
        const void* campaign = nullptr;
        const void* standard = nullptr;
        Services::CampaignRevision campaignRevision;
        std::filesystem::path root, gameRoot;
        std::array<bool, kObjectTypeCount> dirty{};
        AssetState assets;
        bool operator==(const Key&) const = default;
    };
    Key key;
    Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> captured;
};

Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>
EditorProject::CaptureTriggerMapObjectReferences() const {
    TriggerMapObjectCache::Key key{ReferenceStamp(), archiveSession_ ? archiveSession_->SourceSha256() : std::string{},
        archiveSession_.get(), readOnlyArchive_.get(), directorySnapshot_.get(), campaignScope_.get(), standardCatalog_.get(),
        campaignScope_ ? campaignScope_->Revision() : Services::CampaignRevision{}, projectRoot_, war3GameRoot_,
        objectDirty_, CaptureAssets()};
    // 来源待核验时不复用，交给捕获路径报告。
    const bool reusable = !sourceChangeSuspected_ && !sourceChangeConfirmed_;
    if (reusable && triggerMapObjectCache_ && triggerMapObjectCache_->key == key) return triggerMapObjectCache_->captured;
    auto captured = CaptureTriggerMapObjectReferencesUncached();
    if (reusable && captured.Succeeded()) {
        triggerMapObjectCache_ = std::make_shared<const TriggerMapObjectCache>(TriggerMapObjectCache{std::move(key), captured});
    } else triggerMapObjectCache_.reset();
    return captured;
}

Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>
EditorProject::CaptureTriggerMapObjectReferencesUncached() const {
    using Output = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>;
    constexpr std::array kinds{ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item};
    const std::array<std::string, 2> classificationNames{"Units/UnitData.slk", "Units/ItemData.slk"};
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
        auto frozen = CaptureProjectObjectCatalogSources(*this, classificationNames);
        if (!frozen.Succeeded() || !frozen.value) return {{}, frozen.diagnostics};
        const auto classification = frozen.value->Sources();
        return Services::CaptureDirectoryMapObjectReferences(projectRoot_, war3GameRoot_, &classification);
    }

    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
    if (mapEdits_) {
        // 只复制授予预放引用资格的成员（实例、可破坏物、区域、镜头、声音及布局上下文），不复制地形等。
        const auto take = [&](const MapDocument& current, MapDataKind kind) {
            const auto& member = current.Member(kind);
            if (member.state == MapDataState::Missing) removals.push_back(member.memberName);
            else {
                // 原字节交由同一捕获服务解析；歧义、未知布局不在适配层放行。
                for (const auto& source : member.sources) replacements.push_back(source);
            }
        };
        const auto units = mapEdits_->Snapshot(MapDataKind::Units, 128U * 1024U * 1024U);
        if (!units.Succeeded() || !units.value) return {{}, units.diagnostics};
        take(*units.value, MapDataKind::Settings);
        take(*units.value, MapDataKind::Units);
        for (const auto kind : {MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras, MapDataKind::Sounds}) {
            const auto current = mapEdits_->Snapshot(kind, 128U * 1024U * 1024U);
            if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
            take(*current.value, kind);
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
    auto frozen = CaptureProjectObjectCatalogSources(*this, classificationNames);
    if (!frozen.Succeeded() || !frozen.value) return {{}, frozen.diagnostics};
    const auto classification = frozen.value->Sources();
    auto captured = Services::CaptureArchiveMapObjectReferences(*archiveSession_, war3GameRoot_, replacements, removals, &classification);
    captured.diagnostics.insert(captured.diagnostics.begin(), diagnostics.begin(), diagnostics.end());
    return captured;
}

} // namespace War3Map::QtProject
