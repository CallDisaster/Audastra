#include "War3MapCore/ObjectData/ObjectBatchEdit.h"

#include <set>

namespace War3Map::ObjectData {

Result<LniObjectDocument> ApplyBatchModification(const LniObjectDocument& document,
    std::span<const ObjectKey> targets, const ModificationKey& field, const Value& value) {
    if (targets.empty())
        return Result<LniObjectDocument>::Failure("ObjectBatchEdit", "BATCH_EMPTY", "没有选择要编辑的物体");
    std::set<std::string> seen;
    auto candidate = document;
    for (const auto& target : targets) {
        if (target.type != document.Type() || !seen.insert(target.rawcode).second)
            return Result<LniObjectDocument>::Failure("ObjectBatchEdit", "BATCH_TARGET_INVALID",
                "批量编辑只接受同一类别且互不重复的物体");
        const auto applied = candidate.SetModification(target, field, value);
        if (!applied.Succeeded()) {
            Result<LniObjectDocument> result;
            result.diagnostics = applied.diagnostics;
            return result;
        }
    }
    const auto encoded = LniObjectCodec(document.Type()).Serialize(candidate);
    if (!encoded.Succeeded()) {
        Result<LniObjectDocument> result;
        result.diagnostics = encoded.diagnostics;
        return result;
    }
    return Result<LniObjectDocument>::Success(std::move(candidate));
}

} // namespace War3Map::ObjectData
