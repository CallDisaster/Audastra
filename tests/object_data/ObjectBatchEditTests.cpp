#include "War3MapCore/ObjectData/ObjectBatchEdit.h"
#include "War3MapCore/ObjectData/LevelAutofill.h"

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

// 自动填充等级：上一级×因素 + 等级×因素 + 固定值；整数出现小数时报错不取整；文本替换等级记号。
void LevelAutofillSequences() {
    LevelAutofill damage;
    damage.startLevel = 2; damage.endLevel = 4; damage.base = 100; damage.previousFactor = 1; damage.levelFactor = 10; damage.constant = 5;
    const auto integers = ComputeLevelAutofill(damage, LevelAutofillKind::Integer);
    Require(integers.Succeeded() && integers.value->size() == 3, "integer sequence");
    Require((*integers.value)[0].level == 2 && (*integers.value)[0].text == "100", "start level keeps base");
    Require((*integers.value)[1].text == "135" && (*integers.value)[2].text == "180", "previous + level + constant");
    auto scaled = damage; scaled.previousFactor = 1.5; scaled.levelFactor = 0; scaled.constant = 0; scaled.base = 3;
    const auto fraction = ComputeLevelAutofill(scaled, LevelAutofillKind::Integer);
    Require(!fraction.Succeeded() && fraction.diagnostics.front().code == "AUTOFILL_INTEGER_FRACTION", "integer fraction rejected");
    const auto reals = ComputeLevelAutofill(scaled, LevelAutofillKind::Real);
    Require(reals.Succeeded() && (*reals.value)[1].text == "4.5" && (*reals.value)[2].text == "6.75", "real sequence");
    LevelAutofill text; text.startLevel = 1; text.endLevel = 3; text.baseText = "L#: #0"; text.levelToken = "#";
    const auto texts = ComputeLevelAutofill(text, LevelAutofillKind::Text);
    Require(texts.Succeeded() && (*texts.value)[2].text == "L3: 30", "level token replaced everywhere");
    auto backwards = damage; backwards.startLevel = 5;
    Require(!ComputeLevelAutofill(backwards, LevelAutofillKind::Integer).Succeeded(), "start after end rejected");
    auto huge = damage; huge.previousFactor = 1e10; huge.endLevel = 40;
    Require(!ComputeLevelAutofill(huge, LevelAutofillKind::Integer).Succeeded(), "integer overflow rejected");
}

} // namespace

int main() {
    try {
        AtomicBatch();
        LevelAutofillSequences();
        std::cout << "ObjectBatchEdit: atomic multi-object edit and level autofill passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
