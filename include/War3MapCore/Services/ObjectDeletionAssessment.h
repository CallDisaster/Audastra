#pragma once

#include "War3MapCore/Services/ProjectReferenceIndex.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Services {

// 当前服务只做删除前判断，不签发 DeletionAuditToken。Eligible 保留给将来能证明
// JASS/WCT、动态取值和全部地图成员均已扫描的审计实现；当前输入模型无法证明这些条件。
enum class ObjectDeletionDisposition : std::uint8_t {
    BlockedReferences,
    BlockedUnknown,
    Eligible,
};

struct ObjectDeletionReason {
    std::string code;
    std::string source;
    std::string message;
};

// 可由归档层提供实际脚本字节，以展示可疑位置。名称与字节都不声明清单完整，
// 即使所有提供的脚本没有匹配，也不能据此授权删除。
struct ScriptSourceInput {
    std::string name;
    std::span<const std::byte> bytes;
};

struct ScriptReferenceHint {
    std::string source;
    std::size_t byteOffset = 0;
    std::size_t line = 1;
};

struct DynamicValueRisk {
    std::size_t argumentIndex = 0;
    bool variable = false;
    bool functionCall = false;
    bool arrayIndex = false;
    std::string text;
};

struct ObjectDeletionAssessment {
    ObjectData::ObjectKey subject;
    ObjectDeletionDisposition disposition = ObjectDeletionDisposition::BlockedUnknown;
    // 类型明确的静态引用可直接阻断删除；类型不明确与 GUI 记号只能作为未确定来源。
    ObjectData::ObjectReferenceMatches objectReferences;
    std::vector<Usage> untypedGuiReferences;
    std::vector<ObjectData::ReferenceInputCoverage> coverage;
    // 脚本是文字候选位置，可能位于注释/字符串；不是语义解析的确定引用。
    std::vector<ScriptReferenceHint> scriptHints;
    std::size_t scriptHintCount = 0;
    std::vector<DynamicValueRisk> dynamicValues;
    std::size_t dynamicValueCount = 0;
    std::vector<ObjectDeletionReason> reasons;
};

// 从实际工程输入重新计算，不接受外部声称已扫描完成的位掩码。
// 无论静态索引是否零命中，当前 ProjectReferenceInputs 均不携带可信的脚本来源/动态值证明，
// suppliedScripts 仅供定位候选文本，不能证明脚本清单或动态表达式完整。
// 因此此实现只能返回 BlockedReferences 或 BlockedUnknown，绝不将结果用于取得删除令牌。
[[nodiscard]] ObjectDeletionAssessment AssessObjectDeletion(
    const ProjectReferenceInputs& inputs,
    const ObjectData::ObjectKey& subject,
    std::span<const ScriptSourceInput> suppliedScripts = {});

} // namespace War3Map::Services
