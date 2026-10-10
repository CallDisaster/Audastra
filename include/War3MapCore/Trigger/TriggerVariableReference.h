#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

namespace War3Map::Trigger {

enum class VariableReferenceKind { Declared, GeneratedTrigger, PreplacedObject };
struct VariableReference {
    VariableReferenceKind kind = VariableReferenceKind::Declared;
    std::string type;
    bool isArray = false;
    bool writable = false;
    std::string dependencySha256;
};

// 同一解析用于图插入、旧闭包guard和参数类型。没有真实来源时不猜gg_前缀。
[[nodiscard]] Result<VariableReference> ResolveVariableReference(
    const TriggerDocument& document, std::string_view name,
    const MapObjectReferenceSnapshot* mapObjects = nullptr);

} // namespace War3Map::Trigger
