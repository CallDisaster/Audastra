#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <memory>
#include <span>
#include <string_view>

namespace War3Map::Services {

class ObjectTransactionSnapshot;

// 已检查模板产物的物编提交边界。对全部相关类别按明确名字捕获真实源字节；
// 封印类别只允许同一 candidate，其他类别只允许原字节。没有生成的封印也须
// 验证来源身份。本函数不编译脚本、不发布归档、不改变任何冻结输入。
[[nodiscard]] Status ValidateGeneratedObjectMembersForSave(
    const MapArchiveSession& source,
    std::span<const std::shared_ptr<const ObjectTransactionSnapshot>> snapshots,
    std::span<const MemberReplacement> generated,
    std::string_view scriptMember);

} // namespace War3Map::Services
