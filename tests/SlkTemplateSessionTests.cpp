#include "War3MapCore/Script/SlkTemplateSession.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

// 原创纯内存图；摘要是测试身份，不是外部工具/地图 oracle。无需 Lua 或游戏数据。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
using ObjectType = ObjectData::ObjectType;
constexpr std::array<ObjectType, 7> kTypes{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
    ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
constexpr std::array<std::string_view, 7> kNames{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::string_view kLibrary = "9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1";

void Require(bool condition, std::string_view message) { if (!condition) throw std::runtime_error(std::string(message)); }
SlkSessionValue Scalar(SlkSessionScalar value) { return value; }
SlkSessionValue Array(std::vector<SlkSessionScalar> values) { return values; }
SlkSessionValue String(std::string_view value) { return Scalar(std::string(value)); }
SlkSessionBindings Bindings() {
    SlkSessionBindings result;
    result.candidateSha256 = std::string(64, '1');
    result.orderedTemplatesSha256 = std::string(64, '2');
    result.materializerSha256 = std::string(64, '3');
    result.slkLibrarySha256 = kLibrary;
    result.frontendModelSha256 = std::string(64, '4');
    result.metadataSha256 = std::string(64, '5');
    result.dataSetIdentity = "原创测试默认集";
    result.documentRevision = 17;
    result.strings = {SlkSessionSourcePresence::Missing, {}};
    result.mapInfo = {SlkSessionSourcePresence::Present, std::string(64, '6')};
    for (std::size_t i = 0; i < 7; ++i) {
        result.defaultsSha256[i] = std::string(64, 'a');
        result.nativeMembers[i] = {SlkSessionSourcePresence::Present, std::string(64, 'b')};
    }
    return result;
}
SlkSessionObject Parent(std::size_t type) {
    SlkSessionObject result;
    result.id = "P001";
    result.fields = {{"_id", String(result.id)}, {"_type", String(kNames[type])}, {"_code", String(result.id)},
        {"_max_level", Scalar(std::int64_t{1})}, {"name", String("原创对象")},
        {"levels", Scalar(std::int64_t{1})}, {"data", Array({0.5})}, {"flag", Scalar(true)}};
    return result;
}
SlkSessionObject Generated(std::size_t type, bool created) {
    auto result = Parent(type);
    result.id = "N001";
    result.fields["_id"] = String(result.id);
    result.fields["_parent"] = String("P001");
    result.fields["_obj"] = Scalar(true);
    result.fields["w2lobject"] = String("static");
    if (created) result.fields["_create"] = Scalar(true);
    return result;
}
SlkTemplateSession Session() {
    SlkTemplateSession result;
    result.bindings = Bindings();
    result.completeStaticCreationSession = true;
    for (std::size_t i = 0; i < 7; ++i) {
        auto& type = result.classes[i];
        type.type = kTypes[i];
        type.materialization = SlkSessionMaterialization::CompleteDefaultsMetadataOverlayAndOrderedWrites;
        type.metadata = {{"name", SlkSessionFieldType::String, false}, {"levels", SlkSessionFieldType::Number, false},
            {"data", SlkSessionFieldType::Number, true}, {"flag", SlkSessionFieldType::Boolean, false},
            {"legacy", SlkSessionFieldType::String, false}};
        type.standardDefaults.push_back(Parent(i));
        type.existingObjects = {Parent(i), Generated(i, false)};
        type.createdObjects.push_back(Generated(i, true));
    }
    return result;
}
SlkTemplateSessionProof Proved(const SlkTemplateSession& session) {
    const auto result = ProveSlkTemplateSessionNoObjectWrites(session);
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "缺少失败诊断" : result.diagnostics.front().message);
    Require(result.diagnostics.empty(), "正向证明不能夹带未处理诊断");
    Require(result.value->coverage == SlkSessionProofCoverage::WholeSessionNoObjectWrites, "覆盖范围错误");
    Require(SlkTemplateSessionProofMatches(*result.value, session.bindings), "凭据没有绑定原输入");
    return *result.value;
}
void Rejected(const SlkTemplateSession& session, std::string_view code) {
    const auto result = ProveSlkTemplateSessionNoObjectWrites(session);
    Require(!result.Succeeded() && !result.value, "失败不能返回部分会话凭据");
    Require(!result.diagnostics.empty(), "缺少拒绝诊断");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    }), "拒绝诊断类别错误");
    for (const auto& diagnostic : result.diagnostics) {
        Require(diagnostic.severity == DiagnosticSeverity::Error && diagnostic.origin.has_value(), "错误须定位会话/类别/对象");
        Require(diagnostic.origin->fingerprint == session.bindings.candidateSha256, "错误身份未绑定候选");
    }
}

void TestWholeSessionAndDirectionalComparison() {
    auto session = Session();
    const auto proof = Proved(session);
    for (std::size_t i = 0; i < 7; ++i) {
        const auto& item = proof.classes[i];
        Require(item.type == kTypes[i] && item.existingObjectCount == 2 && item.generatedObjectCount == 1 &&
            item.matchedObjectCount == 1 && item.remainingNew == 0 && item.remainingOld == 0 && item.changed == 0,
            "必须证明全部七类 new/old/change 均为空");
    }
    // 真正单向 eq_obj：旧对象额外字段/数组尾部不导致写入，_create 被忽略。
    auto& old = session.classes[1].existingObjects[1];
    old.fields["legacy"] = String("保留旧扩展值");
    old.fields["data"] = Array({0.5, 0.75});
    old.fields["_create"] = Scalar(false);
    (void)Proved(session);
    session.classes[2].createdObjects[0].fields["name"] = String("变化对象");
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES"); // 不能用 ability 的 no-op 掩盖 item 重写

    session = Session();
    session.classes[1].createdObjects[0].fields["data"] = Array({0.5, 0.75});
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES"); // new 独有数组尾项与 old 尾项不同
    session = Session();
    session.classes[1].createdObjects[0].fields["legacy"] = String("新字段");
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES");
}

void TestLifecycleAndGlobalCleanup() {
    for (std::size_t i = 0; i < 7; ++i) {
        auto session = Session();
        session.classes[i].createdObjects.clear();
        Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES");
        session = Session();
        session.classes[i].existingObjects.pop_back();
        Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES");
    }
    auto session = Session();
    for (auto& type : session.classes) type.createdObjects.clear();
    const auto emptyPlan = ProveSlkTemplateSessionNoObjectWrites(session);
    Require(!emptyPlan.value && emptyPlan.diagnostics.size() == 7, "仅 require slk 也必须检查七类待清理对象");
    for (auto& type : session.classes) type.existingObjects.pop_back();
    (void)Proved(session); // 无 generated 时，空模板会话才可证明不写入

    session = Session();
    session.classes[1].existingObjects[1].fields.erase("w2lobject");
    Rejected(session, "SLK_SESSION_CREATION_COLLISION");
    session = Session();
    session.classes[1].existingObjects[1].fields["w2lobject"] = Scalar(false);
    Rejected(session, "SLK_SESSION_CREATION_COLLISION");
    session = Session();
    session.classes[1].existingObjects[1].fields["w2lobject"] = String("");
    session.classes[1].createdObjects.clear();
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES"); // 空字符串在 Lua 中是真
    session = Session();
    session.classes[1].existingObjects[1].fields["w2lobject"] = String("dynamic|original-label");
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES");
    session = Session();
    session.classes[1].existingObjects[1].fields["w2lobject"] = Scalar(true);
    Rejected(session, "SLK_SESSION_MARKER_UNSUPPORTED");
    session = Session();
    session.classes[1].createdObjects.push_back(session.classes[1].createdObjects.front());
    Rejected(session, "SLK_SESSION_DUPLICATE_OBJECT");
    session = Session();
    session.classes[1].createdObjects[0].id = "P001";
    session.classes[1].createdObjects[0].fields["_id"] = String("P001");
    Rejected(session, "SLK_SESSION_CREATION_COLLISION");
}

void TestUnknownMaterializationIsNotProof() {
    auto session = Session();
    session.completeStaticCreationSession = false;
    Rejected(session, "SLK_SESSION_SCOPE_UNPROVEN");
    session = Session();
    session.unresolvedSemantics.push_back("未知地图 SLK 来源优先级");
    Rejected(session, "SLK_SESSION_MATERIALIZATION_UNPROVEN");
    session = Session();
    session.materializationDiagnostics.push_back({DiagnosticSeverity::Warning, "fixture", "unknown", "未识别字段", std::nullopt});
    Rejected(session, "SLK_SESSION_MATERIALIZATION_UNPROVEN");
    session = Session();
    session.classes[6].materialization = SlkSessionMaterialization::Unverified;
    Rejected(session, "SLK_SESSION_MATERIALIZATION_UNPROVEN");
    session = Session();
    session.classes[6].type = ObjectType::Ability;
    Rejected(session, "SLK_SESSION_CLASS_COVERAGE");
    session = Session();
    session.classes[1].metadata[0].type = SlkSessionFieldType::Unknown;
    Rejected(session, "SLK_SESSION_METADATA_UNPROVEN");
    session = Session();
    session.classes[1].existingObjects[1].fields["unknown"] = String("不可删除后当作完整图");
    Rejected(session, "SLK_SESSION_METADATA_UNPROVEN");
    session = Session();
    session.classes[1].createdObjects[0].fields.erase("flag");
    session.classes[1].existingObjects[1].fields.erase("flag");
    Rejected(session, "SLK_SESSION_OBJECT_INCOMPLETE"); // 两侧同样稀疏也不能跳过默认字段
    session = Session();
    session.classes[1].createdObjects[0].fields["_parent"] = String("MISS");
    Rejected(session, "SLK_SESSION_DEFAULT_UNPROVEN");
    session = Session();
    session.classes[1].createdObjects[0].fields["name"] = Scalar(true);
    Rejected(session, "SLK_SESSION_FIELD_TYPE");
    session = Session();
    session.classes[1].createdObjects[0].fields["_create"] = Scalar(false);
    Rejected(session, "SLK_SESSION_CREATION_UNPROVEN");
    session = Session();
    session.classes[1].existingObjects[1].fields["legacy"] = String(std::string(1024 * 1024 + 1, 'a'));
    Rejected(session, "SLK_SESSION_STRING_UNSUPPORTED");
}

void TestNumericAndStringBoundaries() {
    auto session = Session();
    session.classes[1].existingObjects[1].fields["data"] = Array({-0.0});
    session.classes[1].createdObjects[0].fields["data"] = Array({0.0});
    (void)Proved(session); // around_number 对正负零结果相同，旧 native 位模式仍不改写
    session.classes[1].createdObjects[0].fields["data"] = Array({std::int64_t{0}});
    Rejected(session, "SLK_SESSION_ROUNDING_UNPROVEN");
    session = Session();
    session.classes[1].existingObjects[1].fields["data"] = Array({static_cast<double>(static_cast<float>(1.00005))});
    session.classes[1].createdObjects[0].fields["data"] = Array({1.00005});
    Rejected(session, "SLK_SESSION_ROUNDING_UNPROVEN"); // 不能凭相同 Float32 位放行
    session = Session();
    session.classes[1].existingObjects[1].fields["data"] = Array({1.00001});
    session.classes[1].createdObjects[0].fields["data"] = Array({1.00002});
    Rejected(session, "SLK_SESSION_ROUNDING_UNPROVEN"); // 即使数学四位舍入看似相同，也不猜目标运行时
    session = Session();
    session.classes[1].createdObjects[0].fields["data"] = Array({std::numeric_limits<double>::quiet_NaN()});
    Rejected(session, "SLK_SESSION_NUMBER_UNSUPPORTED");
    session = Session();
    session.classes[1].existingObjects[1].fields["name"] = String("前缀TRIGSTR_7后缀");
    session.classes[1].createdObjects[0].fields["name"] = String("前缀TRIGSTR_7后缀");
    Rejected(session, "SLK_SESSION_WTS_UNPROVEN"); // 两侧相同键也不构成 WTS 语义证明
    session = Session();
    session.classes[1].createdObjects[0].fields["name"] = String("不同原文字节");
    Rejected(session, "SLK_SESSION_REQUIRES_OBJECT_WRITES");
}

void TestProofIdentityAndDefaults() {
    const auto session = Session();
    const auto proof = Proved(session);
    auto changed = session.bindings;
    ++changed.documentRevision;
    Require(!SlkTemplateSessionProofMatches(proof, changed), "文档编辑后旧凭据须失效");
    changed = session.bindings;
    changed.orderedTemplatesSha256 = std::string(64, 'c');
    Require(!SlkTemplateSessionProofMatches(proof, changed), "模板序列变化后旧凭据须失效");
    changed = session.bindings;
    changed.strings = {SlkSessionSourcePresence::Present, std::string(64, 'c')};
    Require(!SlkTemplateSessionProofMatches(proof, changed), "WTS 变化后旧凭据须失效");
    for (std::size_t i = 0; i < 7; ++i) {
        changed = session.bindings;
        changed.defaultsSha256[i] = std::string(64, 'c');
        Require(!SlkTemplateSessionProofMatches(proof, changed), "任一类 default 改变后旧凭据须失效");
        changed = session.bindings;
        changed.nativeMembers[i] = {SlkSessionSourcePresence::Missing, {}};
        Require(!SlkTemplateSessionProofMatches(proof, changed), "任一 native 成员改变后旧凭据须失效");
    }
    Require(!SlkTemplateSessionProofMatches(SlkTemplateSessionProof{}, session.bindings), "默认构造不能伪装正向凭据");
    auto invalid = session;
    invalid.bindings.nativeMembers[6] = {};
    Rejected(invalid, "SLK_SESSION_BINDINGS_INCOMPLETE");
    invalid = session;
    invalid.bindings.defaultsSha256[1].clear();
    Rejected(invalid, "SLK_SESSION_BINDINGS_INCOMPLETE");
    invalid = session;
    invalid.bindings.slkLibrarySha256 = std::string(64, 'f');
    Rejected(invalid, "SLK_SESSION_MODEL_UNSUPPORTED");
}
} // namespace

int main() {
    try {
        TestWholeSessionAndDirectionalComparison();
        TestLifecycleAndGlobalCleanup();
        TestUnknownMaterializationIsNotProof();
        TestNumericAndStringBoundaries();
        TestProofIdentityAndDefaults();
        std::cout << "SlkTemplateSession tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
