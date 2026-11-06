#include "War3MapCore/ObjectData/NativeObjectFreeze.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <iostream>

using namespace War3Map;
using namespace War3Map::ObjectData;

int main() {
    int failures = 0;
    const auto check = [&](bool pass, const char* message) {
        if (!pass) {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    auto baseline = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(std::span<const std::byte>{});
    auto* editable = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    if (!baseline.value || !editable) return 1;
    const NativeObjectCodec codec(ObjectType::Unit);
    const auto original = codec.Serialize(*baseline.value);
    check(editable->AddObject("h001", "hfoo").Succeeded(), "create synthetic custom object");
    check(editable->SetModification({ObjectType::Unit, "h001"}, {"unam"}, std::string("Frozen object")).Succeeded(),
        "edit synthetic object name");
    const auto fingerprint = editable->DocumentFingerprint();
    auto legacy = *baseline.value;
    check(ApplyLniChangesToNative(legacy, *editable).Succeeded(), "legacy projection accepts fixture");
    const auto expected = codec.Serialize(legacy);
    auto frozen = FreezeNativeObjectChanges(*baseline.value, *editable);
    check(frozen.value && expected.value && frozen.value->bytes == *expected.value,
        "shared freeze is byte-identical to original projection and serialization");
    check(frozen.value && codec.Serialize(frozen.value->document).value == expected.value,
        "returned baseline matches returned bytes");
    check(codec.Serialize(*baseline.value).value == original.value && editable->DocumentFingerprint() == fingerprint,
        "successful freeze leaves both original documents unchanged");
    if (!expected.value) return 1;
    NativeObjectFreezeOptions options;
    options.maximumBytes = expected.value->size() - 1;
    options.budgetFailure = {"CALLER_BUDGET", "caller budget"};
    auto rejected = FreezeNativeObjectChanges(*baseline.value, *editable, nullptr, nullptr, options);
    check(!rejected.value && rejected.stage == NativeObjectFreezeStage::Budget
        && rejected.failure.code == "CALLER_BUDGET", "over-budget output is not published and preserves caller diagnostic");
    options.maximumBytes = expected.value->size();
    check(FreezeNativeObjectChanges(*baseline.value, *editable, nullptr, nullptr, options).value.has_value(),
        "exact byte budget is accepted");
    auto wrong = LniObjectCodec(ObjectType::Item).Parse(std::span<const std::byte>{});
    auto* wrongType = dynamic_cast<LniObjectDocument*>(wrong.document.get());
    if (!wrongType) return 1;
    options.projectionFailure = {"CALLER_PROJECT", "caller projection"};
    rejected = FreezeNativeObjectChanges(*baseline.value, *wrongType, nullptr, nullptr, options);
    check(!rejected.value && rejected.stage == NativeObjectFreezeStage::Projection
        && rejected.failure.code == "CALLER_PROJECT" && !rejected.projectionDiagnostics.empty(),
        "projection failure retains cause and caller diagnostic without encoded output");
    check(codec.Serialize(*baseline.value).value == original.value && editable->DocumentFingerprint() == fingerprint,
        "all failures leave source documents unchanged");
    return failures == 0 ? 0 : 1;
}
