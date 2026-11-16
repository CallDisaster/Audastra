#include "War3MapCore/ObjectData/ObjectBatchEdit.h"
#include "War3MapCore/ObjectData/LevelAutofill.h"
#include "War3MapCore/ObjectData/LevelCount.h"

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

// 技能等级数：提高时新等级沿用下面最近一级的值，已有值的等级与本来就在范围内的等级不动；不提高时不补。
void LevelCountFillsAddedLevels() {
    ObjectFieldDefinition levels; levels.objectType = ObjectType::Ability; levels.sourceField = "levels"; levels.sourceTable = "AbilityData";
    ObjectFieldDefinition damage; damage.objectType = ObjectType::Ability; damage.sourceField = "Data"; damage.repeat = 1; damage.data = 1;
    ObjectFieldDefinition cooldown; cooldown.objectType = ObjectType::Ability; cooldown.sourceField = "Cool"; cooldown.repeat = 1;
    Require(IsLevelCountField(levels) && !IsLevelCountField(damage), "levels field recognised");
    Require(!IsPerLevelField(damage, {"Hbz1", 0u, 1u, {}}) && IsPerLevelField(damage, {"Hbz1", 2u, 1u, {}}), "level 0 is not a level");
    Require(LevelCountValue(std::int32_t{4}) == 4u && !LevelCountValue(std::int32_t{0}) && !LevelCountValue(std::string("4")), "count value");
    const std::array<LevelFieldValue, 5> fields{{
        {{"Hbz1", 1u, 1u, {}}, &damage, 10.0f},
        {{"Hbz1", 2u, 1u, {}}, &damage, 20.0f},
        {{"Hbz1", 5u, 1u, {}}, &damage, 99.0f},  // 之前调高时写过、调低后保留的第 5 级
        {{"acdn", 1u, 0u, {}}, &cooldown, 8.0f},
        {{"anam", 0u, 0u, {}}, &levels, std::int32_t{1}},
    }};
    Require(FillAddedLevels(fields, 4, 4).empty() && FillAddedLevels(fields, 4, 2).empty(), "no raise, no fill");
    const auto added = FillAddedLevels(fields, 1, 6);
    // Hbz1：2 已有不动，3、4 沿用 2，5 已有不动，6 沿用 5；acdn：2..6 沿用 1。
    Require(added.size() == 8, "added level count");
    Require(added[0].key.fieldId == "Hbz1" && added[0].key.level == 3u && std::get<float>(added[0].value) == 20.0f, "carry level 2");
    Require(added[1].key.level == 4u && std::get<float>(added[1].value) == 20.0f, "carry to level 4");
    Require(added[2].key.level == 6u && std::get<float>(added[2].value) == 99.0f && added[2].key.dataIndex == 1u, "carry kept level 5");
    Require(added[3].key.fieldId == "acdn" && added[3].key.level == 2u && std::get<float>(added[7].value) == 8.0f
        && added[7].key.level == 6u && added[7].definition == &cooldown, "cooldown carried from level 1");
    // 已在原等级数以内但缺值的等级不补（不替作者编造低等级数据）。
    const std::array<LevelFieldValue, 1> sparse{{{{"acdn", 3u, 0u, {}}, &cooldown, 5.0f}}};
    const auto raised = FillAddedLevels(sparse, 4, 5);
    Require(raised.size() == 1 && raised[0].key.level == 5u && std::get<float>(raised[0].value) == 5.0f, "only above old count");
}

} // namespace

int main() {
    try {
        AtomicBatch();
        LevelAutofillSequences();
        LevelCountFillsAddedLevels();
        std::cout << "ObjectBatchEdit: atomic multi-object edit and level autofill passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
