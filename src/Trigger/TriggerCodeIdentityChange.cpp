#include "War3MapCore/Trigger/TriggerCodeIdentityChange.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <algorithm>

namespace War3Map::Trigger {
namespace {
Status Bad(std::string message) { return Status::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_SOURCE_STALE", std::move(message)); }
bool Same(const RawText& a, const RawText& b) {
    const auto left = a.ToBytes(), right = b.ToBytes();
    return a.encoding == b.encoding && left.Succeeded() && right.Succeeded() && *left.value == *right.value;
}
Status MatchCode(const TriggerDocument& authorized, const TriggerDocument& edited) {
    if (!Same(edited.wct.customCode, authorized.wct.customCode) || !Same(edited.wct.customComment, authorized.wct.customComment))
        return Bad("全局代码或WCT头部已超出核定补丁/原始基线。");
    for (const auto& row : edited.triggers) {
        const auto found = std::find_if(authorized.triggers.begin(), authorized.triggers.end(), [&](const auto& t) { return t.id == row.id; });
        if (found == authorized.triggers.end()) {
            if (row.customTextFlag || !row.customText.text.empty() || !row.customText.originalBytes.empty())
                return Bad("出现服务未核定的新自定义代码槽。");
        } else if (found->customTextFlag != row.customTextFlag || !Same(found->customText, row.customText))
            return Bad("自定义代码槽已超出服务核定补丁/原始基线。");
    }
    for (const auto& row : authorized.triggers) if (row.customTextFlag || !row.customText.text.empty() || !row.customText.originalBytes.empty()) {
        if (std::none_of(edited.triggers.begin(), edited.triggers.end(), [&](const auto& t) { return t.id == row.id; }))
            return Bad("服务核定的自定义代码槽被移除。");
    }
    return Status::Ok();
}
}
Status PreparedTriggerCodeIdentityChange::ApplyTo(TriggerDocument& document, const UiCatalog& catalog) const {
    if (!after_) return Bad("没有服务核定的代码身份候选。");
    if (verify_) { const auto checked = verify_(); if (!checked.Succeeded()) return checked; }
    auto checked = VerifyTriggerGraphReplayIdentity(document, catalog, before_);
    if (!checked.Succeeded()) return checked;
    checked = WtgWctCodec{}.Validate(*after_, catalog); if (!checked.Succeeded()) return checked;
    if (document.codeIdentityAuthorization_) {
        checked = document.VerifyCodeIdentityAuthorization(document);
        if (!checked.Succeeded()) return checked;
    }
    auto candidate = *after_;
    // 许可只持有来源seal、头部与代码槽，不再保留一份ECA/参数/物编引用图。
    const auto capture = [](const TriggerDocument& value) {
        auto snapshot = std::make_shared<TriggerDocument>();
        snapshot->sourceSeal_ = value.sourceSeal_; snapshot->wct = value.wct;
        for (const auto& row : value.triggers) {
            TriggerRecord slot; slot.id = row.id; slot.customTextFlag = row.customTextFlag;
            slot.customText = row.customText; snapshot->triggers.push_back(std::move(slot));
        }
        return snapshot;
    };
    auto authorization = capture(candidate);
    // 固定两层：最终代码与首次身份操作前的代码基线；连续操作不吞掉原基线。
    authorization->codeIdentityAuthorization_ = document.codeIdentityAuthorization_
        ? document.codeIdentityAuthorization_->codeIdentityAuthorization_ : capture(document);
    candidate.codeIdentityAuthorization_ = std::move(authorization);
    document = std::move(candidate);
    return Status::Ok();
}
Status TriggerDocument::VerifyCodeIdentityAuthorization(const TriggerDocument& original) const {
    const auto* authorized = codeIdentityAuthorization_.get();
    if (!authorized || !HasTrustedSourceSeal() || SourceWtgSha256() != original.SourceWtgSha256()
        || SourceWctSha256() != original.SourceWctSha256() || authorized->SourceWtgSha256() != SourceWtgSha256()
        || authorized->SourceWctSha256() != SourceWctSha256() || authorized->UiCatalogSha256() != UiCatalogSha256())
        return Bad("没有绑定同一来源的受限代码身份许可。");
    const auto* baseline = authorized->codeIdentityAuthorization_.get();
    if (!baseline || baseline->codeIdentityAuthorization_ || baseline->SourceWtgSha256() != SourceWtgSha256()
        || baseline->SourceWctSha256() != SourceWctSha256()) return Bad("代码身份许可没有固定的原始基线。");
    auto matched = MatchCode(*authorized, *this); if (!matched.Succeeded()) return matched;
    // 图执行只核当前候选；Freeze始终另传真实原配对解析结果，核原基线，不能接纳先前的任意代码修改。
    return &original == this ? Status::Ok() : MatchCode(original, *baseline);
}
}
