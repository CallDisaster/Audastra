#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <array>
#include <map>

namespace War3Map::Script {

// 完整 Lua 对象图的保守子集：标量及稠密一维数组，不是 native modification 列表。
// nil 由字段缺失表示。稀疏/嵌套表、metatable、函数等必须由物化器报告为未覆盖。
using SlkSessionScalar = std::variant<std::int64_t, double, std::string, bool>;
using SlkSessionValue = std::variant<SlkSessionScalar, std::vector<SlkSessionScalar>>;

struct SlkSessionObject {
    std::string id;
    // 保留 _id/_type/_code/_parent/_obj/_max_level/w2lobject 等真实内部字段。
    // _create 是 eq_obj 唯一忽略的字段；字符串已是所属一侧的值，不再解析 WTS。
    std::map<std::string, SlkSessionValue> fields;
};

enum class SlkSessionFieldType { Unknown, Number, String, Boolean };
struct SlkSessionFieldDefinition {
    std::string name; // frontend/new 使用的已解析规范字段名，不是 rawcode 或别名
    SlkSessionFieldType type = SlkSessionFieldType::Unknown;
    bool array = false;
};

enum class SlkSessionSourcePresence { Unknown, Missing, Present };
struct SlkSessionSourceIdentity {
    SlkSessionSourcePresence presence = SlkSessionSourcePresence::Unknown;
    std::string sha256; // Present 必填；Missing 必须为空
    bool operator==(const SlkSessionSourceIdentity&) const = default;
};

struct SlkSessionBindings {
    // 所有摘要均绑定最终候选，不得混用源图、旧 revision 或另一模板展开顺序。
    std::string candidateSha256;
    std::string orderedTemplatesSha256;
    std::string materializerSha256;
    std::string slkLibrarySha256;
    std::string frontendModelSha256;
    std::string metadataSha256;
    std::string dataSetIdentity;
    std::uint64_t documentRevision = 0;
    SlkSessionSourceIdentity strings;
    SlkSessionSourceIdentity mapInfo;
    std::array<SlkSessionSourceIdentity, 7> nativeMembers;
    std::array<std::string, 7> defaultsSha256;
    bool operator==(const SlkSessionBindings&) const = default;
};

enum class SlkSessionMaterialization {
    Unverified,
    CompleteDefaultsMetadataOverlayAndOrderedWrites,
};

struct SlkSessionClass {
    ObjectData::ObjectType type = ObjectData::ObjectType::Unit;
    SlkSessionMaterialization materialization = SlkSessionMaterialization::Unverified;
    std::vector<SlkSessionFieldDefinition> metadata;
    std::vector<SlkSessionObject> standardDefaults; // 原始标准 default，而非地图覆盖后的父对象
    std::vector<SlkSessionObject> existingObjects; // 完整 frontend 结果，在 mark_obj 移走 generated 之前
    std::vector<SlkSessionObject> createdObjects; // 全部模板依次执行后、update_change 之前的静态 new
};

struct SlkTemplateSession {
    SlkSessionBindings bindings;
    // 调用方必须用完整模板序列证明：仅静态 new/有序字段写入，无 permanent、外部副作用、
    // 对现有对象的直接修改或 misc 修改。默认 false，空计划也不能自动推断此事实。
    bool completeStaticCreationSession = false;
    // 已物化时遇到未知 default、字段/metadata、来源覆盖、fill、WTS、Lua 类型等必须列入；
    // 不得通过删掉未知字段后设置 materialization 来伪造完整图。
    std::vector<std::string> unresolvedSemantics;
    std::vector<Diagnostic> materializationDiagnostics;
    // 顺序固定为 Unit, Ability, Item, Buff, Upgrade, Doodad, Destructable。
    std::array<SlkSessionClass, 7> classes;
};

enum class SlkSessionProofCoverage { Unproven, WholeSessionNoObjectWrites };
struct SlkSessionClassProof {
    ObjectData::ObjectType type = ObjectData::ObjectType::Unit;
    std::size_t existingObjectCount = 0;
    std::size_t generatedObjectCount = 0;
    std::size_t matchedObjectCount = 0;
    std::size_t remainingNew = 0, remainingOld = 0, changed = 0;
};
struct SlkTemplateSessionProof {
    SlkSessionProofCoverage coverage = SlkSessionProofCoverage::Unproven;
    SlkSessionBindings bindings;
    std::array<SlkSessionClassProof, 7> classes;
};

// 纯内存、无 I/O。成功只证明所给完整语义图在已审计 refresh 中七类均不需写入。
// 这是比较器，不是物化器：它不从摘要证明图的完整性，也不替调用方验证实际候选字节。
// 生产 adapter 尚未接入；不得仅构造 coverage 标志或借 ExplicitAssignmentsOnly 放行模板。
// 支持相同整数、字符串/boolean、数值相同的有限 double；不同 double 或 int/float 混合
// 的 around_number 等价性未获 oracle，明确拒绝。不会把数值先压成 Float32。
// 按 new→old 单向 eq_obj 比较（含数组前缀、忽略 _create）；old 独有字段可保留，
// 但只在七类均无 new/old/change 时成功。任一变化不以 backend_cleanobj 抵消，须整类物化。
// 失败不返回部分凭据；hard budget 为全部输入 100000 个对象、2000000 个字段/数组元素、
// 64 MiB 字符串、单字段名 256 字节、单字符串 1 MiB。
[[nodiscard]] Result<SlkTemplateSessionProof> ProveSlkTemplateSessionNoObjectWrites(
    const SlkTemplateSession& session);

// 正向凭据只可复用于完全相同的候选/模板/default/metadata/WTS/W3I 身份。
[[nodiscard]] bool SlkTemplateSessionProofMatches(
    const SlkTemplateSessionProof& proof, const SlkSessionBindings& bindings) noexcept;

} // namespace War3Map::Script
