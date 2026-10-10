#pragma once

// 一次完整 ValidateGraph 内的私有只读查找优化；不授来源/编辑资格。
#include "War3MapCore/Trigger/UiCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace War3Map::Trigger::Detail {

struct CodecDefinitionLookup {
    const UiDefinition* definition = nullptr;
    std::size_t argumentCount = 0;
};

class CodecDefinitionLookupMemo final {
public:
    static constexpr std::size_t kEntries = 512;
    explicit CodecDefinitionLookupMemo(const UiCatalog& catalog) noexcept : catalog_(catalog) {}
    CodecDefinitionLookupMemo(const CodecDefinitionLookupMemo&) = delete;
    CodecDefinitionLookupMemo& operator=(const CodecDefinitionLookupMemo&) = delete;

    [[nodiscard]] CodecDefinitionLookup Lookup(std::int32_t type, std::string_view name) {
        return Lookup(UiDefinitionKindFromEcaType(type), name);
    }
    [[nodiscard]] CodecDefinitionLookup Lookup(
        UiDefinitionKind kind, std::string_view name, std::string_view group = {}) {
        // hash 仅路由到一槽；实际命中仍逐字段核 kind/group/name 完整相等。
        const auto nameHash = std::hash<std::string_view>{}(name);
        const auto groupHash = std::hash<std::string_view>{}(group);
        const auto slot = (nameHash ^ (groupHash << 1) ^ static_cast<std::size_t>(kind)) & (kEntries - 1);
        auto& entry = entries_[slot];
        if (entry.definition != nullptr && entry.definition->kind == kind
            && entry.definition->group == group && entry.definition->name == name) return entry;

        // 空槽/碰撞/满表/未知项均执行原 Find；原 Find/ArgumentCount 异常原样传播。
        const auto* definition = catalog_.Find(kind, name, group);
        const CodecDefinitionLookup result{definition, definition != nullptr ? definition->ArgumentCount() : 0};
        if (entry.definition == nullptr && definition != nullptr && definition->kind == kind
            && definition->group == group && definition->name == name) entry = result;
        return result;
    }

private:
    // 只借当前调用的 const catalog；每槽键来自其真实 definition，无字符串复制/堆申请。
    const UiCatalog& catalog_;
    std::array<CodecDefinitionLookup, kEntries> entries_{};
};

static_assert((CodecDefinitionLookupMemo::kEntries & (CodecDefinitionLookupMemo::kEntries - 1)) == 0);
static_assert(sizeof(CodecDefinitionLookupMemo) <= 32 * 1024);
// 原 Validate 的 16KiB ID arena 在 ValidateGraph 时仍存活，与 memo 同时收费。
inline constexpr std::size_t kCodecFixedValidationScratchBytes = 16 * 1024 + sizeof(CodecDefinitionLookupMemo);

} // namespace War3Map::Trigger::Detail
