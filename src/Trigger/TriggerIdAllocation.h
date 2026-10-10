#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <algorithm>
#include <cstdint>

// 触发器编辑内部共用：新建记录的稳定 ID 与文本编码。只供 src/Trigger 内部包含。
namespace War3Map::Trigger::detail {

inline constexpr std::uint64_t kGeneratedDomain = 0x7e00000000000000ULL;
inline constexpr std::uint64_t kGeneratedMask = 0x00ffffffffffffffULL;

// 新文本沿用文档现有编码（原版中文地图为 GB18030）。
inline TextEncoding NewTextEncoding(const TriggerDocument& document) {
    if (!document.triggers.empty()) {
        return document.triggers.front().name.encoding;
    }
    if (!document.categories.empty()) {
        return document.categories.front().name.encoding;
    }
    return TextEncoding::Gb18030;
}

// 生成域内的下一个计数：扫描全部记录，保证新 ID 不与已有生成 ID 冲突。
inline std::uint64_t NextGeneratedCounter(const TriggerDocument& document) {
    std::uint64_t next = 1;
    const auto inspect = [&next](StableId id) {
        if ((id & ~kGeneratedMask) == kGeneratedDomain) {
            next = std::max(next, (id & kGeneratedMask) + 1);
        }
    };
    for (const auto& value : document.categories) inspect(value.id);
    for (const auto& value : document.variables) inspect(value.id);
    for (const auto& value : document.triggers) inspect(value.id);
    for (const auto& value : document.ecaNodes) inspect(value.id);
    for (const auto& value : document.argumentNodes) inspect(value.id);
    return next;
}

// 分配失败（空间耗尽）返回 0。
inline StableId AllocateId(std::uint64_t& counter) {
    if (counter == 0 || counter > kGeneratedMask) {
        return 0;
    }
    return kGeneratedDomain | counter++;
}

} // namespace War3Map::Trigger::detail
