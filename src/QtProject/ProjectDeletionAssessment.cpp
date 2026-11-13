#include "EditorProject.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"

namespace War3Map::QtProject {

Services::ObjectDeletionAssessment EditorProject::AssessDeletion(
    const ObjectData::ObjectKey& subject) const {
    std::vector<ObjectData::ObjectRecordSource> objects;
    for (std::size_t i = 0; i < objectDocuments_.size(); ++i) {
        const auto type = static_cast<ObjectData::ObjectType>(i);
        const auto* document = objectDocuments_[i].get();
        // 没有载入的类型：原图确实没有这个成员才是“缺失”（零个对象）；有成员却没载入是读不了，引用看不全。
        const bool present = archiveSession_ && archiveSession_->HasMember(ObjectData::NativeFileName(type));
        objects.push_back({type, "table/" + std::string(ObjectData::LniFileName(type)),
            document ? ObjectData::ReferenceInputState::Ready
                     : present ? ObjectData::ReferenceInputState::Unreadable : ObjectData::ReferenceInputState::Missing,
            document ? document->Objects() : std::span<const ObjectData::ObjectRecord>{}});
    }
    std::optional<MapDocument> edited;
    if (mapEdits_) edited = mapEdits_->Snapshot();
    Services::ProjectReferenceInputs inputs;
    inputs.triggers = triggerDocument_.get();
    inputs.catalog = uiCatalog_.get();
    inputs.objects = objects;
    inputs.map = edited ? &*edited : mapDocument_.get();
    inputs.schema = objectFieldSchema_.get();
    return Services::AssessObjectDeletion(inputs, subject);
}

} // namespace War3Map::QtProject
