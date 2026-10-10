#include "EditorProject.h"

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
        objects.push_back({type, "table/" + std::string(ObjectData::LniFileName(type)),
            document ? ObjectData::ReferenceInputState::Ready : ObjectData::ReferenceInputState::Missing,
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
