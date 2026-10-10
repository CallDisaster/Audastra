#include "War3MapCore/ObjectData/ObjectBatchEdit.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map::ObjectData;

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return {begin, begin + value.size()};
}

void AtomicBatch() {
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes(
        "[hfoo]\nuhpm=420\n\n[hrif]\nuhpm=360\n"));
    Require(parsed.Succeeded(), "synthetic batch document parses");
    const auto* source = dynamic_cast<LniObjectDocument*>(parsed.document.get());
    Require(source != nullptr, "document type");
    const auto fingerprint = source->DocumentFingerprint();
    const std::array targets{ObjectKey{ObjectType::Unit, "hfoo"}, ObjectKey{ObjectType::Unit, "hrif"}};
    const ModificationKey health{"uhpm"};
    const auto changed = ApplyBatchModification(*source, targets, health, std::int32_t{975});
    Require(changed.Succeeded(), "batch edit succeeds");
    Require(source->DocumentFingerprint() == fingerprint, "source remains unchanged until caller commits");
    for (const auto& key : targets) {
        const auto* record = changed.value->FindObject(key);
        Require(record && record->modifications.size() == 1
            && std::get<std::int32_t>(record->modifications[0].value) == 975,
            "each target receives the same final value");
    }
    auto invalid = targets;
    invalid[1].rawcode = "miss";
    Require(!ApplyBatchModification(*source, invalid, health, std::int32_t{120}).Succeeded(),
        "one missing target rejects the entire batch");
    invalid = targets;
    invalid[1] = invalid[0];
    Require(!ApplyBatchModification(*source, invalid, health, std::int32_t{120}).Succeeded(),
        "duplicate target rejects the batch");
    invalid = targets;
    invalid[1].type = ObjectType::Ability;
    Require(!ApplyBatchModification(*source, invalid, health, std::int32_t{120}).Succeeded(),
        "cross-type batch rejects instead of guessing a field mapping");
    Require(source->DocumentFingerprint() == fingerprint, "all failed batches leave source unchanged");
}

} // namespace

int main() {
    try {
        AtomicBatch();
        std::cout << "ObjectBatchEdit: atomic multi-object edit passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
