#include "War3MapCore/Services/TriggerReplace.h"
#include "TriggerReplaceWriterInternal.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Text/Utf8Scalar.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <map>
#include <new>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Services {
namespace {
constexpr const char* kDomain = "Trigger.Replace";
constexpr std::size_t kText = 2048;
Status Fail(std::string code, std::string message) { return Status::Failure(kDomain, std::move(code), std::move(message)); }
template<class T> Result<T> Failed(const Status& status) { Result<T> out; out.diagnostics = status.diagnostics; return out; }
template<class T, class U> Result<T> Failed(const Result<U>& source) { Result<T> out; out.diagnostics = source.diagnostics; return out; }
bool Add(std::size_t& used, std::size_t count, std::size_t cap) {
    if (used > cap || count > cap - used) return false;
    used += count; return true;
}
bool Utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto scalar = Text::Detail::ReadUtf8Scalar(text, i);
        if (!scalar.Valid() || scalar.codepoint == 0) return false;
        i += scalar.bytes;
    }
    return true;
}
std::string Short(std::string_view value) {
    auto size = std::min(kText, value.size());
    if (size < value.size()) while (size && (static_cast<unsigned char>(value[size]) & 0xc0U) == 0x80U) --size;
    return std::string(value.substr(0, size));
}
// 每个请求只准备一次 KMP；近重复长文本的比较次数保持线性，不由用户输入制造 text*needle 扫描。
class LinearMatcher {
public:
    LinearMatcher(std::string_view needle, bool caseSensitive, TriggerReplaceMatch kind)
        : needle_(needle), caseSensitive_(caseSensitive), kind_(kind), prefix_(needle.size()) {
        for (auto& c : needle_) c = Fold(c);
        for (std::size_t i = 1, matched = 0; i < needle_.size(); ++i) {
            while (matched && needle_[i] != needle_[matched]) matched = prefix_[matched - 1];
            if (needle_[i] == needle_[matched]) ++matched; prefix_[i] = matched;
        }
    }
    bool Matches(std::string_view value) const {
        if (kind_ == TriggerReplaceMatch::Exact) {
            if (value.size() != needle_.size()) return false;
            for (std::size_t i = 0; i < value.size(); ++i) if (Fold(value[i]) != needle_[i]) return false;
            return true;
        }
        if (needle_.empty()) return true;
        std::size_t matched = 0;
        for (auto c : value) {
            c = Fold(c);
            while (matched && c != needle_[matched]) matched = prefix_[matched - 1];
            if (c == needle_[matched] && ++matched == needle_.size()) return true;
        }
        return false;
    }
private:
    char Fold(char value) const { return !caseSensitive_ && value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value; }
    std::string needle_; bool caseSensitive_; TriggerReplaceMatch kind_; std::vector<std::size_t> prefix_;
};
// 字段采用长度分隔；预算包括非命中来源和数字/边身份。不是 WTG/WCT 序列化。
using Writer = detail::TriggerReplaceWriter<Add>;

Status Limits(const TriggerReplaceLimits& l) {
    if (!l.pageLimit || l.pageLimit > 128 || !l.maxGraphRecords || l.maxGraphRecords > 1048576
        || !l.maxGraphEdges || l.maxGraphEdges > 2097152 || !l.maxScanTextBytes || l.maxScanTextBytes > 128 * 1024 * 1024
        || !l.maxResultBytes || l.maxResultBytes > 256 * 1024 || !l.maxSelection || l.maxSelection > 256
        || !l.maxTargetRecords || l.maxTargetRecords > 65536 || !l.maxTargetTextBytes || l.maxTargetTextBytes > 8 * 1024 * 1024
        || !l.maxNameRecords || l.maxNameRecords > 131072 || !l.maxNameTextBytes || l.maxNameTextBytes > 64 * 1024 * 1024
        || !l.maxPreviewDocumentBytes || l.maxPreviewDocumentBytes > 512 * 1024 * 1024
        || !l.maxPreviewWorkUnits || l.maxPreviewWorkUnits > 256 * 1024 * 1024
        || static_cast<unsigned>(l.scalarWorkPolicy) > static_cast<unsigned>(Trigger::TriggerScalarWorkPolicy::LegacyKnownWork))
        return Fail("TRIGGER_REPLACE_BUDGET_INVALID", "查找替换预算超过公开上限或为零。");
    return Status::Ok();
}
std::size_t KnownWorkCap(const TriggerReplaceLimits& limits) {
    return limits.scalarWorkPolicy == Trigger::TriggerScalarWorkPolicy::LegacyKnownWork
        ? limits.maxPreviewWorkUnits : std::numeric_limits<std::size_t>::max();
}
Status Context(const TriggerReplaceContext& c, std::string_view expected) {
    if (expected.empty() || expected.size() > 4096 || expected != c.revision || !Utf8(expected))
        return Fail("TRIGGER_REPLACE_REVISION_STALE", "当前工程真实修订与请求不一致。");
    if (!c.document || !c.catalog || !c.types || &c.types->Catalog() != c.catalog
        || !c.document->HasTrustedSourceSeal() || c.document->UiCatalogSha256() != c.catalog->Sha256())
        return Fail("TRIGGER_REPLACE_SOURCE_UNAVAILABLE", "缺少当前文档、来源密封或同目录的已准备类型系统。");
    return Status::Ok();
}
bool WriteLimits(Writer& w, const TriggerReplaceLimits& l) {
    return w.Number(l.pageLimit) && w.Number(l.maxGraphRecords) && w.Number(l.maxGraphEdges)
        && w.Number(l.maxScanTextBytes) && w.Number(l.maxResultBytes) && w.Number(l.maxSelection)
        && w.Number(l.maxTargetRecords) && w.Number(l.maxTargetTextBytes) && w.Number(l.maxNameRecords)
        && w.Number(l.maxNameTextBytes) && w.Number(l.maxPreviewDocumentBytes) && w.Number(l.maxPreviewWorkUnits) && w.Number(static_cast<unsigned>(l.scalarWorkPolicy));
}
Result<std::string> RequestHash(const TriggerReplaceQuery& q, const TriggerReplaceLimits& l) {
    Writer w(16384); w.Text("trigger-replace-query-v1"); WriteLimits(w, l);
    w.Number(static_cast<unsigned>(q.mode)); w.Number(static_cast<unsigned>(q.match)); w.Text(q.needle); w.Number(q.caseSensitive);
    w.Number(q.scopeTriggerId.has_value()); if (q.scopeTriggerId) w.Number(*q.scopeTriggerId);
    w.Number(q.sourceVariableId.has_value()); if (q.sourceVariableId) w.Number(*q.sourceVariableId);
    w.Number(q.sourceObject.has_value()); if (q.sourceObject) { w.Number(static_cast<unsigned>(q.sourceObject->type)); w.Text(q.sourceObject->rawcode); }
    return w.Hash();
}
Status Query(const TriggerReplaceQuery& q) {
    if (static_cast<unsigned>(q.mode) > 3 || static_cast<unsigned>(q.match) > 1 || q.needle.size() > kText || !Utf8(q.needle)
        || (q.scopeTriggerId && !*q.scopeTriggerId) || (q.sourceVariableId && (!*q.sourceVariableId || q.mode != TriggerReplaceMode::Variable))
        || (q.sourceObject && (q.mode != TriggerReplaceMode::ObjectReference
            || static_cast<unsigned>(q.sourceObject->type) > static_cast<unsigned>(ObjectData::ObjectType::Destructable)
            || !Trigger::TriggerTypeSystem::IsObjectCodeLiteral(q.sourceObject->rawcode)))
        || (q.needle.empty() && !q.sourceVariableId && !q.sourceObject))
        return Fail("TRIGGER_REPLACE_QUERY_INVALID", "查询需要有界 UTF8 文本或正确模式的真实身份。");
    return Status::Ok();
}
TriggerReplaceQueryProof Proof(const TriggerReplaceContext& c, std::string request, std::string snapshot) {
    return {c.revision, std::move(request), std::move(snapshot), std::string(c.document->SourceWtgSha256()),
        std::string(c.document->SourceWctSha256()), c.catalog->Sha256()};
}
std::string TypeName(ObjectData::ObjectType type) {
    using T = ObjectData::ObjectType;
    switch (type) { case T::Unit: return "unitcode"; case T::Item: return "itemcode"; case T::Ability: return "abilcode";
    case T::Buff: return "buffcode"; case T::Upgrade: return "techcode"; case T::Doodad: return "doodadcode"; case T::Destructable: return "destructablecode"; }
    return {};
}
// 分类身份只沿实际 Catalog 父链取得；可接受性/字面量合法性仍由原 TypeSystem 决定。
std::optional<ObjectData::ObjectType> ObjectType(const Trigger::TriggerTypeSystem& types, std::string_view type) {
    using T = ObjectData::ObjectType; std::set<std::string_view> seen;
    for (unsigned depth = 0; depth < 32 && seen.insert(type).second; ++depth) {
        if (type == "unitcode") return T::Unit; if (type == "itemcode") return T::Item;
        if (type == "abilcode" || type == "heroskillcode") return T::Ability;
        if (type == "buffcode") return T::Buff; if (type == "techcode") return T::Upgrade;
        if (type == "doodadcode") return T::Doodad; if (type == "destructablecode") return T::Destructable;
        const auto* info = types.Find(type); if (!info || info->baseType.empty() || info->baseType == type) break; type = info->baseType;
    }
    return std::nullopt;
}
struct Mount { enum class Kind { None, Trigger, Eca, Argument }; Kind kind = Kind::None; std::size_t index = 0, ordinal = 0; };
struct Scan {
    TriggerReplacePage page;
    std::vector<TriggerReplaceHit> selected;
    std::size_t selectedWorkUnits = 0;
    bool selectedWorkOverflow = false;
};
class Scanner {
public:
    Scanner(const TriggerReplaceContext& c, const TriggerReplaceQuery& q, const TriggerReplaceLimits& l,
        std::size_t offset, std::span<const std::string> selected)
        : c_(c), d_(*c.document), q_(q), l_(l), matcher_(q.needle, q.caseSensitive, q.match), writer_(l.maxScanTextBytes), offset_(offset), selected_(selected.begin(), selected.end()) {}
    Result<Scan> Run() {
        if (const auto status = Prepare(); !status.Succeeded()) return Failed<Scan>(status);
        if (!out_.page.coverage.scanTruncated) {
            for (std::size_t ti = 0; ti < d_.triggers.size(); ++ti) {
                const auto& trigger = d_.triggers[ti];
                if (q_.scopeTriggerId && trigger.id != *q_.scopeTriggerId) continue;
                ++out_.page.coverage.triggersExamined;
                if (trigger.type != 0 || trigger.customTextFlag != 0) { ++out_.page.coverage.customCodeSlotsExcluded; continue; }
                std::vector<std::pair<bool, std::size_t>> stack;
                std::size_t triggerVisits = 0;
                for (auto it = trigger.ecas.rbegin(); it != trigger.ecas.rend(); ++it) stack.emplace_back(true, *it);
                while (!stack.empty()) {
                    const auto [eca, index] = stack.back(); stack.pop_back();
                    ++triggerVisits;
                    if (eca) {
                        const auto& node = d_.ecaNodes[index];
                        if (q_.mode == TriggerReplaceMode::Function && matcher_.Matches(node.name.text))
                            Emit(ti, index, std::nullopt, node.name.text, node.name.text, {}, "TRIGGER_REPLACE_FUNCTION_READ_ONLY", "函数只查找和定位，不改写调用或代码。");
                        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) stack.emplace_back(true, *it);
                        for (auto it = node.arguments.rbegin(); it != node.arguments.rend(); ++it) stack.emplace_back(false, *it);
                    } else {
                        const auto& argument = d_.argumentNodes[index];
                        if (argument.indexArgument) stack.emplace_back(false, *argument.indexArgument);
                        if (argument.callEca) stack.emplace_back(true, *argument.callEca);
                        if (q_.mode != TriggerReplaceMode::Function) Visit(ti, index);
                    }
                }
                if (selectedTriggers_.contains(trigger.id)) {
                    // CollectLocals 每个选中触发器只做一次，但 visited 初始化仍覆盖全部 ECA。
                    if (!Add(out_.selectedWorkUnits, d_.ecaNodes.size(), KnownWorkCap(l_))
                        || !Add(out_.selectedWorkUnits, triggerVisits * 4, KnownWorkCap(l_))) out_.selectedWorkOverflow = true;
                }
            }
        }
        auto& coverage = out_.page.coverage;
        coverage.state = coverage.scanTruncated || coverage.displayResolutionPartial || coverage.unresolvedReferenceRows
            ? TriggerReplaceState::Partial : TriggerReplaceState::Ready;
        if (coverage.state == TriggerReplaceState::Ready) coverage.totalMatches = coverage.matchedInScannedScope;
        writer_.Number(coverage.scanTruncated); writer_.Number(coverage.displayResolutionPartial);
        writer_.Number(coverage.unresolvedReferenceRows); writer_.Number(coverage.matchedInScannedScope);
        const auto snapshot = writer_.Hash(), request = RequestHash(q_, l_);
        if (!snapshot.Succeeded()) return Failed<Scan>(snapshot); if (!request.Succeeded()) return Failed<Scan>(request);
        out_.page.proof = Proof(c_, *request.value, *snapshot.value); out_.page.matchOffset = offset_;
        if (offset_ > coverage.matchedInScannedScope || (offset_ == coverage.matchedInScannedScope && offset_ != 0))
            return Failed<Scan>(Fail("TRIGGER_REPLACE_CURSOR_INVALID", "分页偏移超出同一查询的已扫描命中范围。"));
        const auto end = offset_ + out_.page.rows.size();
        if (end < coverage.matchedInScannedScope) {
            if (out_.page.rows.empty()) return Failed<Scan>(Fail("TRIGGER_REPLACE_RESULT_BUDGET", "单条命中无法放入结果预算；请提高页文本预算或缩小查询。"));
            out_.page.next = TriggerReplaceCursor{out_.page.proof, end};
        }
        if (writer_.Used() > l_.maxScanTextBytes || resolutionBytes_ > l_.maxScanTextBytes - writer_.Used())
            return Failed<Scan>(Fail("TRIGGER_REPLACE_TEXT_BUDGET", "名称解析与来源摘要合计超过查询文本工作预算。"));
        coverage.textBytesCharged = writer_.Used() + resolutionBytes_;
        return Result<Scan>::Success(std::move(out_));
    }
private:
    bool Text(std::string_view value) {
        if (writer_.Used() + resolutionBytes_ > l_.maxScanTextBytes || value.size() > l_.maxScanTextBytes - writer_.Used() - resolutionBytes_) return false;
        return Utf8(value) && writer_.Text(value);
    }
    bool Raw(const Trigger::RawText& value) {
        if (!Text(value.text) || !writer_.Number(static_cast<unsigned>(value.encoding))) return false;
        if (writer_.Used() > l_.maxScanTextBytes || resolutionBytes_ > l_.maxScanTextBytes - writer_.Used()
            || value.originalBytes.size() > l_.maxScanTextBytes - writer_.Used() - resolutionBytes_) return false;
        return writer_.Text(value.originalBytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(value.originalBytes.data()), value.originalBytes.size()));
    }
    bool ChargeResolution(std::size_t bytes) {
        if (writer_.Used() > l_.maxScanTextBytes || resolutionBytes_ > l_.maxScanTextBytes - writer_.Used()
            || bytes > l_.maxScanTextBytes - writer_.Used() - resolutionBytes_) return false;
        resolutionBytes_ += bytes; return true;
    }
    void Partial() { out_.page.coverage.displayResolutionPartial = true; }
    Status Prepare() {
        auto& coverage = out_.page.coverage;
        for (auto size : {d_.categories.size(), d_.variables.size(), d_.triggers.size(), d_.ecaNodes.size(), d_.argumentNodes.size()})
            if (!Add(coverage.graphRecordsCharged, size, l_.maxGraphRecords)) { coverage.scanTruncated = true; return Status::Ok(); }
        writer_.Text("trigger-replace-snapshot-v1"); Text(c_.revision); Text(c_.catalog->Sha256());
        // 五域收费通过后建立局部 arena；不改变身份、顺序、摘要或资源上限。
        alignas(std::max_align_t) std::array<std::byte, 16 * 1024> identityStorage;
        std::pmr::monotonic_buffer_resource identityResource(
            identityStorage.data(), identityStorage.size(), std::pmr::new_delete_resource());
        std::pmr::unordered_set<StableId> ids{std::pmr::polymorphic_allocator<StableId>{&identityResource}};
        // 五个身份域的记录合计已通过上限检查；这里只预留容量，不承诺实际内存占用。
        ids.reserve(coverage.graphRecordsCharged);
        const auto identity = [&](StableId id) { return id && ids.insert(id).second && writer_.Number(id); };
        const auto edges = [&](std::span<const std::uint32_t> values, std::size_t size) {
            if (!Add(coverage.graphEdgesCharged, values.size(), l_.maxGraphEdges)) return false;
            for (auto index : values) if (index >= size || !writer_.Number(index)) return false;
            return true;
        };
        for (const auto& category : d_.categories) if (!identity(category.id) || !Raw(category.name)) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "分类身份或文本无效/超预算。");
        for (const auto& variable : d_.variables) {
            if (!identity(variable.id) || !Raw(variable.name) || !Raw(variable.typeName) || !Raw(variable.value)
                || !writer_.Number(variable.isArray) || !writer_.Number(variable.size) || !writer_.Number(variable.hasDefault)) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "变量身份或文本无效/超预算。");
            resolverDomainBytes_ += variable.name.text.size() + variable.typeName.text.size() + 8;
        }
        bool scopeFound = !q_.scopeTriggerId;
        ecas_.resize(d_.ecaNodes.size()); arguments_.resize(d_.argumentNodes.size());
        const auto mount = [](Mount& destination, Mount value) { if (destination.kind != Mount::Kind::None) return false; destination = value; return true; };
        for (std::size_t ti = 0; ti < d_.triggers.size(); ++ti) {
            const auto& trigger = d_.triggers[ti];
            if (!identity(trigger.id) || !Raw(trigger.name) || !writer_.Number(trigger.type) || !writer_.Number(trigger.customTextFlag)
                || !writer_.Number(trigger.enabled) || !edges(trigger.ecas, d_.ecaNodes.size())) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "触发器身份、边或文本无效/超预算。");
            resolverDomainBytes_ += trigger.name.text.size() + 16;
            if (q_.scopeTriggerId == trigger.id) scopeFound = true;
            for (auto index : trigger.ecas) if (!mount(ecas_[index], {Mount::Kind::Trigger, ti, 0})) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "ECA 挂载不唯一。");
        }
        if (!scopeFound) return Fail("TRIGGER_REPLACE_SCOPE_MISSING", "当前触发器范围的稳定身份不存在。");
        for (std::size_t ei = 0; ei < d_.ecaNodes.size(); ++ei) {
            const auto& node = d_.ecaNodes[ei];
            if (!identity(node.id) || !Raw(node.name) || !Text(node.uiSignature) || !Text(node.uiLayer)
                || !writer_.Number(node.sourceTriggerId) || !writer_.Number(node.type) || !writer_.Number(node.enabled)
                || !writer_.Number(node.childId.has_value()) || (node.childId && !writer_.Number(*node.childId))
                || !edges(node.arguments, d_.argumentNodes.size()) || !edges(node.children, d_.ecaNodes.size()))
                return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "ECA 身份、边或文本无效/超预算。");
            for (std::size_t ai = 0; ai < node.arguments.size(); ++ai)
                if (!mount(arguments_[node.arguments[ai]], {Mount::Kind::Eca, ei, ai})) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "参数挂载不唯一。");
            for (auto index : node.children) if (!mount(ecas_[index], {Mount::Kind::Eca, ei, 0})) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "子 ECA 挂载不唯一。");
        }
        for (std::size_t ai = 0; ai < d_.argumentNodes.size(); ++ai) {
            const auto& node = d_.argumentNodes[ai];
            if (!identity(node.id) || !Raw(node.value) || !writer_.Number(node.type) || !writer_.Number(node.insertCall)
                || !writer_.Number(node.insertIndex) || !writer_.Number(node.callEca.has_value()) || !writer_.Number(node.indexArgument.has_value()))
                return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "参数身份或文本无效/超预算。");
            if (node.callEca && (*node.callEca >= ecas_.size() || !Add(coverage.graphEdgesCharged, 1, l_.maxGraphEdges)
                || !writer_.Number(*node.callEca) || !mount(ecas_[*node.callEca], {Mount::Kind::Argument, ai, 0}))) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "函数参数边越界、共享或超预算。");
            if (node.indexArgument && (*node.indexArgument >= arguments_.size() || !Add(coverage.graphEdgesCharged, 1, l_.maxGraphEdges)
                || !writer_.Number(*node.indexArgument) || !mount(arguments_[*node.indexArgument], {Mount::Kind::Argument, ai, 0}))) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "数组下标边越界、共享或超预算。");
        }
        // 唯一挂载不足以证明无循环；线性颜色遍历验证每个拥有链的最终触发器。
        std::vector<unsigned char> ecaColor(ecas_.size()), argumentColor(arguments_.size());
        std::vector<StableId> ecaOwner(ecas_.size()), argumentOwner(arguments_.size());
        const auto owner = [&](bool isEca, std::size_t index) -> std::optional<StableId> {
            if ((isEca ? ecaColor[index] : argumentColor[index]) == 2) return isEca ? ecaOwner[index] : argumentOwner[index];
            const auto direct = isEca ? ecas_[index] : arguments_[index];
            StableId immediate = 0;
            if (direct.kind == Mount::Kind::Trigger) immediate = d_.triggers[direct.index].id;
            else if (direct.kind == Mount::Kind::Eca && ecaColor[direct.index] == 2) immediate = ecaOwner[direct.index];
            if (immediate) {
                if (isEca && d_.ecaNodes[index].sourceTriggerId != immediate) return std::nullopt;
                (isEca ? ecaColor[index] : argumentColor[index]) = 2;
                (isEca ? ecaOwner[index] : argumentOwner[index]) = immediate; return immediate;
            }
            std::vector<std::pair<bool, std::size_t>> path;
            StableId result = 0;
            while (true) {
                auto& color = isEca ? ecaColor[index] : argumentColor[index];
                if (color == 1) return std::nullopt;
                if (color == 2) { result = isEca ? ecaOwner[index] : argumentOwner[index]; break; }
                color = 1; path.emplace_back(isEca, index);
                const auto current = isEca ? ecas_[index] : arguments_[index];
                if (current.kind == Mount::Kind::Trigger) { result = d_.triggers[current.index].id; break; }
                if (current.kind == Mount::Kind::None) return std::nullopt;
                isEca = current.kind == Mount::Kind::Eca; index = current.index;
                if (path.size() > 256) return std::nullopt;
            }
            for (const auto& [eca, item] : path) {
                (eca ? ecaColor[item] : argumentColor[item]) = 2;
                (eca ? ecaOwner[item] : argumentOwner[item]) = result;
                if (eca && d_.ecaNodes[item].sourceTriggerId != result) return std::nullopt;
            }
            return result;
        };
        for (std::size_t i = 0; i < ecas_.size(); ++i) if (!owner(true, i)) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "ECA 所有权不完整、循环或跨触发器。");
        for (std::size_t i = 0; i < arguments_.size(); ++i) if (!owner(false, i)) return Fail("TRIGGER_REPLACE_GRAPH_INVALID", "参数所有权不完整或循环。");
        if (q_.sourceVariableId) {
            const auto found = std::find_if(d_.variables.begin(), d_.variables.end(), [&](const auto& v) { return v.id == *q_.sourceVariableId; });
            if (found == d_.variables.end()) return Fail("TRIGGER_REPLACE_VARIABLE_MISSING", "所选来源变量的稳定声明不存在。"); sourceVariable_ = &*found;
        }
        std::size_t catalogBytes = 0, catalogDefinitions = 0;
        if (c_.catalog->Manifest().size() > 1024) return Fail("TRIGGER_REPLACE_CATALOG_BUDGET", "目录来源项超过 1024 项预算。");
        for (const auto& source : c_.catalog->Manifest()) {
            if (source.size > 16 * 1024 * 1024 || !Add(catalogBytes, static_cast<std::size_t>(source.size), 16 * 1024 * 1024)
                || !Add(catalogDefinitions, source.definitionCount, 32768)) return Fail("TRIGGER_REPLACE_CATALOG_BUDGET", "类型目录来源/定义超过准备预算。");
        }
        if (q_.mode == TriggerReplaceMode::ObjectReference) {
            // 只读取一次已持有来源的状态元数据，不扫描/复制物编正文。未知域仍可查原值但不授唯一替换。
            if (c_.objects.size() > 14) return Fail("TRIGGER_REPLACE_OBJECT_SOURCE_BUDGET", "物编来源状态超过14项；没有猜测完整域。");
            std::array<unsigned, 7> counts{};
            for (const auto& source : c_.objects) {
                const auto type = static_cast<unsigned>(source.type);
                if (type >= counts.size()) return Fail("TRIGGER_REPLACE_OBJECT_SOURCE_INVALID", "物编来源类型无效。");
                ++counts[type];
                literalSourcesReady_[type] = source.state == ProjectSearchState::Ready
                    || (source.state == ProjectSearchState::Missing && source.records.empty() && c_.standardObjectsReady && c_.standardObjects);
                writer_.Number(type); writer_.Number(static_cast<unsigned>(source.state)); writer_.Number(static_cast<unsigned>(source.origin)); writer_.Number(source.records.size());
            }
            for (std::size_t type = 0; type != counts.size(); ++type) literalSourcesReady_[type] = literalSourcesReady_[type] && counts[type] == 1;
            writer_.Number(c_.standardObjectsReady && c_.standardObjects);
        }
        PrepareStrings();
        if (c_.mapObjects) Text(c_.mapObjects->Identity());
        return Status::Ok();
    }
    void PrepareStrings() {
        auto& coverage = out_.page.coverage;
        if (!c_.strings) return;
        if (c_.strings->entries.size() > l_.maxNameRecords) { Partial(); return; }
        std::size_t bytes = 0;
        for (const auto& entry : c_.strings->entries) if (!Add(bytes, entry.value.size() + sizeof(entry.id), l_.maxNameTextBytes)) { Partial(); return; }
        coverage.nameRecordsCharged = c_.strings->entries.size(); coverage.nameTextBytesCharged = bytes;
        for (const auto& entry : c_.strings->entries) {
            // WTS parser 保留旧本地编码原字节；编码展示必须交给共享 N1 DecodeSourceText。
            if (entry.value.find('\0') != std::string::npos || !strings_.emplace(entry.id, &entry).second) { strings_.clear(); Partial(); return; }
            // 完整 WTS 摘要包含非命中和晚出现的重复 ID；不把一个局部表当完整来源。
            if (!writer_.Number(entry.id) || !writer_.Text(entry.value)) { strings_.clear(); Partial(); return; }
        }
        stringsReady_ = true;
    }
    std::string_view Display(std::string_view raw) {
        const auto id = StringReferenceId(raw); if (!id) return raw;
        if (!stringsReady_) { Partial(); return raw; }
        auto cached = display_.find(*id); if (cached != display_.end()) return cached->second;
        const auto found = strings_.find(*id);
        if (found == strings_.end()) { Partial(); return raw; }
        // N1 对可信空条目保留本次 raw；不同 leading-zero 别名不能共享该回退值。
        if (found->second->value.empty()) return raw;
        // 完整域已经证明 ID 唯一；共享 N1 只在可信单项视图运行，避免每个引用重扫全 WTS。
        if (!ChargeResolution(found->second->value.size() * 6 + raw.size())) { Partial(); return raw; }
        MapData::StringTableDocument one; one.entries.push_back(*found->second);
        NameResolutionContext names; names.strings = &one;
        const auto resolved = ResolveName(names, raw, "string");
        if ((resolved.state != NameResolutionState::Resolved && resolved.state != NameResolutionState::Literal) || !Utf8(resolved.displayName)) { Partial(); return raw; }
        return display_.emplace(*id, resolved.displayName).first->second;
    }
    bool VariableMatches(const Trigger::ArgumentNode& argument) {
        if (argument.type != 1) return false;
        if (sourceVariable_ && argument.value.text != sourceVariable_->name.text
            && argument.value.text != "udg_" + sourceVariable_->name.text) return false;
        if (!sourceVariable_ && !matcher_.Matches(argument.value.text)) return false;
        auto found = references_.find(argument.value.text);
        if (found == references_.end()) {
            if (!ChargeResolution(resolverDomainBytes_)) { Partial(); ++out_.page.coverage.unresolvedReferenceRows; return false; }
            const auto resolved = Trigger::ResolveVariableReference(d_, argument.value.text, c_.mapObjects);
            const bool declared = resolved.Succeeded() && resolved.value->kind == Trigger::VariableReferenceKind::Declared;
            found = references_.emplace(argument.value.text, declared).first;
        }
        if (!found->second) { ++out_.page.coverage.unresolvedReferenceRows; return false; }
        return true;
    }
    void Visit(std::size_t ti, std::size_t ai) {
        const auto& argument = d_.argumentNodes[ai]; const auto mount = arguments_[ai];
        // 下标可以查到原值，但没有直接 ECA 参数身份，永远不授予替换资格。
        std::size_t ei = mount.index;
        if (mount.kind != Mount::Kind::Eca) {
            auto cursor = mount;
            while (cursor.kind == Mount::Kind::Argument) cursor = arguments_[cursor.index];
            if (cursor.kind != Mount::Kind::Eca) return; ei = cursor.index;
        }
        const auto& eca = d_.ecaNodes[ei]; const Trigger::UiDefinition* definition = nullptr;
        std::string declared;
        const auto prepareType = [&] {
            definition = c_.catalog->Find(eca.type, eca.name.text);
            declared = definition && mount.kind == Mount::Kind::Eca ? Trigger::TriggerTypeSystem::DeclaredType(*definition, mount.ordinal) : "integer";
        };
        // 对象域的匹配依赖声明类型；参数/变量模式只为命中行准备展示及替换资格。
        if (q_.mode == TriggerReplaceMode::ObjectReference) prepareType();
        std::string_view display = argument.value.text; bool matched = false, literalSourceUntrusted = false;
        if (q_.mode == TriggerReplaceMode::ParameterValue) {
            display = Display(argument.value.text); matched = matcher_.Matches(argument.value.text);
            if (const auto id = StringReferenceId(argument.value.text)) {
                auto cached = displayMatches_.find(*id);
                // 只有真实解析后的展示文字才可按 StringId 共享；TRIGSTR_01/1 的 raw fallback 各不相同。
                if (display_.contains(*id)) {
                    if (cached == displayMatches_.end()) cached = displayMatches_.emplace(*id, matcher_.Matches(display)).first;
                    matched = matched || cached->second;
                }
            }
        }
        else if (q_.mode == TriggerReplaceMode::Variable) matched = VariableMatches(argument);
        else if (q_.mode == TriggerReplaceMode::ObjectReference) {
            if (argument.type == 1) {
                const auto* object = Preplaced(argument.value.text);
                if (!object) return;
                const auto type = object->type == "unit" ? std::optional{ObjectData::ObjectType::Unit}
                    : object->type == "item" ? std::optional{ObjectData::ObjectType::Item} : std::nullopt;
                if (!type) { ++out_.page.coverage.unresolvedReferenceRows; return; }
                matched = q_.sourceObject ? *type == q_.sourceObject->type && object->rawcode == q_.sourceObject->rawcode
                    : matcher_.Matches(argument.value.text) || matcher_.Matches(object->rawcode);
            } else {
            const auto type = ObjectType(*c_.types, declared);
            if (declared == "Null" || declared.empty()) { if (argument.type == 3 && Trigger::TriggerTypeSystem::IsObjectCodeLiteral(argument.value.text)) ++out_.page.coverage.unresolvedReferenceRows; return; }
            if (argument.type != 3 || !type || !Trigger::TriggerTypeSystem::IsObjectCodeLiteral(argument.value.text)) return;
            literalSourceUntrusted = !literalSourcesReady_[static_cast<unsigned>(*type)];
            if (literalSourceUntrusted) ++out_.page.coverage.unresolvedReferenceRows;
            if (q_.sourceObject) matched = *type == q_.sourceObject->type && argument.value.text == q_.sourceObject->rawcode;
            else matched = matcher_.Matches(argument.value.text);
            }
        }
        if (!matched) return;
        if (q_.mode != TriggerReplaceMode::ObjectReference) prepareType();
        std::string code, reason;
        if (mount.kind != Mount::Kind::Eca || argument.callEca || argument.indexArgument || argument.insertCall || argument.insertIndex) {
            code = "TRIGGER_REPLACE_COMPLEX_READ_ONLY"; reason = "函数、数组根和下标只查找，不进行标量替换。";
        } else {
            auto signature = signatures_.find(definition);
            if (signature == signatures_.end()) signature = signatures_.emplace(definition, definition ? definition->Signature() : std::string{}).first;
            if (!definition || signature->second != eca.uiSignature) { code = "TRIGGER_REPLACE_SIGNATURE_STALE"; reason = "当前函数目录签名不可确认。"; }
        }
        if (argument.value.text.size() > kText) { code = "TRIGGER_REPLACE_VALUE_TOO_LARGE"; reason = "原值超过单项 2048 字节；只显示有界摘要，不授予替换。"; }
        if (declared == "AnyGlobal" || declared == "AnyType" || declared == "anytype" || declared == "AnyReturnType" || declared == "code"
            || c_.types->LiteralKind(declared) == Trigger::TriggerTypeSystem::Literal::Script) {
            code = "TRIGGER_REPLACE_TYPE_READ_ONLY"; reason = "任意类型目标、可写左值或代码槽只查找；本轮不猜测替换语义。";
        }
        if (literalSourceUntrusted) { code = "TRIGGER_REPLACE_OBJECT_SOURCE_UNKNOWN"; reason = "物编来源不完整或未知；原值可查但不能确认对象域唯一性，保留只读。"; }
        if (declared.size() > kText) { code = "TRIGGER_REPLACE_TYPE_TOO_LARGE"; reason = "参数类型名超过单项 2048 字节；只展示有界摘要，不授予替换。"; declared = Short(declared); }
        Emit(ti, ei, ai, argument.value.text, display, std::move(declared), std::move(code), std::move(reason));
    }
    void Emit(std::size_t ti, std::size_t ei, std::optional<std::size_t> ai, std::string_view raw, std::string_view display,
        std::string declared, std::string code, std::string reason) {
        const auto match = out_.page.coverage.matchedInScannedScope++;
        const auto key = ai ? "argument/" + std::to_string(d_.argumentNodes[*ai].id) : "eca/" + std::to_string(d_.ecaNodes[ei].id);
        const bool selected = selected_.contains(key);
        if (!selected && (match < offset_ || out_.page.rows.size() >= l_.pageLimit || pageFull_)) return;
        TriggerReplaceHit hit; hit.rowKey = key; hit.triggerId = d_.triggers[ti].id; hit.ecaId = d_.ecaNodes[ei].id;
        if (ai) { hit.argumentId = d_.argumentNodes[*ai].id; hit.oldKind = d_.argumentNodes[*ai].type; }
        hit.triggerName = Short(d_.triggers[ti].name.text); hit.functionName = Short(d_.ecaNodes[ei].name.text);
        hit.oldValue = Short(raw); hit.displayValue = Short(display); hit.declaredType = std::move(declared);
        hit.eligibility = code.empty() && ai ? TriggerReplaceEligibility::PreviewRequired : TriggerReplaceEligibility::ReadOnly;
        hit.reasonCode = std::move(code); hit.reason = std::move(reason);
        if (selected) {
            out_.selected.push_back(hit);
            if (ai && hit.eligibility == TriggerReplaceEligibility::PreviewRequired) BillSelected(ti, *ai);
        }
        const auto bytes = hit.rowKey.size() + hit.triggerName.size() + hit.functionName.size() + hit.oldValue.size()
            + hit.displayValue.size() + hit.declaredType.size() + hit.reasonCode.size() + hit.reason.size() + 128;
        if (match >= offset_ && out_.page.rows.size() < l_.pageLimit && !pageFull_) {
            if (Add(resultBytes_, bytes, l_.maxResultBytes)) out_.page.rows.push_back(std::move(hit)); else pageFull_ = true;
        }
    }
    void BillSelected(std::size_t ti, std::size_t ai) {
        if (out_.selectedWorkOverflow) return;
        selectedTriggers_.insert(d_.triggers[ti].id);
        const auto domain = d_.variables.size() + d_.triggers.size();
        const auto charge = [&](std::size_t units) { if (!Add(out_.selectedWorkUnits, units, KnownWorkCap(l_))) out_.selectedWorkOverflow = true; };
        // GraphSet 的真实 Find 是线性扫描到这个当前索引；不能把批量 Guard 的索引优化冒为 Apply 也 O(1)。
        charge(ai + 1); charge(domain); charge(domain); // 旧标量/新变量资格；字面量目标取更保守一遍。
        const auto slot = [&](const Mount& owner, std::size_t passes) {
            if (out_.selectedWorkOverflow) return;
            const auto& node = d_.ecaNodes[owner.index]; const auto* definition = c_.catalog->Find(node.type, node.name.text);
            if (!definition) { out_.selectedWorkOverflow = true; return; }
            // Signature/DeclaredType 按这个真实定义计费；这里仅核工作量，不判新的类型语义。
            std::size_t definitionBytes = definition->name.size() + definition->group.size();
            for (const auto& field : definition->Active().fields) if (!Add(definitionBytes, field.first.size() + field.second.size(), KnownWorkCap(l_))) { out_.selectedWorkOverflow = true; return; }
            for (const auto& argument : definition->Active().arguments) {
                if (!Add(definitionBytes, argument.type.size() + argument.defaultValue.size(), KnownWorkCap(l_))) { out_.selectedWorkOverflow = true; return; }
                for (const auto& field : argument.fields) if (!Add(definitionBytes, field.first.size() + field.second.size(), KnownWorkCap(l_))) { out_.selectedWorkOverflow = true; return; }
            }
            charge(definitionBytes); charge((owner.ordinal + 1) * passes); charge(32 * passes);
            if (Trigger::TriggerTypeSystem::DeclaredType(*definition, owner.ordinal) != "Null") return;
            if (owner.ordinal > 128) { out_.selectedWorkOverflow = true; return; }
            // ExpectedType 的真实 Null 前缀最长为当前 ordinal；只计其中可能调用 ValueType 的变量。
            // 选择器会提前结束，保留整前缀是有界保守账，不更改原解析规则。
            charge((owner.ordinal + 1) * (owner.ordinal + 1) * passes);
            for (std::size_t previous = 0; previous < owner.ordinal && previous < node.arguments.size(); ++previous)
                if (d_.argumentNodes[node.arguments[previous]].type == 1 || selected_.contains("argument/" + std::to_string(d_.argumentNodes[node.arguments[previous]].id)))
                    for (std::size_t pass = 0; pass < passes; ++pass) charge(domain);
        };
        const auto direct = arguments_[ai]; slot(direct, 2); // Guard 与 GraphSet 后检查。
        if (out_.selectedWorkOverflow) return;
        auto cursor = ecas_[direct.index];
        for (std::size_t depth = 0; depth <= 256; ++depth) {
            if (cursor.kind == Mount::Kind::Trigger) return;
            if (cursor.kind == Mount::Kind::Eca) cursor = ecas_[cursor.index];
            else if (cursor.kind == Mount::Kind::Argument) {
                cursor = arguments_[cursor.index]; if (cursor.kind == Mount::Kind::Eca) slot(cursor, 1);
            } else { out_.selectedWorkOverflow = true; return; }
        }
        out_.selectedWorkOverflow = true;
    }
    const Trigger::MapObjectReference* Preplaced(std::string_view value) {
        auto cached = preplaced_.find(std::string(value));
        if (cached == preplaced_.end()) {
            PreplacedLookup found;
            if (!ChargeResolution(resolverDomainBytes_)) found.unresolved = true;
            else {
                const auto reference = Trigger::ResolveVariableReference(d_, value, c_.mapObjects);
                if (!reference.Succeeded()) found.unresolved = true;
                else if (reference.value->kind == Trigger::VariableReferenceKind::PreplacedObject) {
                    found.object = c_.mapObjects ? c_.mapObjects->Find(value) : nullptr;
                    found.unresolved = !found.object || reference.value->dependencySha256 != c_.mapObjects->Identity();
                }
            }
            cached = preplaced_.emplace(std::string(value), found).first;
        }
        if (cached->second.unresolved) ++out_.page.coverage.unresolvedReferenceRows;
        return cached->second.object;
    }
    const TriggerReplaceContext& c_; const Trigger::TriggerDocument& d_; const TriggerReplaceQuery& q_; const TriggerReplaceLimits& l_;
    LinearMatcher matcher_; Writer writer_; std::size_t offset_, resultBytes_ = 0, resolutionBytes_ = 0, resolverDomainBytes_ = 0;
    std::unordered_set<std::string> selected_; Scan out_; std::vector<Mount> ecas_, arguments_;
    const Trigger::VariableRecord* sourceVariable_ = nullptr;
    std::map<std::uint32_t, const MapData::StringTableEntry*> strings_; bool stringsReady_ = false;
    std::map<std::uint32_t, std::string> display_; std::map<std::string, bool> references_;
    std::map<std::uint32_t, bool> displayMatches_;
    std::unordered_map<const Trigger::UiDefinition*, std::string> signatures_; bool pageFull_ = false;
    std::set<StableId> selectedTriggers_;
    struct PreplacedLookup { const Trigger::MapObjectReference* object = nullptr; bool unresolved = false; };
    std::map<std::string, PreplacedLookup> preplaced_;
    std::array<bool, 7> literalSourcesReady_{};
};
} // namespace

Result<TriggerReplacePage> QueryTriggerReplace(const TriggerReplaceContext& context, std::string_view expectedRevision,
    const TriggerReplaceQuery& query, const TriggerReplaceLimits& limits, const TriggerReplaceCursor* cursor) {
    if (const auto status = Context(context, expectedRevision); !status.Succeeded()) return Failed<TriggerReplacePage>(status);
    if (const auto status = Limits(limits); !status.Succeeded()) return Failed<TriggerReplacePage>(status);
    if (const auto status = Query(query); !status.Succeeded()) return Failed<TriggerReplacePage>(status);
    try {
        auto scan = Scanner(context, query, limits, cursor ? cursor->nextMatchOffset : 0, {}).Run();
        if (!scan.Succeeded()) return Failed<TriggerReplacePage>(scan);
        if (cursor && cursor->proof != scan.value->page.proof) return Failed<TriggerReplacePage>(Fail("TRIGGER_REPLACE_CURSOR_STALE", "查询、预算或当前来源已变化；旧分页无效。"));
        return Result<TriggerReplacePage>::Success(std::move(scan.value->page));
    } catch (const std::bad_alloc&) { return Failed<TriggerReplacePage>(Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "有界查询分配失败，未修改文档。")); }
}

namespace {
struct TargetScan { TriggerReplacementOptionsPage page; std::optional<TriggerReplacementOption> selected; };
class Targets {
public:
    Targets(const TriggerReplaceContext& c, TriggerReplacementKind kind, std::string_view filter,
        const TriggerReplaceLimits& limits, std::size_t offset, const TriggerReplacement* selected)
        : c_(c), kind_(kind), filter_(filter), l_(limits), matcher_(filter, false, TriggerReplaceMatch::Substring), offset_(offset), selected_(selected), writer_(limits.maxTargetTextBytes) {}
    Result<TargetScan> Run() {
        if (static_cast<unsigned>(kind_) > 2 || filter_.size() > kText || !Utf8(filter_))
            return Failed<TargetScan>(Fail("TRIGGER_REPLACE_TARGET_QUERY_INVALID", "替换对象类型或筛选文字无效。"));
        if (kind_ == TriggerReplacementKind::Variable) {
            if (!Count(c_.document->variables.size()) || !Count(c_.document->triggers.size())) return Budget();
            // 全部名字先准入，随后才建立别名唯一域，晚出现的同名不能留下早期可选项。
            std::map<std::string, std::size_t> names;
            std::set<StableId> ids;
            for (const auto& variable : c_.document->variables) {
                if (!variable.id || !ids.insert(variable.id).second || !Text(variable.name.text) || !Text(variable.typeName.text)) return Invalid();
                writer_.Number(variable.id); writer_.Number(variable.isArray);
                ++names[variable.name.text]; ++names["udg_" + variable.name.text];
            }
            for (const auto& trigger : c_.document->triggers) {
                if (!Text(trigger.name.text)) return Invalid(); writer_.Number(trigger.id); writer_.Number(trigger.enabled); writer_.Number(trigger.type);
                if (trigger.type == 0 && trigger.enabled) ++names["gg_trg_" + Trigger::TriggerDirectoryEditor::ScriptName(trigger.name.text)];
            }
            for (const auto& variable : c_.document->variables) {
                if (variable.isArray != 0 || variable.name.text.empty() || names[variable.name.text] != 1 || names["udg_" + variable.name.text] != 1
                    || (c_.mapObjects && (c_.mapObjects->Find(variable.name.text) || c_.mapObjects->Find("udg_" + variable.name.text)))) { partial_ = true; continue; }
                TriggerReplacementOption row; row.rowKey = "variable/" + std::to_string(variable.id);
                row.display = variable.name.text; row.typeName = variable.typeName.text; row.source = "当前全局变量";
                row.replacement.variableId = variable.id; Emit(std::move(row));
            }
        } else if (kind_ == TriggerReplacementKind::PreplacedObject) {
            if (!c_.mapObjects) partial_ = true;
            else {
                if (!Count(c_.mapObjects->References().size())) return Budget();
                if (!Text(c_.mapObjects->Identity())) return Invalid();
                for (const auto& object : c_.mapObjects->References()) {
                    if (!Text(object.symbol) || !Text(object.type) || !Text(object.memberSha256) || !Text(object.rawcode)) return Invalid();
                    TriggerReplacementOption row; row.rowKey = "preplaced/" + object.symbol; row.display = object.symbol;
                    row.source = "已准备的真实预放对象"; row.typeName = object.type;
                    row.replacement.kind = kind_; row.replacement.preplacedSymbol = object.symbol;
                    row.replacement.mapObjectSnapshotIdentity = c_.mapObjects->Identity(); Emit(std::move(row));
                }
            }
        } else {
            if (c_.objects.size() > 14) return Budget();
            std::set<std::pair<unsigned, std::string>> customKeys;
            std::set<unsigned> customTypes;
            for (const auto& source : c_.objects) {
                if (!Count(source.records.size())) return Budget();
                writer_.Number(static_cast<unsigned>(source.type)); writer_.Number(static_cast<unsigned>(source.state)); writer_.Number(static_cast<unsigned>(source.origin));
                if (!customTypes.insert(static_cast<unsigned>(source.type)).second) return Invalid();
                if (source.state == ProjectSearchState::Missing && !source.records.empty()) return Invalid();
                if (source.state != ProjectSearchState::Ready && source.state != ProjectSearchState::Missing) {
                    partial_ = true; rejectedTypes_.insert(source.type); continue;
                }
                for (const auto& object : source.records) {
                    writer_.Number(object.stableId); writer_.Number(static_cast<unsigned>(object.key.type)); if (!Text(object.key.rawcode)) return Invalid();
                    const auto key = std::pair{static_cast<unsigned>(object.key.type), object.key.rawcode};
                    if (object.key.type != source.type || !customKeys.insert(key).second) return Invalid();
                    if (!Trigger::TriggerTypeSystem::IsObjectCodeLiteral(object.key.rawcode)) { partial_ = true; continue; }
                }
            }
            if (!c_.standardObjectsReady || !c_.standardObjects) partial_ = true;
            else if (!Count(c_.standardObjects->Entries().size())) return Budget();
            // 缺席的 overlay 不是明确 Missing。稀疏公开 span 只能证明已提供类型中的正向对象。
            for (unsigned type = 0; type <= static_cast<unsigned>(ObjectData::ObjectType::Destructable); ++type)
                if (!customTypes.contains(type)) { partial_ = true; rejectedTypes_.insert(static_cast<ObjectData::ObjectType>(type)); }
            for (const auto& source : c_.objects) {
                if (rejectedTypes_.contains(source.type)) continue;
                for (const auto& object : source.records) if (Trigger::TriggerTypeSystem::IsObjectCodeLiteral(object.key.rawcode))
                    Emit(Object(object.key, object.key.rawcode, "当前物编"));
            }
            if (c_.standardObjectsReady && c_.standardObjects) {
                for (const auto& object : c_.standardObjects->Entries()) {
                    writer_.Number(static_cast<unsigned>(object.key.type)); if (!Text(object.key.rawcode)) return Invalid();
                    const auto key = std::pair{static_cast<unsigned>(object.key.type), object.key.rawcode};
                    if (!Text(object.displayName)) return Invalid();
                    if (customKeys.contains(key) || rejectedTypes_.contains(object.key.type)) continue;
                    if (!Trigger::TriggerTypeSystem::IsObjectCodeLiteral(object.key.rawcode)) { partial_ = true; continue; }
                    Emit(Object(object.key, object.displayName.empty() ? object.key.rawcode : object.displayName, "已加载标准物编"));
                }
            }
        }
        Writer request(16384); request.Text("trigger-replace-options-v1"); WriteLimits(request, l_); request.Number(static_cast<unsigned>(kind_)); request.Text(filter_);
        if (c_.mapObjects) writer_.Text(c_.mapObjects->Identity());
        writer_.Number(partial_); const auto sha = writer_.Hash(), requestSha = request.Hash();
        if (!sha.Succeeded()) return Failed<TargetScan>(sha); if (!requestSha.Succeeded()) return Failed<TargetScan>(requestSha);
        out_.page.proof = Proof(c_, *requestSha.value, *sha.value); out_.page.matchOffset = offset_;
        auto& coverage = out_.page.coverage; coverage.state = partial_ ? TriggerReplaceState::Partial : TriggerReplaceState::Ready;
        coverage.textBytesCharged = writer_.Used(); if (!partial_) coverage.totalMatches = coverage.matchedInScannedScope;
        if (offset_ > coverage.matchedInScannedScope || (offset_ == coverage.matchedInScannedScope && offset_)) return Invalid();
        const auto end = offset_ + out_.page.rows.size();
        if (end < coverage.matchedInScannedScope) {
            if (out_.page.rows.empty()) return Budget(); out_.page.next = TriggerReplaceCursor{out_.page.proof, end};
        }
        return Result<TargetScan>::Success(std::move(out_));
    }
private:
    Result<TargetScan> Budget() const { return Failed<TargetScan>(Fail("TRIGGER_REPLACE_TARGET_BUDGET", "完整替换目录超过记录/文本预算；没有截半域猜测唯一性。")); }
    Result<TargetScan> Invalid() const { return Failed<TargetScan>(Fail("TRIGGER_REPLACE_TARGET_INVALID", "替换目录有重复身份、坏编码、过大字段或不可信来源。")); }
    bool Count(std::size_t count) { return Add(out_.page.coverage.graphRecordsCharged, count, l_.maxTargetRecords); }
    bool Text(std::string_view text) { return text.size() <= kText && Utf8(text) && writer_.Text(text); }
    TriggerReplacementOption Object(ObjectData::ObjectKey key, std::string display, std::string source) const {
        TriggerReplacementOption row; row.rowKey = "object/" + std::to_string(static_cast<unsigned>(key.type)) + "/" + key.rawcode;
        row.display = std::move(display); row.source = std::move(source); row.typeName = TypeName(key.type); row.replacement.kind = kind_; row.replacement.object = key; return row;
    }
    void Emit(TriggerReplacementOption row) {
        if (selected_ && row.replacement == *selected_) out_.selected = row;
        if (!filter_.empty() && !matcher_.Matches(row.display)
            && (!row.replacement.object || !matcher_.Matches(row.replacement.object->rawcode))) return;
        const auto count = out_.page.coverage.matchedInScannedScope++;
        if (count < offset_ || out_.page.rows.size() >= l_.pageLimit || pageFull_) return;
        const auto bytes = row.rowKey.size() + row.display.size() + row.source.size() + row.typeName.size()
            + row.replacement.preplacedSymbol.size() + row.replacement.mapObjectSnapshotIdentity.size() + 128;
        if (!Add(outputBytes_, bytes, l_.maxResultBytes)) { pageFull_ = true; return; }
        out_.page.rows.push_back(std::move(row));
    }
    const TriggerReplaceContext& c_; TriggerReplacementKind kind_; std::string_view filter_; const TriggerReplaceLimits& l_;
    LinearMatcher matcher_; std::size_t offset_, outputBytes_ = 0; const TriggerReplacement* selected_; Writer writer_; TargetScan out_;
    bool partial_ = false, pageFull_ = false; std::set<ObjectData::ObjectType> rejectedTypes_;
};

// 账本覆盖 IR 对象、全部原文本/原字节、路径/指纹与边数组；不读地图也不编码正文。
// 只读元数据先准入；完整 codec/identity/单 candidate/compact 账由唯一 Core 统计。
Status DocumentCost(const TriggerReplaceContext& c, const TriggerReplaceLimits& l, const Scan& scan,
    std::size_t& documentBytes, std::size_t& workUnits) {
    Trigger::TriggerGraphEditLimits limits; limits.maxScalarOwnedBytes = l.maxPreviewDocumentBytes;
    limits.maxScalarRecords = l.maxGraphRecords; limits.maxScalarEdges = l.maxGraphEdges; limits.scalarWorkPolicy = l.scalarWorkPolicy;
    const auto source = Trigger::InspectTriggerGraphScalarSourceCost(*c.document, limits);
    if (!source.Succeeded()) return {source.diagnostics};
    documentBytes = source.value->sourceDocumentBytes;
    workUnits = source.value->recordVisitsCharged;
    if (scan.selectedWorkOverflow || !Add(workUnits, source.value->edgeVisitsCharged, KnownWorkCap(l))
        || !Add(workUnits, scan.selectedWorkUnits, KnownWorkCap(l)))
        return Fail("TRIGGER_REPLACE_PREVIEW_WORK_BUDGET", "元数据及所选实际位置/类型路径收费已超过预览工作预算。");
    return Status::Ok();
}
std::string ReasonCode(const std::vector<Diagnostic>& diagnostics) { return diagnostics.empty() ? "TRIGGER_REPLACE_REJECTED" : diagnostics.front().code; }
std::string Reason(const std::vector<Diagnostic>& diagnostics) { return diagnostics.empty() ? "当前参数未取得可替换资格。" : diagnostics.front().message; }
} // namespace

Result<TriggerReplacementOptionsPage> QueryTriggerReplacementOptions(const TriggerReplaceContext& context,
    std::string_view expectedRevision, TriggerReplacementKind kind, std::string_view filter,
    const TriggerReplaceLimits& limits, const TriggerReplaceCursor* cursor) {
    if (const auto status = Context(context, expectedRevision); !status.Succeeded()) return Failed<TriggerReplacementOptionsPage>(status);
    if (const auto status = Limits(limits); !status.Succeeded()) return Failed<TriggerReplacementOptionsPage>(status);
    try {
        auto target = Targets(context, kind, filter, limits, cursor ? cursor->nextMatchOffset : 0, nullptr).Run();
        if (!target.Succeeded()) return Failed<TriggerReplacementOptionsPage>(target);
        if (cursor && cursor->proof != target.value->page.proof) return Failed<TriggerReplacementOptionsPage>(Fail("TRIGGER_REPLACE_CURSOR_STALE", "替换目录、筛选或预算已变化；旧分页无效。"));
        return Result<TriggerReplacementOptionsPage>::Success(std::move(target.value->page));
    } catch (const std::bad_alloc&) { return Failed<TriggerReplacementOptionsPage>(Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "有界替换目录分配失败。")); }
}

struct TriggerReplacePreviewData {
    TriggerReplaceQueryProof proof; TriggerReplaceQuery query; TriggerReplacement replacement;
    TriggerReplaceQueryProof targetProof; std::vector<std::string> selected;
    std::vector<TriggerReplacementPreviewRow> rows;
    Trigger::TriggerGraphEditBatch batch; Trigger::TriggerGraphScalarEditReceipt scalarReceipt;
};
struct TriggerReplacePlanData {
    TriggerReplacePreviewData prepared;
    TriggerReplaceLimits limits;
    std::string limitsSha256, inputSha256;
    std::size_t documentBytesCharged = 0, workUnitsCharged = 0, ownedBytes = 0;
    std::optional<Trigger::MapObjectReferenceSnapshot> mapObjects;
};

namespace {
Result<std::string> LimitsProof(const TriggerReplaceLimits& limits) {
    Writer writer(16384); writer.Text("trigger-replace-plan-policy-v2"); WriteLimits(writer, limits); return writer.Hash();
}
// 只证明所选完整参数及其实际挂载和数量，不冒整个 live 图身份。
// 未选字段/WCT 的 live 变化最终由强 Verify/唯一 Core Replay 的整体前身份拒绝。
Result<std::pair<std::string, std::size_t>> PlanInputProof(const Trigger::TriggerDocument& document,
    const Trigger::TriggerGraphEditBatch& batch, const TriggerReplaceLimits& limits) {
    using ProofResult = Result<std::pair<std::string, std::size_t>>;
    const auto invalid = [] { return Failed<std::pair<std::string, std::size_t>>(Fail("TRIGGER_REPLACE_PLAN_INPUT_STALE", "所选参数完整元数据、唯一真实挂载无效或超过计划身份预算。")); };
    if (batch.operations.empty() || batch.operations.size() > limits.maxSelection) return invalid();
    std::size_t records = 0, edges = 0;
    for (const auto count : {document.categories.size(), document.variables.size(), document.triggers.size(), document.ecaNodes.size(), document.argumentNodes.size()})
        if (!Add(records, count, limits.maxGraphRecords)) return invalid();
    for (const auto& trigger : document.triggers) if (!Add(edges, trigger.ecas.size(), limits.maxGraphEdges)) return invalid();
    for (const auto& owner : document.ecaNodes) if (!Add(edges, owner.arguments.size(), limits.maxGraphEdges) || !Add(edges, owner.children.size(), limits.maxGraphEdges)) return invalid();
    for (const auto& node : document.argumentNodes) if (!Add(edges, static_cast<std::size_t>(node.callEca.has_value()) + static_cast<std::size_t>(node.indexArgument.has_value()), limits.maxGraphEdges)) return invalid();
    struct Position { std::size_t argument = 0, owner = 0, ordinal = 0, nodeCount = 0, mountCount = 0; };
    std::map<StableId, Position> selected;
    for (const auto& operation : batch.operations) {
        const auto* set = std::get_if<Trigger::GraphSet>(&operation);
        if (!set || set->field != Trigger::GraphField::ArgumentScalar || set->target.kind != Trigger::GraphRecordKind::Argument
            || !set->target.id || !set->target.clientKey.empty() || !selected.emplace(set->target.id, Position{}).second) return invalid();
    }
    for (std::size_t index = 0; index < document.argumentNodes.size(); ++index)
        if (auto found = selected.find(document.argumentNodes[index].id); found != selected.end()) {
            found->second.argument = index; if (++found->second.nodeCount != 1) return invalid();
        }
    for (std::size_t oi = 0; oi < document.ecaNodes.size(); ++oi) {
        const auto& owner = document.ecaNodes[oi];
        for (std::size_t ordinal = 0; ordinal < owner.arguments.size(); ++ordinal) {
            const auto ai = owner.arguments[ordinal]; if (ai >= document.argumentNodes.size()) return invalid();
            if (auto found = selected.find(document.argumentNodes[ai].id); found != selected.end()) {
                found->second.owner = oi; found->second.ordinal = ordinal; if (++found->second.mountCount != 1) return invalid();
            }
        }
    }
    Writer writer(std::min<std::size_t>(4 * 1024 * 1024, limits.maxPreviewDocumentBytes));
    writer.Text("trigger-replace-selected-input-v2"); writer.Number(records); writer.Number(edges);
    writer.Number(document.categories.size()); writer.Number(document.variables.size()); writer.Number(document.triggers.size());
    writer.Number(document.ecaNodes.size()); writer.Number(document.argumentNodes.size());
    writer.Text(document.SourceWtgSha256()); writer.Text(document.SourceWctSha256()); writer.Text(document.UiCatalogSha256());
    const auto raw = [&](const Trigger::RawText& text) {
        return writer.Number(static_cast<unsigned>(text.encoding)) && writer.Text(text.text)
            && writer.Text(text.originalBytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(text.originalBytes.data()), text.originalBytes.size()));
    };
    const auto path = [&](const std::filesystem::path& value) {
        const auto& native = value.native();
        return native.size() <= std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type)
            && writer.Text(std::string_view(reinterpret_cast<const char*>(native.data()), native.size() * sizeof(std::filesystem::path::value_type)));
    };
    for (const auto& operation : batch.operations) {
        const auto& set = std::get<Trigger::GraphSet>(operation); const auto& position = selected.at(set.target.id);
        if (position.nodeCount != 1 || position.mountCount != 1) return invalid();
        const auto& node = document.argumentNodes[position.argument]; const auto& owner = document.ecaNodes[position.owner];
        if (!writer.Number(position.argument) || !writer.Number(position.owner) || !writer.Number(position.ordinal)
            || !writer.Number(node.id) || !writer.Number(node.type) || !raw(node.value) || !writer.Number(node.insertCall)
            || !writer.Number(node.callEca.has_value()) || (node.callEca && !writer.Number(*node.callEca))
            || !writer.Number(node.insertIndex) || !writer.Number(node.indexArgument.has_value()) || (node.indexArgument && !writer.Number(*node.indexArgument))
            || !path(node.origin.source) || !writer.Number(node.origin.offset) || !writer.Number(node.origin.length) || !writer.Text(node.origin.fingerprint)
            || !writer.Number(owner.id) || !writer.Number(owner.sourceTriggerId) || !writer.Number(owner.type)
            || !raw(owner.name) || !path(owner.uiSource) || !writer.Text(owner.uiLayer) || !writer.Text(owner.uiSignature)) return invalid();
    }
    const auto digest = writer.Hash(); if (!digest.Succeeded()) return Failed<std::pair<std::string, std::size_t>>(digest);
    std::size_t charged = writer.Used();
    if (!Add(charged, writer.Used(), limits.maxPreviewDocumentBytes)
        || !Add(charged, selected.size() * sizeof(std::pair<const StableId, Position>), limits.maxPreviewDocumentBytes)) return invalid();
    return ProofResult::Success({*digest.value, charged});
}
Result<std::size_t> PlanMapOwnedBytes(const Trigger::MapObjectReferenceSnapshot* source, const TriggerReplaceLimits& limits) {
    if (!source) return Result<std::size_t>::Success(0);
    if (source->References().size() > limits.maxTargetRecords || source->Inputs().size() > 1024 || source->Diagnostics().size() > 4096)
        return Failed<std::size_t>(Fail("TRIGGER_REPLACE_PLAN_MAP_BUDGET", "拥有预放快照的记录数超过计划预算。"));
    std::size_t bytes = sizeof(Trigger::MapObjectReferenceSnapshot);
    const auto charge = [&](std::size_t size) { return Add(bytes, size, limits.maxPreviewDocumentBytes); };
    bool valid = charge(source->Identity().size());
    for (const auto& reference : source->References()) valid = valid && charge(sizeof(reference)) && charge(reference.symbol.size())
        && charge(reference.type.size()) && charge(reference.rawcode.size()) && charge(reference.memberName.size())
        && charge(reference.memberSha256.size()) && charge(reference.classificationSource.size());
    for (const auto& input : source->Inputs()) valid = valid && charge(sizeof(input)) && charge(input.role.size())
        && charge(input.source.size()) && charge(input.bytes.size()) && charge(input.sha256.size());
    for (const auto& diagnostic : source->Diagnostics()) {
        valid = valid && charge(sizeof(diagnostic)) && charge(diagnostic.subsystem.size()) && charge(diagnostic.code.size()) && charge(diagnostic.message.size());
        if (diagnostic.origin) valid = valid && charge(diagnostic.origin->fingerprint.size())
            && diagnostic.origin->source.native().size() <= std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type)
            && charge(diagnostic.origin->source.native().size() * sizeof(std::filesystem::path::value_type));
    }
    if (!valid) return Failed<std::size_t>(Fail("TRIGGER_REPLACE_PLAN_MAP_BUDGET", "拥有预放快照的完整输入字节超过计划预算。"));
    return Result<std::size_t>::Success(bytes);
}
// UiCatalog 是独立拥有值；计其公开配置/源行/定义历史及真实私有根/索引键逻辑存储，不只计原始文件字节。
// Strings 为 shared_ptr<const> 只读共享服务资产；不含借用 QObject 或可变外部来源。
Result<std::size_t> CatalogOwnedBytes(const Trigger::UiCatalog& catalog, std::size_t cap) {
    const auto failure = [] { return Failed<std::size_t>(Fail("TRIGGER_REPLACE_PLAN_CATALOG_BUDGET", "owned Catalog 完整元数据超过准入预算。")); };
    if (catalog.Manifest().size() > 1024 || catalog.Documents().size() > 1024) return failure();
    std::size_t bytes = sizeof(Trigger::UiCatalog), definitions = 0, rawBytes = 0;
    const auto charge = [&](std::size_t count) { return Add(bytes, count, cap); };
    const auto additionalOwned = catalog.AdditionalOwnedBytes(cap);
    if (!additionalOwned || !charge(*additionalOwned)) return failure();
    const auto path = [&](const std::filesystem::path& value) {
        return value.native().size() <= std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type)
            && charge(value.native().size() * sizeof(std::filesystem::path::value_type));
    };
    if (!charge(catalog.Sha256().size())) return failure();
    for (const auto& layer : catalog.Layers()) if (!charge(sizeof(layer)) || !charge(layer.size())) return failure();
    for (const auto& manifest : catalog.Manifest()) if (!Add(definitions, manifest.definitionCount, 32768)
        || !charge(sizeof(manifest)) || !charge(manifest.layer.size()) || !charge(manifest.sha256.size()) || !path(manifest.relativePath)) return failure();
    const auto document = [&](const Trigger::UiTextDocument& source) {
        if (!Add(rawBytes, source.OriginalBytes().size(), 16 * 1024 * 1024) || !charge(sizeof(source))
            || !path(source.Source()) || !charge(source.OriginalBytes().size())) return false;
        for (const auto& line : source.Lines()) if (!charge(sizeof(line)) || !charge(line.text.size()) || !charge(line.lineEnding.size())
            || !charge(line.section.size()) || !charge(line.key.size()) || !charge(line.value.size())) return false;
        return true;
    };
    if (!document(catalog.ConfigDocument())) return failure();
    for (const auto& source : catalog.Documents()) if (!document(source)) return failure();
    std::size_t rows = 0;
    for (const auto kind : {Trigger::UiDefinitionKind::Define, Trigger::UiDefinitionKind::Event, Trigger::UiDefinitionKind::Condition, Trigger::UiDefinitionKind::Action, Trigger::UiDefinitionKind::Call}) {
        const auto ownedRows = catalog.Definitions(kind);
        if (!Add(rows, ownedRows.size(), definitions) || !charge(ownedRows.size() * sizeof(const Trigger::UiDefinition*))) return failure();
        for (const auto* row : ownedRows) {
            if (!row || !charge(sizeof(*row)) || !charge(row->group.size()) || !charge(row->name.size())) return failure();
            for (const auto& revision : row->revisions) {
                if (!charge(sizeof(revision)) || !charge(revision.group.size()) || !charge(revision.name.size()) || !charge(revision.layer.size())
                    || !path(revision.source) || !charge(revision.sourceSha256.size())) return failure();
                for (const auto& field : revision.fields) if (!charge(sizeof(field)) || !charge(field.first.size()) || !charge(field.second.size())) return failure();
                for (const auto& argument : revision.arguments) {
                    if (!charge(sizeof(argument)) || !charge(argument.type.size()) || !charge(argument.defaultValue.size())) return failure();
                    for (const auto& field : argument.fields) if (!charge(sizeof(field)) || !charge(field.first.size()) || !charge(field.second.size())) return failure();
                }
            }
        }
    }
    return Result<std::size_t>::Success(bytes);
}
Result<std::size_t> PreviewOwnedBytes(std::span<const TriggerReplacementPreviewRow> rows,
    const Trigger::TriggerGraphEditBatch& batch, const Scan& scan,
    std::span<const Trigger::ScalarArgumentReplacementGuard> guards, const TriggerReplacementOptionsPage& targets,
    const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof, const TriggerReplacement& replacement,
    std::span<const std::string> selected) {
    constexpr std::size_t cap = 512 * 1024 * 1024;
    std::size_t bytes = sizeof(TriggerReplacePreviewData) + sizeof(TriggerReplacePreviewResult) + sizeof(Scan);
    const auto text = [&](std::string_view value) { return Add(bytes, value.size(), cap); };
    const auto array = [&](std::size_t count, std::size_t size) { return count <= cap / size && Add(bytes, count * size, cap); };
    const auto hit = [&](const TriggerReplaceHit& value) {
        return text(value.rowKey) && text(value.triggerName) && text(value.functionName) && text(value.oldValue)
            && text(value.displayValue) && text(value.declaredType) && text(value.reasonCode) && text(value.reason);
    };
    const auto bound = [&](const TriggerReplaceQueryProof& value) {
        return text(value.contextRevision) && text(value.requestSha256) && text(value.snapshotSha256)
            && text(value.sourceWtgSha256) && text(value.sourceWctSha256) && text(value.catalogSha256);
    };
    // GUI PlanResult、不可变 Plan、worker PreviewResult、不可变 Preview 可同时存活，分别计四份 row。
    bool valid = array(rows.size() * 4, sizeof(TriggerReplacementPreviewRow)) && array(selected.size() * 2, sizeof(std::string))
        && array(scan.page.rows.size() + scan.selected.size(), sizeof(TriggerReplaceHit))
        && array(guards.size(), sizeof(Trigger::ScalarArgumentReplacementGuard))
        && array(targets.rows.size(), sizeof(TriggerReplacementOption)) && array(batch.operations.size() * 2, sizeof(Trigger::GraphEditOperation))
        && bound(proof) && bound(proof) && bound(proof) && bound(scan.page.proof) && bound(targets.proof) && bound(targets.proof);
    for (unsigned copy = 0; copy < 2; ++copy) valid = valid && text(query.needle) && text(replacement.preplacedSymbol) && text(replacement.mapObjectSnapshotIdentity)
        && (!query.sourceObject || text(query.sourceObject->rawcode)) && (!replacement.object || text(replacement.object->rawcode));
    for (const auto& row : rows) for (unsigned copy = 0; copy < 4; ++copy)
        valid = valid && hit(row.hit) && text(row.expectedType) && text(row.replacementValue) && text(row.reasonCode) && text(row.reason);
    for (const auto& key : selected) valid = valid && text(key) && text(key);
    for (const auto& value : scan.page.rows) valid = valid && hit(value);
    for (const auto& value : scan.selected) valid = valid && hit(value);
    for (const auto& guarded : guards) if (guarded.guard)
        valid = valid && text(guarded.guard->uiSignature) && text(guarded.guard->expectedType) && text(guarded.guard->subtreeSha256);
    for (const auto& option : targets.rows) valid = valid && text(option.rowKey) && text(option.display) && text(option.source)
        && text(option.typeName) && text(option.replacement.preplacedSymbol) && text(option.replacement.mapObjectSnapshotIdentity)
        && (!option.replacement.object || text(option.replacement.object->rawcode));
    for (const auto& operation : batch.operations) for (unsigned copy = 0; copy < 2; ++copy) {
        const auto* set = std::get_if<Trigger::GraphSet>(&operation);
        if (!set) { valid = false; break; }
        valid = valid && text(set->target.clientKey) && text(std::get<Trigger::GraphScalarValue>(set->expected).value)
            && text(std::get<Trigger::GraphScalarValue>(set->value).value);
    }
    // Source/current SHA 字符串与返回 batch 的真正 before binding；目标 selected 与 rows 可能各拥有一份。
    valid = valid && Add(bytes, 12 * 64 + sizeof(TriggerReplacementOption) + 6 * kText, cap);
    if (!valid) return Failed<std::size_t>(Fail("TRIGGER_REPLACE_PREVIEW_BUDGET", "请求/选中项/预览行/guard拥有账溢出或超预算，未复制整图。"));
    return Result<std::size_t>::Success(bytes);
}
}
bool TriggerReplacePlan::Empty() const noexcept { return !data_ || data_->prepared.batch.operations.empty(); }
const TriggerReplaceQueryProof& TriggerReplacePlan::Proof() const noexcept { static const TriggerReplaceQueryProof empty; return data_ ? data_->prepared.proof : empty; }
std::span<const TriggerReplacementPreviewRow> TriggerReplacePlan::Rows() const noexcept { return data_ ? std::span<const TriggerReplacementPreviewRow>(data_->prepared.rows) : std::span<const TriggerReplacementPreviewRow>{}; }
const TriggerReplaceQueryProof& TriggerReplacePreview::Proof() const noexcept { return data_->proof; }
std::span<const TriggerReplacementPreviewRow> TriggerReplacePreview::Rows() const noexcept { return data_->rows; }
const Trigger::TriggerGraphScalarEditReceipt& TriggerReplacePreview::ScalarReceipt() const noexcept { return data_->scalarReceipt; }
const Trigger::TriggerGraphEditBatch& TriggerReplacePreview::Batch() const noexcept { return data_->batch; }
const Trigger::TriggerGraphEditReceipt& TriggerReplacePreview::Receipt() const noexcept { return data_->scalarReceipt.Receipt(); }

class TriggerReplacePlanner {
public:
    static Result<TriggerReplacePlanResult> Prepare(const TriggerReplaceContext& c, const TriggerReplaceQuery& query,
        const TriggerReplaceQueryProof& proof, std::span<const std::string> selected, const TriggerReplacement& replacement,
        const TriggerReplaceLimits& l) {
        TriggerReplacePlanResult out;
        if (selected.empty() || selected.size() > l.maxSelection) return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_SELECTION_BUDGET", "必须勾选 1..256 个同查询身份的命中项。"));
        std::set<std::string> unique;
        for (const auto& key : selected) if (key.empty() || key.size() > 128 || !Utf8(key) || !unique.insert(key).second)
            return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_SELECTION_INVALID", "选择需要唯一、有界的稳定命中身份。"));
        const auto catalogBytes = CatalogOwnedBytes(*c.catalog, l.maxPreviewDocumentBytes);
        if (!catalogBytes.Succeeded()) return Failed<TriggerReplacePlanResult>(catalogBytes);
        auto scan = Scanner(c, query, l, 0, selected).Run();
        if (!scan.Succeeded()) return Failed<TriggerReplacePlanResult>(scan);
        if (scan.value->page.proof != proof) return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_PREVIEW_STALE", "查询、来源、原值或预算已变化，请重新查找。"));
        // 公开 proof 六串可能伪造为任意大小；只复制已比对成功的有界真实扫描 proof。
        out.proof = scan.value->page.proof;
        if (scan.value->selected.size() != selected.size()) return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_SELECTION_MISSING", "有勾选身份不属于当前已扫描命中范围，未丢弃选择。"));
        const auto cost = DocumentCost(c, l, *scan.value, out.documentBytesCharged, out.workUnitsCharged);
        if (!cost.Succeeded()) return Failed<TriggerReplacePlanResult>(cost);
        auto targets = Targets(c, replacement.kind, {}, l, 0, &replacement).Run();
        if (!targets.Succeeded()) return Failed<TriggerReplacePlanResult>(targets);
        if (!targets.value->selected) return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_TARGET_MISSING", "所选替换对象未取得当前唯一来源资格。"));
        std::vector<StableId> ids; bool all = true;
        for (const auto& hit : scan.value->selected) {
            TriggerReplacementPreviewRow row; row.hit = hit;
            if (hit.eligibility != TriggerReplaceEligibility::PreviewRequired || !hit.argumentId) {
                row.reasonCode = hit.reasonCode; row.reason = hit.reason; all = false;
            } else ids.push_back(*hit.argumentId);
            out.rows.push_back(std::move(row));
        }
        if (!all) return Result<TriggerReplacePlanResult>::Success(std::move(out));
        // 在批量 Guard 复制 owner.uiSignature 之前先核所选重复签名的 scratch；不再把旧工作门当内存门。
        std::map<StableId, std::size_t> selectedOwners;
        for (const auto& row : out.rows) ++selectedOwners[row.hit.ecaId];
        std::size_t guardScratch = 0;
        for (const auto& owner : c.document->ecaNodes) if (const auto found = selectedOwners.find(owner.id); found != selectedOwners.end()) {
            const auto perGuard = sizeof(Trigger::ScalarArgumentReplacementGuard) + 2 * kText + 256;
            if (owner.uiSignature.size() > l.maxPreviewDocumentBytes || perGuard > l.maxPreviewDocumentBytes - owner.uiSignature.size()
                || found->second > l.maxPreviewDocumentBytes / (perGuard + owner.uiSignature.size())
                || !Add(guardScratch, found->second * (perGuard + owner.uiSignature.size()), l.maxPreviewDocumentBytes))
                return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_PLAN_GUARD_BUDGET", "所选 guard 重复拥有签名超过逻辑准入预算。"));
        }
        const auto guards = Trigger::CaptureScalarArgumentReplacementGuards(*c.document, *c.catalog, ids, {}, c.mapObjects);
        if (!guards.Succeeded()) return Failed<TriggerReplacePlanResult>(guards);
        const auto& target = *targets.value->selected;
        const auto scalarKind = replacement.kind == TriggerReplacementKind::ObjectLiteral ? 3 : 1;
        const std::string scalar = replacement.kind == TriggerReplacementKind::ObjectLiteral ? replacement.object->rawcode
            : replacement.kind == TriggerReplacementKind::PreplacedObject ? replacement.preplacedSymbol : target.display;
        std::optional<Trigger::VariableReference> targetReference;
        if (scalarKind == 1) {
            const auto reference = Trigger::ResolveVariableReference(*c.document, scalar, c.mapObjects);
            if (!reference.Succeeded()) return Failed<TriggerReplacePlanResult>(reference);
            if (reference.value->isArray || (replacement.kind == TriggerReplacementKind::Variable && reference.value->kind != Trigger::VariableReferenceKind::Declared)
                || (replacement.kind == TriggerReplacementKind::PreplacedObject && (reference.value->kind != Trigger::VariableReferenceKind::PreplacedObject
                    || !c.mapObjects || c.mapObjects->Identity() != replacement.mapObjectSnapshotIdentity)))
                return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_TARGET_INVALID", "替换声明或预放对象快照与当前真实来源不一致。"));
            targetReference = *reference.value;
        }
        Trigger::TriggerGraphEditBatch batch;
        for (std::size_t index = 0; index < out.rows.size(); ++index) {
            auto& row = out.rows[index]; const auto& guard = (*guards.value)[index];
            row.replacementValue = scalar; row.replacementKind = scalarKind;
            if (!guard.guard) { row.reasonCode = ReasonCode(guard.diagnostics); row.reason = Reason(guard.diagnostics); all = false; continue; }
            row.expectedType = guard.guard->expectedType;
            const bool compatible = scalarKind == 1 ? c.types->Accepts(row.expectedType, targetReference->type)
                : ObjectType(*c.types, row.expectedType) == replacement.object->type && c.types->ValidateLiteral(row.expectedType, scalar).Succeeded();
            if (!compatible) { row.reasonCode = "TRIGGER_REPLACE_TYPE_MISMATCH"; row.reason = "真实参数类型与所选变量/对象类型不兼容；整批不发布。"; all = false; continue; }
            if (guard.guard->ownerEcaId != row.hit.ecaId || guard.guard->sourceTriggerId != row.hit.triggerId) {
                row.reasonCode = "TRIGGER_REPLACE_OWNER_STALE"; row.reason = "参数当前真实所属 ECA/触发器与命中身份不符。"; all = false; continue;
            }
            row.eligible = true;
            batch.operations.emplace_back(Trigger::GraphSet{{Trigger::GraphRecordKind::Argument, *row.hit.argumentId, {}},
                Trigger::GraphField::ArgumentScalar, Trigger::GraphScalarValue{row.hit.oldKind, row.hit.oldValue}, Trigger::GraphScalarValue{scalarKind, scalar}});
        }
        if (!all) return Result<TriggerReplacePlanResult>::Success(std::move(out));
        // 本轮 const Prepare 在 Core 内建立完整 before；source-only 不是一般 Apply 的 wildcard。
        batch.expected = {std::string(c.document->SourceWtgSha256()), std::string(c.document->SourceWctSha256()), c.catalog->Sha256(), {}, {},
            c.mapObjects ? c.mapObjects->Identity() : std::string{}};
        const auto owned = PreviewOwnedBytes(out.rows, batch, *scan.value, *guards.value, targets.value->page, query, proof, replacement, selected);
        if (!owned.Succeeded()) return Failed<TriggerReplacePlanResult>(owned);
        const auto input = PlanInputProof(*c.document, batch, l);
        if (!input.Succeeded()) return Failed<TriggerReplacePlanResult>(input);
        const auto mapBytes = PlanMapOwnedBytes(c.mapObjects, l);
        if (!mapBytes.Succeeded()) return Failed<TriggerReplacePlanResult>(mapBytes);
        // GUI clone 之前已经核未来独立拥有 Catalog 的完整元数据账，worker 会再核实际输入。
        std::size_t allOwned = *owned.value;
        if (!Add(allOwned, *mapBytes.value, l.maxPreviewDocumentBytes)
            || !Add(allOwned, input.value->second + sizeof(TriggerReplacePlanData), l.maxPreviewDocumentBytes))
            return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_PLAN_BUDGET", "拥有计划、所选身份或预放来源超过逻辑预算。"));
        std::size_t pendingPeak = out.documentBytesCharged;
        if (!Add(pendingPeak, allOwned, l.maxPreviewDocumentBytes) || !Add(pendingPeak, *catalogBytes.value, l.maxPreviewDocumentBytes))
            return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_PLAN_BUDGET", "GUI 一次 owned 文档/Catalog 与计划的逻辑持有准入失败。"));
        const auto limitsProof = LimitsProof(l); if (!limitsProof.Succeeded()) return Failed<TriggerReplacePlanResult>(limitsProof);
        auto data = std::make_shared<TriggerReplacePlanData>();
        data->prepared.proof = proof; data->prepared.query = query; data->prepared.replacement = replacement;
        data->prepared.targetProof = targets.value->page.proof;
        data->prepared.selected.assign(selected.begin(), selected.end()); data->prepared.rows = out.rows;
        data->prepared.batch = std::move(batch); data->limits = l; data->limitsSha256 = *limitsProof.value;
        data->inputSha256 = input.value->first; data->ownedBytes = allOwned;
        data->documentBytesCharged = out.documentBytesCharged; data->workUnitsCharged = out.workUnitsCharged;
        if (c.mapObjects) data->mapObjects.emplace(*c.mapObjects);
        out.plan = TriggerReplacePlan(std::move(data)); return Result<TriggerReplacePlanResult>::Success(std::move(out));
    }
    static Result<TriggerReplacePreviewResult> Complete(const TriggerReplacePlan& plan, Trigger::TriggerDocument document,
        Trigger::UiCatalog catalog, const TriggerReplaceLimits& l, std::stop_token stop) {
        if (plan.Empty()) return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_PLAN_UNBOUND", "缺少服务实际创建的拥有计划。"));
        const auto& source = *plan.data_;
        const auto limitsProof = LimitsProof(l); if (!limitsProof.Succeeded()) return Failed<TriggerReplacePreviewResult>(limitsProof);
        if (*limitsProof.value != source.limitsSha256)
            return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_PLAN_LIMITS_STALE", "异步计划政策或任一预算已变化。"));
        if (stop.stop_requested()) return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_CANCELLED", "完成阶段开始前取消。"));
        const auto& expected = source.prepared.batch.expected;
        if (!document.HasTrustedSourceSeal() || document.SourceWtgSha256() != expected.sourceWtgSha256
            || document.SourceWctSha256() != expected.sourceWctSha256 || document.UiCatalogSha256() != expected.uiCatalogSha256
            || catalog.Sha256() != expected.uiCatalogSha256)
            return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_PLAN_INPUT_STALE", "owned 文档来源或 Catalog 与计划不同。"));
        const auto input = PlanInputProof(document, source.prepared.batch, l);
        if (!input.Succeeded()) return Failed<TriggerReplacePreviewResult>(input);
        if (input.value->first != source.inputSha256)
            return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_PLAN_INPUT_STALE", "owned 所选完整参数、真实父挂载或结构计数已变化。"));
        const auto catalogOwned = CatalogOwnedBytes(catalog, l.maxPreviewDocumentBytes);
        if (!catalogOwned.Succeeded()) return Failed<TriggerReplacePreviewResult>(catalogOwned);
        Trigger::TriggerGraphEditLimits core; core.maxScalarOwnedBytes = l.maxPreviewDocumentBytes;
        core.maxScalarWorkUnits = l.maxPreviewWorkUnits; core.scalarWorkPolicy = l.scalarWorkPolicy;
        core.maxScalarRecords = l.maxGraphRecords; core.maxScalarEdges = l.maxGraphEdges;
        core.scalarAdditionalWorkUnits = source.workUnitsCharged;
        core.scalarAdditionalOwnedBytes = source.ownedBytes;
        if (!Add(core.scalarAdditionalOwnedBytes, *catalogOwned.value, l.maxPreviewDocumentBytes))
            return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_PLAN_BUDGET", "owned Catalog 与计划合计超过逻辑预算。"));
        const auto applied = Trigger::PrepareOwnedTriggerGraphScalarEditBatch(std::move(document), catalog,
            source.prepared.batch, core, source.mapObjects ? &*source.mapObjects : nullptr, stop);
        if (!applied.Succeeded()) return Failed<TriggerReplacePreviewResult>(applied);
        if (stop.stop_requested()) return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_CANCELLED", "终态完成后取消；结果未发布。"));
        TriggerReplacePreviewResult out; out.proof = source.prepared.proof; out.rows = source.prepared.rows;
        auto data = std::make_shared<TriggerReplacePreviewData>(source.prepared);
        data->scalarReceipt = *applied.value; data->batch.expected = applied.value->Receipt().before;
        out.cost = applied.value->Cost(); out.documentBytesCharged = out.cost.peakOwnedBytes; out.workUnitsCharged = out.cost.workUnitsCharged;
        out.preview = TriggerReplacePreview(std::move(data)); return Result<TriggerReplacePreviewResult>::Success(std::move(out));
    }
    static Status CheckContext(const TriggerReplaceContext& c, const TriggerReplacePreview& preview, const TriggerReplaceLimits& l) {
        if (!preview.data_ || preview.data_->scalarReceipt.Empty()) return Fail("TRIGGER_REPLACE_PREVIEW_UNBOUND", "没有服务实际创建的非空预览。");
        const auto& data = *preview.data_; const auto scan = Scanner(c, data.query, l, 0, data.selected).Run();
        if (!scan.Succeeded()) return {scan.diagnostics};
        if (scan.value->page.proof != data.proof) return Fail("TRIGGER_REPLACE_PREVIEW_STALE", "预览后来源、原值、查询或预算变化，不能发布旧候选。");
        const auto target = Targets(c, data.replacement.kind, {}, l, 0, &data.replacement).Run();
        if (!target.Succeeded()) return {target.diagnostics};
        if (!target.value->selected || target.value->page.proof != data.targetProof) return Fail("TRIGGER_REPLACE_TARGET_STALE", "预览后替换目录或真实目标变化。");
        std::size_t bytes = 0, work = 0; const auto cost = DocumentCost(c, l, *scan.value, bytes, work); if (!cost.Succeeded()) return cost;
        const auto& before = data.scalarReceipt.Receipt().before;
        if (data.scalarReceipt.Empty() || (c.mapObjects ? c.mapObjects->Identity() : std::string{}) != before.mapObjectReferencesSha256)
            return Fail("TRIGGER_REPLACE_PREVIEW_STALE", "紧凑收据为空或当前预放对象快照身份已变化。");
        if (data.scalarReceipt.Cost().peakOwnedBytes > l.maxPreviewDocumentBytes
            || data.scalarReceipt.Cost().workPolicy != l.scalarWorkPolicy
            || (l.scalarWorkPolicy == Trigger::TriggerScalarWorkPolicy::LegacyKnownWork && data.scalarReceipt.Cost().workUnitsCharged > l.maxPreviewWorkUnits))
            return Fail("TRIGGER_REPLACE_PREVIEW_BUDGET", "密封紧凑收据的实际收费不符合当前预算。");
        return Status::Ok(); // 此入口明确不检查 wholeGraph；发布必须随后唯一 Core Replay。
    }
    static Status Verify(const TriggerReplaceContext& c, const TriggerReplacePreview& preview, const TriggerReplaceLimits& l) {
        if (const auto context = CheckContext(c, preview, l); !context.Succeeded()) return context;
        return Trigger::VerifyTriggerGraphScalarReplay(*c.document, *c.catalog, preview.data_->scalarReceipt, false);
    }
};

Result<TriggerReplacePlanResult> PrepareTriggerReplacementPlan(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof,
    std::span<const std::string> selectedRowKeys, const TriggerReplacement& replacement, const TriggerReplaceLimits& limits) {
    if (const auto status = Context(context, expectedRevision); !status.Succeeded()) return Failed<TriggerReplacePlanResult>(status);
    if (const auto status = Limits(limits); !status.Succeeded()) return Failed<TriggerReplacePlanResult>(status);
    if (const auto status = Query(query); !status.Succeeded()) return Failed<TriggerReplacePlanResult>(status);
    try { return TriggerReplacePlanner::Prepare(context, query, proof, selectedRowKeys, replacement, limits); }
    catch (...) { return Failed<TriggerReplacePlanResult>(Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "拥有计划准备失败，源未修改。")); }
}
Result<TriggerReplacePreviewResult> CompleteTriggerReplacementPlan(const TriggerReplacePlan& plan,
    Trigger::TriggerDocument ownedDocument, Trigger::UiCatalog ownedCatalog, const TriggerReplaceLimits& limits, std::stop_token stop) {
    if (const auto status = Limits(limits); !status.Succeeded()) return Failed<TriggerReplacePreviewResult>(status);
    try { return TriggerReplacePlanner::Complete(plan, std::move(ownedDocument), std::move(ownedCatalog), limits, stop); }
    catch (...) { return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "worker 拥有候选完成失败；私有输入丢弃，源未发布。")); }
}
Result<TriggerReplacePreviewResult> PrepareTriggerReplacement(const TriggerReplaceContext& context,
    std::string_view expectedRevision, const TriggerReplaceQuery& query, const TriggerReplaceQueryProof& proof,
    std::span<const std::string> selectedRowKeys, const TriggerReplacement& replacement, const TriggerReplaceLimits& limits) {
    const auto plan = PrepareTriggerReplacementPlan(context, expectedRevision, query, proof, selectedRowKeys, replacement, limits);
    if (!plan.Succeeded()) return Failed<TriggerReplacePreviewResult>(plan);
    if (!plan.value->plan) {
        TriggerReplacePreviewResult out; out.proof = plan.value->proof; out.rows = plan.value->rows;
        out.documentBytesCharged = plan.value->documentBytesCharged; out.workUnitsCharged = plan.value->workUnitsCharged;
        return Result<TriggerReplacePreviewResult>::Success(std::move(out));
    }
    try { return CompleteTriggerReplacementPlan(*plan.value->plan, *context.document, *context.catalog, limits); }
    catch (...) { return Failed<TriggerReplacePreviewResult>(Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "唯一私有快照复制失败；源未发布。")); }
}
Status CheckTriggerReplacementPublicationContext(const TriggerReplaceContext& context, std::string_view expectedRevision,
    const TriggerReplacePreview& preview, const TriggerReplaceLimits& limits) {
    if (const auto status = Context(context, expectedRevision); !status.Succeeded()) return status;
    if (const auto status = Limits(limits); !status.Succeeded()) return status;
    try { return TriggerReplacePlanner::CheckContext(context, preview, limits); }
    catch (...) { return Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "上下文核验失败；尚无整图发布资格。"); }
}
Status VerifyTriggerReplacementPreview(const TriggerReplaceContext& context, std::string_view expectedRevision,
    const TriggerReplacePreview& preview, const TriggerReplaceLimits& limits) {
    if (const auto status = Context(context, expectedRevision); !status.Succeeded()) return status;
    if (const auto status = Limits(limits); !status.Succeeded()) return status;
    try { return TriggerReplacePlanner::Verify(context, preview, limits); }
    catch (const std::bad_alloc&) { return Fail("TRIGGER_REPLACE_ALLOCATION_FAILED", "发布核验分配失败，未发布候选。"); }
}

} // namespace War3Map::Services
