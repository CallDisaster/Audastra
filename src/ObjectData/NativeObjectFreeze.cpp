#include "War3MapCore/ObjectData/NativeObjectFreeze.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

namespace War3Map::ObjectData {

NativeObjectFreezeResult FreezeNativeObjectChanges(
    NativeObjectDocument native,
    const LniObjectDocument& editable,
    const ObjectFieldSchema* schema,
    const StandardObjectCatalog* standard,
    const NativeObjectFreezeOptions& options) {
    NativeObjectFreezeResult result;
    auto applied = ApplyLniChangesToNative(native, editable, schema, standard);
    const bool projected = applied.Succeeded();
    result.projectionDiagnostics = std::move(applied.diagnostics);
    if (!projected) {
        result.failure = options.projectionFailure;
        return result;
    }
    result.stage = NativeObjectFreezeStage::Serialization;
    auto encoded = NativeObjectCodec(options.outputType.value_or(native.Type())).Serialize(native);
    const bool serialized = encoded.Succeeded() && encoded.value.has_value();
    result.serializationDiagnostics = std::move(encoded.diagnostics);
    if (!serialized) {
        result.failure = options.serializationFailure;
        return result;
    }
    result.stage = NativeObjectFreezeStage::Budget;
    if (encoded.value->size() > options.maximumBytes) {
        result.failure = options.budgetFailure;
        return result;
    }
    result.stage = NativeObjectFreezeStage::Complete;
    result.value = FrozenNativeObject{std::move(native), std::move(*encoded.value)};
    return result;
}

} // namespace War3Map::ObjectData
