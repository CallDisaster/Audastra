#pragma once
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/WtgSource.h"
namespace War3Map::Trigger::Detail {
// 经典编辑入口仍严格要求 v7 及完整匹配的 WCT；不会发放来源 seal。
TriggerDocument DecodeClassicWtg(std::span<const std::byte> bytes, const UiCatalog& catalog,
    const std::vector<RawText>& slots, const WctHeader& header);
WtgSourceSnapshot DecodeWtgSource(std::span<const std::byte> bytes, const UiCatalog& catalog,
    const WtgSourceLimits& limits);
} // namespace War3Map::Trigger::Detail
