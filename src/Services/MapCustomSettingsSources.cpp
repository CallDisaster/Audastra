#include "War3MapCore/Services/MapCustomSettingsSources.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <map>

namespace War3Map::Services {
namespace {
using namespace ObjectData;
struct Sources {
    ObjectCatalogSourceSnapshot snapshot;
    StandardObjectCatalog standard;
    ObjectFieldSchema schema;
    MapCustomSettingsObjectOverlay documents;
    Result<ObjectCommandContext> Context(ObjectType type) {
        const auto index = static_cast<std::size_t>(type);
        if (index >= documents.size()) return Result<ObjectCommandContext>::Failure("Map.CustomSettings", "SETTINGS_OBJECT_KIND_INVALID", "对象类别无效。");
        if (!documents[index]) {
            const auto input = snapshot.Read(NativeFileName(type));
            if (!input.Succeeded()) return {{}, input.diagnostics};
            std::vector<std::byte> bytes;
            if (input.value->has_value()) {
                auto converted = ConvertNativeObjectDataToLni(type, input.value->value().bytes);
                if (!converted.Succeeded() || !converted.diagnostics.empty()) return {{}, converted.diagnostics};
                bytes = std::move(*converted.value);
            }
            const auto parsed = LniObjectCodec(type).Parse(bytes);
            const auto* document = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
            if (!document || !document->IsStructurallySafe() || !parsed.diagnostics.empty()) {
                if (!parsed.diagnostics.empty()) return {{}, parsed.diagnostics};
                return Result<ObjectCommandContext>::Failure("Map.CustomSettings", "SETTINGS_OBJECT_DOCUMENT_UNSAFE", "原生物编不能无损建立验证文档。");
            }
            documents[index] = std::make_shared<const LniObjectDocument>(*document);
        }
        ObjectCommandContext context; context.type = type; context.standard = &standard; context.schema = &schema;
        for (std::size_t i = 0; i < documents.size(); ++i) context.documents[i] = documents[i].get();
        context.document = documents[index].get();
        return Result<ObjectCommandContext>::Success(context);
    }
};
} // namespace

Result<MapCustomSettingsValidation> CaptureMapCustomSettingsValidation(
    const ObjectCatalogSources& input, MapCustomSettingsObjectOverlay overlay) {
    using R = Result<MapCustomSettingsValidation>;
    std::size_t overlayBytes = 0;
    for (std::size_t i = 0; i < overlay.size(); ++i) if (overlay[i]) {
        if (overlay[i]->Type() != static_cast<ObjectType>(i) || !overlay[i]->IsStructurallySafe())
            return R::Failure("Map.CustomSettings", "SETTINGS_OBJECT_OVERLAY_UNSAFE", "工程物编覆盖类别不匹配或未完整解析。");
        auto encoded = LniObjectCodec(static_cast<ObjectType>(i)).Serialize(*overlay[i]);
        if (!encoded.Succeeded()) return {{}, encoded.diagnostics};
        if (encoded.value->size() > 32U * 1024U * 1024U || encoded.value->size() > 64U * 1024U * 1024U - overlayBytes)
            return R::Failure("Map.CustomSettings", "SETTINGS_OBJECT_OVERLAY_BUDGET", "工程物编覆盖超过 32 MiB/类或 64 MiB 总预算。");
        overlayBytes += encoded.value->size();
    }
    const auto inputNames = MapCustomSettingsInputNames();
    if (!inputNames.Succeeded()) return {{}, inputNames.diagnostics};
    auto names = *inputNames.value;
    auto metadata = ObjectFieldSchema::DefaultSources();
    std::vector<std::string> gameOnly{"UI/WorldEditStrings.txt", "UI/WorldEditGameStrings.txt", "UI/WorldEditData.txt", "UI/UnitEditorData.txt"};
    for (const auto& source : metadata) gameOnly.push_back(source.metadataFile);
    auto configured = input; configured.gameOnlyNames = gameOnly;
    auto captured = CaptureObjectCatalogSources(configured, names);
    if (!captured.Succeeded()) return {{}, captured.diagnostics};
    auto owner = std::make_shared<Sources>(); owner->snapshot = std::move(*captured.value); owner->documents = std::move(overlay);
    const auto reader = [owner](const std::string& name) -> Result<std::vector<std::byte>> {
        auto source = owner->snapshot.Read(name);
        if (!source.Succeeded()) return {{}, source.diagnostics};
        if (!source.value->has_value()) return Result<std::vector<std::byte>>::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_MISSING", "固定物编来源缺席：" + name);
        return Result<std::vector<std::byte>>::Success(source.value->value().bytes);
    };
    CatalogLoadOptions options; options.rejectInvalidPresentSources = true;
    auto catalog = StandardObjectCatalog::Load(reader, options);
    if (!catalog.Succeeded()) return {{}, catalog.diagnostics};
    auto schema = ObjectFieldSchema::Load(reader);
    if (!schema.Succeeded()) return {{}, schema.diagnostics};
    owner->standard = std::move(*catalog.value); owner->schema = std::move(*schema.value);
    MapCustomSettingsValidation result;
    result.objectContext = [owner](ObjectType type) { return owner->Context(type); };
    result.verify = [owner] { return owner->snapshot.Verify(); };
    return R::Success(std::move(result));
}
Result<std::vector<std::string>> MapCustomSettingsInputNames() {
    auto names = ObjectData::EffectiveObjectCatalog::InputNames(); if (!names.Succeeded()) return names;
    for (const auto& metadata : ObjectData::ObjectFieldSchema::DefaultSources()) names.value->push_back(metadata.metadataFile);
    for (const auto& name : {"UI/WorldEditStrings.txt", "UI/WorldEditGameStrings.txt", "UI/WorldEditData.txt", "UI/UnitEditorData.txt"}) names.value->emplace_back(name);
    for (unsigned i = 0; i < 7; ++i) names.value->emplace_back(ObjectData::NativeFileName(static_cast<ObjectData::ObjectType>(i)));
    std::map<std::string, std::string> unique;
    for (const auto& name : *names.value) {
        auto key = Resources::CanonicalImportPath(name); if (!key.Succeeded()) return {{}, key.diagnostics};
        unique.try_emplace(*key.value, name);
    }
    names.value->clear(); for (auto& [key, name] : unique) names.value->push_back(std::move(name));
    return names;
}
} // namespace War3Map::Services
