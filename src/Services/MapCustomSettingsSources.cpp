#include "War3MapCore/Services/MapCustomSettingsSources.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <memory>

namespace War3Map::Services {

Result<MapCustomSettingsValidation> CaptureMapCustomSettingsValidation(
    const ObjectCatalogSources& input, MapCustomSettingsObjectOverlay overlay, const AuxiliaryReferenceContext& references) {
    auto sources = CaptureObjectCommandSources(input, std::move(overlay));
    if (!sources.Succeeded() || !sources.value) return {{}, std::move(sources.diagnostics)};
    MapCustomSettingsValidation result;
    result.objectContext = std::move(sources.value->context);
    result.verify = std::move(sources.value->verify);
    const auto triggerOwner = references.triggers ? std::make_shared<const Trigger::TriggerDocument>(*references.triggers) : nullptr;
    result.references = [triggerOwner, complete = references.completeTriggerSource, opaque = references.opaqueScriptSources] {
        return AuxiliaryReferenceContext{triggerOwner.get(), complete, opaque};
    };
    return Result<MapCustomSettingsValidation>::Success(std::move(result));
}

Result<std::vector<std::string>> MapCustomSettingsInputNames() { return ObjectCommandSourceNames(); }

} // namespace War3Map::Services
