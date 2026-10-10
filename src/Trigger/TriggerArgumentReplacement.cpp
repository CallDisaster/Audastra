#include "TriggerArgumentReplacement.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace War3Map::Trigger {
namespace {
constexpr const char* kDomain = "Trigger.GraphEdit";
constexpr std::size_t kMaxFingerprintBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxClosureRecords = 250000;

Status Fail(std::string code, std::string message) {
    return Status::Failure(kDomain, std::move(code), std::move(message));
}
template<class T> Result<T> Failed(const Status& status) {
    Result<T> result; result.diagnostics = status.diagnostics; return result;
}
template<class T, class U> Result<T> Failed(const Result<U>& result) {
    Result<T> output; output.diagnostics = result.diagnostics; return output;
}

bool CodeName(std::string_view value) {
    return value == "ExecuteFunc" || value == "CustomScriptCode" || value == "YDWECustomScriptCode";
}
bool CodeDefinition(const UiDefinition& definition) {
    if (CodeName(definition.name)) return true;
    for (const auto& [key, value] : definition.Active().fields) {
        if ((key == "script_name" || key == "script") && CodeName(value)) return true;
    }
    return false;
}
bool Unresolved(std::string_view type) {
    return type.empty() || type == "Null" || type == "AnyType" || type == "anytype"
        || type == "AnyGlobal" || type == "AnyReturnType";
}
Status CheckType(const TriggerTypeSystem& types, std::string_view type) {
    if (Unresolved(type)) return Fail("TRIGGER_GRAPH_TYPE_UNRESOLVED", "参数替换不能猜测未知或任意返回类型。");
    if (type == "code" || types.LiteralKind(type) == TriggerTypeSystem::Literal::Script)
        return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "参数替换不能改写代码槽或代码回调。");
    if (types.LiteralKind(type) == TriggerTypeSystem::Literal::Other
        && !types.Find(type) && !types.Accepts("handle", type))
        return Fail("TRIGGER_GRAPH_TYPE_UNRESOLVED", "参数替换的类型没有真实目录或已知 native 父链。");
    return Status::Ok();
}

// 编码为长度分隔字段；边以 StableId 和实际顺序表示，不依赖压实后的 vector 下标。
class FingerprintWriter {
public:
    void Add(std::string_view value) {
        if (!bounded_) return;
        const auto size = std::to_string(value.size());
        if (size.size() + 1 > kMaxFingerprintBytes - payload_.size()
            || value.size() > kMaxFingerprintBytes - payload_.size() - size.size() - 1) {
            bounded_ = false; return;
        }
        payload_ += size; payload_ += ':'; payload_ += value;
    }
    template<class T> void Number(T value) { Add(std::to_string(value)); }
    void Bytes(std::span<const std::byte> value) {
        Add(value.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
    }
    Status Raw(const RawText& value) {
        Add(value.text); Number(static_cast<unsigned>(value.encoding)); Bytes(value.originalBytes);
        if (const auto status = Check(); !status.Succeeded()) return status;
        const auto encoded = value.ToBytes();
        if (!encoded.Succeeded()) return {encoded.diagnostics};
        Bytes(*encoded.value);
        return Check();
    }
    [[nodiscard]] Status Check() const {
        return bounded_ ? Status::Ok() : Fail("TRIGGER_GRAPH_BUDGET", "参数子树原字节指纹超过 64 MiB 工作预算。");
    }
    [[nodiscard]] Result<std::string> Finish() const {
        if (const auto status = Check(); !status.Succeeded()) return Failed<std::string>(status);
        return Sha256Text(payload_);
    }
private:
    std::string payload_;
    bool bounded_ = true;
};

enum class MountKind { None, Trigger, Eca, Argument };
struct Mount {
    MountKind kind = MountKind::None;
    std::size_t parent = 0;
    std::uint32_t ordinal = 0;
};
struct Location {
    std::uint32_t argumentIndex = 0;
    std::size_t ownerIndex = 0, triggerIndex = 0;
    std::uint32_t ordinal = 0;
    std::vector<std::pair<std::size_t, std::uint32_t>> ancestorSlots;
};

Result<Location> LocateFromMounts(const TriggerDocument& document, Location location,
    std::span<const Mount> ecas, std::span<const Mount> arguments) {
    const auto direct = arguments[location.argumentIndex];
    if (direct.kind != MountKind::Eca)
        return Failed<Location>(Fail("TRIGGER_GRAPH_ARGUMENT_MOUNT_UNSUPPORTED", "本轮只替换已有 ECA 的直接参数槽，不替换数组下标根。"));
    location.ownerIndex = direct.parent; location.ordinal = direct.ordinal;
    std::vector<std::size_t> ancestorEcas;
    Mount cursor{MountKind::Eca, direct.parent, direct.ordinal};
    for (std::size_t depth = 0; depth <= 256; ++depth) {
        if (cursor.kind == MountKind::Eca) {
            ancestorEcas.push_back(cursor.parent);
            cursor = ecas[cursor.parent];
        } else if (cursor.kind == MountKind::Argument) {
            const auto parent = arguments[cursor.parent];
            if (parent.kind == MountKind::Eca) location.ancestorSlots.emplace_back(parent.parent, parent.ordinal);
            cursor = parent;
        } else if (cursor.kind == MountKind::Trigger) {
            location.triggerIndex = cursor.parent;
            const auto& trigger = document.triggers[cursor.parent];
            if (trigger.type != 0 || trigger.customTextFlag != 0)
                return Failed<Location>(Fail("TRIGGER_GRAPH_CUSTOM_READ_ONLY", "原位参数替换只接受普通 GUI 触发器。"));
            for (const auto index : ancestorEcas) if (document.ecaNodes[index].sourceTriggerId != trigger.id)
                return Failed<Location>(Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "参数祖先的 sourceTriggerId 与实际触发器所有权不符。"));
            return Result<Location>::Success(std::move(location));
        } else break;
    }
    return Failed<Location>(Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "参数没有有界、可验证的触发器祖先。"));
}

Status FillMounts(const TriggerDocument& document, std::vector<Mount>& ecas, std::vector<Mount>& arguments) {
    for (std::size_t index = 0; index < document.triggers.size(); ++index)
        for (const auto root : document.triggers[index].ecas) ecas[root] = {MountKind::Trigger, index, 0};
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        const auto& node = document.ecaNodes[index];
        if (node.arguments.size() > std::numeric_limits<std::uint32_t>::max())
            return Fail("TRIGGER_GRAPH_BUDGET", "参数序号超过合同范围。");
        for (std::size_t ordinal = 0; ordinal < node.arguments.size(); ++ordinal)
            arguments[node.arguments[ordinal]] = {MountKind::Eca, index, static_cast<std::uint32_t>(ordinal)};
        for (const auto child : node.children) ecas[child] = {MountKind::Eca, index, 0};
    }
    for (std::size_t index = 0; index < document.argumentNodes.size(); ++index) {
        const auto& argument = document.argumentNodes[index];
        if (argument.callEca) ecas[*argument.callEca] = {MountKind::Argument, index, 0};
        if (argument.indexArgument) arguments[*argument.indexArgument] = {MountKind::Argument, index, 0};
    }
    return Status::Ok();
}

Result<Location> Locate(const TriggerDocument& document, StableId id) {
    const auto target = std::find_if(document.argumentNodes.begin(), document.argumentNodes.end(),
        [id](const ArgumentNode& argument) { return argument.id == id; });
    if (target == document.argumentNodes.end()) return Failed<Location>(Fail("TRIGGER_GRAPH_ARGUMENT_MISSING", "目标参数不存在。"));
    if (document.ecaNodes.size() >= kInvalidNodeIndex || document.argumentNodes.size() >= kInvalidNodeIndex)
        return Failed<Location>(Fail("TRIGGER_GRAPH_BUDGET", "参数图超出格式索引空间。"));
    Location location;
    location.argumentIndex = static_cast<std::uint32_t>(target - document.argumentNodes.begin());
    std::vector<Mount> ecas(document.ecaNodes.size()), arguments(document.argumentNodes.size());
    // 调用者先运行 codec.Validate；这里记录它已验证的唯一所有权，随后只走目标祖先路径。
    if (const auto filled = FillMounts(document, ecas, arguments); !filled.Succeeded()) return Failed<Location>(filled);
    return LocateFromMounts(document, std::move(location), ecas, arguments);
}

// 批量标量查询共享现有 Inspect 的准备；不缓存文档指针到异步或改写 IR。
class PreparedInspection {
public:
    PreparedInspection(const TriggerDocument& document, const UiCatalog& catalog,
        const MapObjectReferenceSnapshot* mapObjects)
        : document_(document), types(catalog, mapObjects), ecas_(document.ecaNodes.size()),
          arguments_(document.argumentNodes.size()), ecaPass_(document.ecaNodes.size()),
          argumentPass_(document.argumentNodes.size()) {}
    Status Initialize() {
        if (const auto filled = FillMounts(document_, ecas_, arguments_); !filled.Succeeded()) return filled;
        for (std::size_t index = 0; index < document_.argumentNodes.size(); ++index) {
            const auto id = document_.argumentNodes[index].id;
            if (!id || !byId_.emplace(id, static_cast<std::uint32_t>(index)).second)
                return Fail("TRIGGER_GRAPH_ARGUMENT_OWNER_INVALID", "批量标量准备发现零或重复参数身份。");
        }
        return Status::Ok();
    }
    Result<Location> LocateArgument(StableId id) const {
        const auto found = byId_.find(id);
        if (found == byId_.end()) return Failed<Location>(Fail("TRIGGER_GRAPH_ARGUMENT_MISSING", "目标参数不存在。"));
        Location location; location.argumentIndex = found->second;
        return LocateFromMounts(document_, std::move(location), ecas_, arguments_);
    }
    const ArgumentNode* Argument(StableId id) const {
        const auto found = byId_.find(id);
        return found == byId_.end() ? nullptr : &document_.argumentNodes[found->second];
    }
    const std::vector<LocalVariable>& Locals(std::size_t triggerIndex) {
        auto found = locals_.find(triggerIndex);
        if (found == locals_.end())
            found = locals_.emplace(triggerIndex, types.CollectLocals(document_, document_.triggers[triggerIndex])).first;
        return found->second;
    }
    void BeginPass() { ++pass_; }
    bool EcaSeen(std::size_t index) { const bool seen = ecaPass_[index] == pass_; ecaPass_[index] = pass_; return seen; }
    bool ArgumentSeen(std::size_t index) { const bool seen = argumentPass_[index] == pass_; argumentPass_[index] = pass_; return seen; }
    const TriggerDocument& document_;
    const TriggerTypeSystem types;
private:
    std::vector<Mount> ecas_, arguments_;
    std::vector<std::uint32_t> ecaPass_, argumentPass_;
    std::unordered_map<StableId, std::uint32_t> byId_;
    std::map<std::size_t, std::vector<LocalVariable>> locals_;
    std::uint32_t pass_ = 0;
};

Result<detail::ArgumentReplacementInspection> Inspect(const TriggerDocument& document,
    const UiCatalog& catalog, StableId argumentId, const TriggerGraphEditLimits& limits, bool allowRootIndex,
    const MapObjectReferenceSnapshot* mapObjects, PreparedInspection* prepared = nullptr) {
    using ResultT = Result<detail::ArgumentReplacementInspection>;
    if (limits.maxDepth == 0 || limits.maxDepth > 128)
        return Failed<detail::ArgumentReplacementInspection>(Fail("TRIGGER_GRAPH_BUDGET", "参数替换递归预算须为 1..128。"));
    if (!prepared) {
        if (const auto valid = WtgWctCodec{}.Validate(document, catalog); !valid.Succeeded())
            return Failed<detail::ArgumentReplacementInspection>(valid);
    }
    const auto location = prepared ? prepared->LocateArgument(argumentId) : Locate(document, argumentId);
    if (!location.Succeeded()) return Failed<detail::ArgumentReplacementInspection>(location);
    const auto& root = document.argumentNodes[location.value->argumentIndex];
    if (!allowRootIndex && (root.indexArgument || root.insertIndex != 0))
        return Failed<detail::ArgumentReplacementInspection>(Fail("TRIGGER_GRAPH_ARGUMENT_INDEX_UNSUPPORTED", "旧根参数带数组下标；本轮尚未开放该闭包的原位替换。"));
    const auto& owner = document.ecaNodes[location.value->ownerIndex];
    const auto& trigger = document.triggers[location.value->triggerIndex];
    std::optional<TriggerTypeSystem> singleTypes;
    if (!prepared) singleTypes.emplace(catalog, mapObjects);
    const TriggerTypeSystem& types = prepared ? prepared->types : *singleTypes;
    std::vector<LocalVariable> singleLocals;
    if (!prepared) singleLocals = types.CollectLocals(document, trigger);
    const auto& locals = prepared ? prepared->Locals(location.value->triggerIndex) : singleLocals;
    const auto checkDefinition = [&](const EcaNode& node) -> Status {
        const auto* definition = catalog.Find(node.type, node.name.text);
        if (!definition || node.uiSignature.empty() || node.uiSignature != definition->Signature())
            return Fail("TRIGGER_GRAPH_ARGUMENT_SIGNATURE_STALE", "参数所属函数的当前 Catalog 签名与密封签名不符。");
        if (CodeDefinition(*definition)) return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "自定义脚本或动态代码入口不允许参数子树替换。");
        return Status::Ok();
    };
    if (const auto status = checkDefinition(owner); !status.Succeeded()) return Failed<detail::ArgumentReplacementInspection>(status);
    for (const auto& [index, ordinal] : location.value->ancestorSlots) {
        const auto& ancestor = document.ecaNodes[index];
        if (const auto status = checkDefinition(ancestor); !status.Succeeded()) return Failed<detail::ArgumentReplacementInspection>(status);
        if (const auto status = CheckType(types, types.ExpectedType(document, ancestor, ordinal, locals)); !status.Succeeded())
            return Failed<detail::ArgumentReplacementInspection>(status);
    }
    detail::ArgumentReplacementInspection result;
    result.guard.ownerEcaId = owner.id; result.guard.sourceTriggerId = trigger.id;
    result.guard.argumentOrdinal = location.value->ordinal; result.guard.uiSignature = owner.uiSignature;
    result.guard.expectedType = types.ExpectedType(document, owner, location.value->ordinal, locals);
    if (const auto status = CheckType(types, result.guard.expectedType); !status.Succeeded())
        return Failed<detail::ArgumentReplacementInspection>(status);
    FingerprintWriter fingerprint;
    fingerprint.Add("forge.argument-subtree.v1"); fingerprint.Number(argumentId);
    fingerprint.Number(owner.id); fingerprint.Number(trigger.id); fingerprint.Number(location.value->ordinal);
    fingerprint.Add(owner.uiSignature); fingerprint.Add(catalog.Sha256()); fingerprint.Add(result.guard.expectedType);
    std::vector<bool> visitedEcas(prepared ? 0 : document.ecaNodes.size()),
        visitedArguments(prepared ? 0 : document.argumentNodes.size());
    if (prepared) prepared->BeginPass();
    const auto ecaSeen = [&](std::size_t index) {
        if (prepared) return prepared->EcaSeen(index);
        const bool seen = visitedEcas[index]; visitedEcas[index] = true; return seen;
    };
    const auto argumentSeen = [&](std::size_t index) {
        if (prepared) return prepared->ArgumentSeen(index);
        const bool seen = visitedArguments[index]; visitedArguments[index] = true; return seen;
    };
    std::size_t records = 0;
    const auto step = [&](std::size_t depth) -> Status {
        if (depth > limits.maxDepth || ++records > kMaxClosureRecords)
            return Fail("TRIGGER_GRAPH_BUDGET", "参数闭包超过节点或深度预算；没有限制整张地图的实例/节点总数。");
        return fingerprint.Check();
    };
    std::function<Status(std::uint32_t, std::string_view, std::size_t)> argument;
    std::function<Status(std::uint32_t, std::string_view, std::size_t)> call;
    call = [&](std::uint32_t index, std::string_view expected, std::size_t depth) -> Status {
        if (const auto status = step(depth); !status.Succeeded()) return status;
        if (index >= document.ecaNodes.size() || ecaSeen(index))
            return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "参数调用闭包越界、循环或存在共享节点。");
        const auto& node = document.ecaNodes[index];
        if (node.type != 3 || node.childId || !node.children.empty() || node.sourceTriggerId != trigger.id)
            return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "参数闭包只能包含有类型的 Call，不能删除代码动作、分支或其他触发器节点。");
        if (const auto status = checkDefinition(node); !status.Succeeded()) return status;
        const auto actual = types.ReturnType(document, node, locals);
        if (const auto status = CheckType(types, actual); !status.Succeeded()) return status;
        if (!types.Accepts(expected, actual)) return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "旧参数函数返回类型与当前槽位不符。");
        ++result.removedEcas;
        fingerprint.Add("eca"); fingerprint.Number(node.id); fingerprint.Number(node.sourceTriggerId);
        fingerprint.Number(node.type); fingerprint.Number(node.enabled); fingerprint.Number(node.childId.has_value());
        if (const auto status = fingerprint.Raw(node.name); !status.Succeeded()) return status;
        fingerprint.Add(node.uiSignature); fingerprint.Add(node.uiLayer);
        const auto* definition = catalog.Find(node.type, node.name.text);
        fingerprint.Add(definition->Signature()); fingerprint.Add(definition->Active().sourceSha256);
        fingerprint.Add(actual); fingerprint.Number(node.arguments.size()); fingerprint.Number(node.children.size());
        for (const auto child : node.arguments) fingerprint.Number(document.argumentNodes[child].id);
        for (std::size_t ordinal = 0; ordinal < node.arguments.size(); ++ordinal) {
            const auto nestedExpected = types.ExpectedType(document, node, ordinal, locals);
            if (const auto status = argument(node.arguments[ordinal], nestedExpected, depth + 1); !status.Succeeded()) return status;
        }
        return fingerprint.Check();
    };
    argument = [&](std::uint32_t index, std::string_view expected, std::size_t depth) -> Status {
        if (const auto status = step(depth); !status.Succeeded()) return status;
        if (index >= document.argumentNodes.size() || argumentSeen(index))
            return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "参数闭包越界、循环或存在共享节点。");
        if (const auto status = CheckType(types, expected); !status.Succeeded()) return status;
        const auto& value = document.argumentNodes[index];
        if (index != location.value->argumentIndex) ++result.removedArguments;
        fingerprint.Add("argument"); fingerprint.Number(value.id); fingerprint.Number(value.type);
        fingerprint.Number(value.insertCall); fingerprint.Number(value.insertIndex); fingerprint.Add(expected);
        fingerprint.Number(value.callEca.has_value()); fingerprint.Number(value.indexArgument.has_value());
        if (value.callEca) fingerprint.Number(document.ecaNodes[*value.callEca].id);
        if (value.indexArgument) fingerprint.Number(document.argumentNodes[*value.indexArgument].id);
        if (const auto status = fingerprint.Raw(value.value); !status.Succeeded()) return status;
        if (value.type == 2 && value.callEca && !value.indexArgument) {
            return call(*value.callEca, expected, depth + 1);
        }
        if (value.callEca) return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "参数值种类与 call 挂载不一致。");
        if (value.type == 0) {
            const auto preset = types.PresetType(value.value.text);
            if (value.indexArgument || !preset || !types.Accepts(expected, *preset))
                return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "旧参数预设缺失或类型不符。");
            if (const auto status = CheckType(types, *preset); !status.Succeeded()) return status;
        } else if (value.type == 1) {
            const auto reference = ResolveVariableReference(document, value.value.text, mapObjects);
            if (!reference.Succeeded()) return {reference.diagnostics};
            if (!types.Accepts(expected, reference.value->type) || reference.value->isArray != value.indexArgument.has_value())
                return Fail("TRIGGER_GRAPH_TYPE_MISMATCH", "旧参数变量的类型或数组资格不符。");
            // 仅实际引用预放对象时扩展摘要；普通变量/触发器的既有闭包摘要保持原格式。
            if (reference.value->kind == VariableReferenceKind::PreplacedObject) {
                fingerprint.Add("preplaced-object.v1");
                fingerprint.Add(reference.value->dependencySha256);
                fingerprint.Add(reference.value->type);
                fingerprint.Number(reference.value->isArray);
                fingerprint.Number(reference.value->writable);
            }
        } else if (value.type == 3) {
            if (value.indexArgument) return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "直接值不能带数组下标。");
            if (types.LiteralKind(expected) == TriggerTypeSystem::Literal::Other) {
                if (value.value.text != "null" || !types.Accepts("handle", expected))
                    return Fail("TRIGGER_GRAPH_CODE_UNSUPPORTED", "未知直接文本不能当作安全参数闭包删除。");
            } else if (const auto status = types.ValidateLiteral(expected, value.value.text); !status.Succeeded()) return status;
        } else return Fail("TRIGGER_GRAPH_ARGUMENT_CLOSURE_INVALID", "参数闭包含未知值种类。");
        if (value.indexArgument) return argument(*value.indexArgument, "integer", depth + 1);
        return fingerprint.Check();
    };
    if (const auto status = argument(location.value->argumentIndex, result.guard.expectedType, 0); !status.Succeeded())
        return Failed<detail::ArgumentReplacementInspection>(status);
    const auto hash = fingerprint.Finish();
    if (!hash.Succeeded()) return Failed<detail::ArgumentReplacementInspection>(hash);
    result.guard.subtreeSha256 = *hash.value;
    return ResultT::Success(std::move(result));
}
} // namespace

namespace detail {
Result<ArgumentReplacementInspection> InspectArgumentReplacement(const TriggerDocument& document,
    const UiCatalog& catalog, StableId argumentId, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects) {
    return Inspect(document, catalog, argumentId, limits, false, mapObjects);
}
Result<std::string> FingerprintArgumentSubtree(const TriggerDocument& document, const UiCatalog& catalog,
    StableId argumentId, const TriggerGraphEditLimits& limits, const MapObjectReferenceSnapshot* mapObjects) {
    const auto inspected = Inspect(document, catalog, argumentId, limits, true, mapObjects);
    if (!inspected.Succeeded()) return Failed<std::string>(inspected);
    return Result<std::string>::Success(inspected.value->guard.subtreeSha256);
}
} // namespace detail

Result<GraphArgumentReplacementGuard> CaptureArgumentReplacementGuard(const TriggerDocument& document,
    const UiCatalog& catalog, StableId argumentId, const TriggerGraphEditLimits& limits,
    const MapObjectReferenceSnapshot* mapObjects) {
    const auto inspected = detail::InspectArgumentReplacement(document, catalog, argumentId, limits, mapObjects);
    if (!inspected.Succeeded()) return Failed<GraphArgumentReplacementGuard>(inspected);
    return Result<GraphArgumentReplacementGuard>::Success(inspected.value->guard);
}
Result<std::vector<ScalarArgumentReplacementGuard>> CaptureScalarArgumentReplacementGuards(
    const TriggerDocument& document, const UiCatalog& catalog, std::span<const StableId> argumentIds,
    const TriggerGraphEditLimits& limits, const MapObjectReferenceSnapshot* mapObjects) {
    using Output = std::vector<ScalarArgumentReplacementGuard>;
    if (argumentIds.empty() || argumentIds.size() > std::min<std::size_t>(limits.maxOperations, 256)
        || limits.maxDepth == 0 || limits.maxDepth > 128
        || document.ecaNodes.size() >= kInvalidNodeIndex || document.argumentNodes.size() >= kInvalidNodeIndex)
        return Failed<Output>(Fail("TRIGGER_GRAPH_BUDGET", "批量标量请求为空或超过操作/深度/格式索引预算。"));
    std::set<StableId> unique;
    for (const auto id : argumentIds) if (!id || !unique.insert(id).second)
        return Failed<Output>(Fail("TRIGGER_GRAPH_ARGUMENT_TARGET_INVALID", "批量标量请求需要唯一非零参数身份。"));
    if (const auto valid = WtgWctCodec{}.Validate(document, catalog); !valid.Succeeded()) return Failed<Output>(valid);
    PreparedInspection prepared(document, catalog, mapObjects);
    if (const auto initialized = prepared.Initialize(); !initialized.Succeeded()) return Failed<Output>(initialized);
    Output output; output.reserve(argumentIds.size());
    for (const auto id : argumentIds) {
        ScalarArgumentReplacementGuard row; row.argumentId = id;
        const auto* argument = prepared.Argument(id);
        if (argument && (argument->callEca || argument->indexArgument
            || argument->insertCall != 0 || argument->insertIndex != 0)) {
            row.diagnostics = Fail("TRIGGER_GRAPH_ARGUMENT_COMPLEX", "批量标量准备不替换函数或数组下标闭包。").diagnostics;
        } else {
            const auto inspected = Inspect(document, catalog, id, limits, false, mapObjects, &prepared);
            if (inspected.Succeeded()) row.guard = inspected.value->guard;
            else row.diagnostics = inspected.diagnostics;
        }
        output.push_back(std::move(row));
    }
    return Result<Output>::Success(std::move(output));
}
} // namespace War3Map::Trigger
