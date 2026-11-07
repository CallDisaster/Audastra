#pragma once

#include "War3MapCore/HLua/Hslk.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

namespace War3Map::HLua {
struct IndexedFieldValue {
    std::string field, nativeId, effectiveValue, valueSource;
};
struct IndexedFieldAudit {
    std::string objectClass, id, source, field, declaredValue;
    std::vector<IndexedFieldValue> indices;
};
// 只产生领域文档；原生编码、来源复核和回读仍由 ObjectTransactionSnapshot 完成。
[[nodiscard]] Result<ObjectData::LniObjectDocument> ApplyHslkObjects(
    ObjectData::ObjectType type, const ObjectData::LniObjectDocument& baseline,
    std::span<const HslkObject> objects, const ObjectData::ObjectFieldSchema& schema,
    const ObjectData::StandardObjectCatalog& standard,
    std::vector<IndexedFieldAudit>* redundantFields = nullptr);

[[nodiscard]] Result<std::vector<HslkObject>> ParseF6ObjectIni(ObjectData::ObjectType type,
    std::string_view text, std::string_view source, const HslkLimits& limits = {});
} // namespace War3Map::HLua
