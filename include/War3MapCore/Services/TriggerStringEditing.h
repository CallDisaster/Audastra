#pragma once

#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

namespace War3Map::Services {

enum class TriggerStringEditScope { AllReferences, OnlyThisArgument };
struct TriggerStringUsage {
    std::size_t count = 0;
    bool complete = false;
    std::vector<std::string> coverage, limitations;
};

class TriggerStringEditPlan {
public:
    [[nodiscard]] const TriggerStringUsage& Usage() const noexcept { return usage_; }
    [[nodiscard]] const Trigger::ArgumentNode& Before() const noexcept { return before_; }
    [[nodiscard]] const Trigger::ArgumentNode& After() const noexcept { return after_; }
    [[nodiscard]] bool ChangedReference() const noexcept { return before_.value.text != after_.value.text; }
private:
    friend Result<TriggerStringEditPlan> PrepareTriggerStringEdit(const NameResolutionContext&, StableId,
        std::string_view, TriggerStringEditScope);
    friend Result<class TriggerStringEditReceipt> ApplyTriggerStringEdit(Trigger::TriggerDocument&,
        MapData::MapDataEditSession&, const TriggerStringEditPlan&);
    Trigger::ArgumentNode before_, after_;
    MapData::StringTableDocument stringsAfter_;
    std::vector<std::byte> stringsBeforeBytes_;
    TriggerStringUsage usage_;
};

class TriggerStringEditReceipt {
public:
    [[nodiscard]] StableId ArgumentId() const noexcept { return before_.id; }
    [[nodiscard]] const MapData::MapDataEditSession::EditReceipt& StringsReceipt() const noexcept { return strings_; }
private:
    friend Result<TriggerStringEditReceipt> ApplyTriggerStringEdit(Trigger::TriggerDocument&,
        MapData::MapDataEditSession&, const TriggerStringEditPlan&);
    friend Status ReplayTriggerStringEdit(Trigger::TriggerDocument&, MapData::MapDataEditSession&,
        const TriggerStringEditReceipt&, bool);
    Trigger::ArgumentNode before_, after_;
    MapData::MapDataEditSession::EditReceipt strings_;
};

[[nodiscard]] TriggerStringUsage CountTriggerStringUsage(const NameResolutionContext&, std::string_view reference);
[[nodiscard]] Result<TriggerStringEditPlan> PrepareTriggerStringEdit(const NameResolutionContext&,
    StableId argumentId, std::string_view newTextUtf8, TriggerStringEditScope);
// 先完成所有分配/验证，再提交 WTS 并 noexcept swap 单个参数。两者只属于一次共同撤销。
[[nodiscard]] Result<TriggerStringEditReceipt> ApplyTriggerStringEdit(Trigger::TriggerDocument&,
    MapData::MapDataEditSession&, const TriggerStringEditPlan&);
[[nodiscard]] Status ReplayTriggerStringEdit(Trigger::TriggerDocument&, MapData::MapDataEditSession&,
    const TriggerStringEditReceipt&, bool forward);

} // namespace War3Map::Services
