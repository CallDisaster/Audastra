#pragma once

#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <limits>

namespace War3Map::ObjectData {

class LniObjectDocument;
class ObjectFieldSchema;
class StandardObjectCatalog;

struct NativeObjectFreezeFailure {
    std::string code;
    std::string message;
};

struct NativeObjectFreezeOptions {
    std::size_t maximumBytes = std::numeric_limits<std::size_t>::max();
    std::optional<ObjectType> outputType;
    NativeObjectFreezeFailure projectionFailure;
    NativeObjectFreezeFailure serializationFailure;
    NativeObjectFreezeFailure budgetFailure;
};

enum class NativeObjectFreezeStage { Projection, Serialization, Budget, Complete };

struct FrozenNativeObject {
    NativeObjectDocument document;
    std::vector<std::byte> bytes;
};

struct NativeObjectFreezeResult {
    std::optional<FrozenNativeObject> value;
    NativeObjectFreezeStage stage = NativeObjectFreezeStage::Projection;
    NativeObjectFreezeFailure failure;
    // 分阶段保留诊断，使调用方沿用既有的警告与失败展示规则。
    std::vector<Diagnostic> projectionDiagnostics;
    std::vector<Diagnostic> serializationDiagnostics;
};

// 仅回灌传入的私有副本；编码和预算均通过后才返回可发布的快照。
[[nodiscard]] NativeObjectFreezeResult FreezeNativeObjectChanges(
    NativeObjectDocument native,
    const LniObjectDocument& editable,
    const ObjectFieldSchema* schema = nullptr,
    const StandardObjectCatalog* standard = nullptr,
    const NativeObjectFreezeOptions& options = {});

} // namespace War3Map::ObjectData
