#pragma once

#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Agent 测试步骤与测试场景共用：在文档副本里解析要读取的全局变量、建立一个临时类别和触发器。
namespace War3Map::Services::detail {

struct TestRead {
    std::string request;  // 请求原文
    std::string name;     // 不带 udg_
    std::string index;    // 数组下标原文，非数组为空
    const Trigger::VariableRecord* variable = nullptr;
};

// 读取项写法为 名字 或 名字[下标]，可带 udg_；最多 32 项。错误码为 <codePrefix>_READS / <codePrefix>_READ_UNKNOWN。
[[nodiscard]] Result<std::vector<TestRead>> ResolveTestReads(const Trigger::TriggerDocument& copy,
    std::span<const std::string> reads, std::string_view subsystem, std::string_view codePrefix);

// 建立类别 category 及其下的触发器 trigger，正文为紧凑 T 文本 text；名字已被占用时以 <codePrefix>_NAME_TAKEN 拒绝。
[[nodiscard]] Result<StableId> AddTestTrigger(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog,
    std::string_view category, std::string_view trigger, std::string_view keyPrefix, std::string_view text,
    std::string_view subsystem, std::string_view codePrefix, const Trigger::MapObjectReferenceSnapshot* mapObjects);

} // namespace War3Map::Services::detail
