#include "GameNativeProvider.h"
#include "NativeCallX86.h"
#include "GameCodeBackend.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace Audastra::Runtime32::OwnLua {
namespace {
NativeCallResult Failure(std::string message) { return {false, {}, std::move(message)}; }
bool IdentityConverter(std::uintptr_t function) noexcept {
    // 1.27a 实际 Convert* 整数包装器的完整8字节函数；没有对象持有／分配。
    constexpr unsigned char expected[]{0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x5d, 0xc3};
    __try { return function && std::memcmp(reinterpret_cast<const void*>(function), expected, sizeof(expected)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
class HandleOwner final : public NativeHandleOwner {
public:
    HandleOwner(std::shared_ptr<GameVm127> vm, std::uint32_t id, std::uintptr_t object)
        : vm_(std::move(vm)), id_(id), object_(object) {}
    ~HandleOwner() override { if (object_) vm_->ReleaseHandle(id_, object_); }
    bool Alive() const noexcept override { return object_ ? vm_->HandleAlive(id_, object_) : vm_->Alive(); }
private:
    std::shared_ptr<GameVm127> vm_;
    std::uint32_t id_;
    std::uintptr_t object_;
};
struct Arguments {
    const GameVm127& vm;
    std::array<std::uint32_t, 32> words{};
    std::array<float, 32> reals{};
    std::array<std::uint32_t, 32> strings{};
    ~Arguments() { for (const auto id : strings) if (id) vm.ReleaseString(id); }
};
bool Unsupported(std::string_view name) {
    // coroutine 与另开地图/任意文件输入输出未接入此受限宿主。未知能力明确拒绝。
    constexpr std::array names{
        "TriggerSleepAction", "TriggerWaitForSound", "TriggerExecuteWait", "TriggerSyncReady", "SyncSelections",
        "Preloader", "Preload", "PreloadEnd", "PreloadEndEx", "PreloadGenStart", "PreloadGenClear", "PreloadGenEnd",
        "PreloadStart", "PreloadRefresh", "EndGame", "ChangeLevel", "RestartGame", "ReloadGame",
        "SaveGame", "LoadGame", "RenameSaveDirectory", "RemoveSaveDirectory", "CopySaveGame", "SaveGameExists",
        "SaveGameCache", "InitGameCache", "ReloadGameCachesFromDisk"};
    for (const auto value : names) if (name == value) return true;
    return false;
}
}

GameNativeProvider::GameNativeProvider(std::shared_ptr<GameVm127> vm, std::shared_ptr<const NativeRegistry> registry,
    std::shared_ptr<const NativeTypeHierarchy> types, bool codeDispatchReady) : vm_(std::move(vm)), registry_(std::move(registry)), types_(std::move(types)) {
    if (!Generation()) return;
    if (codeDispatchReady) codes_ = std::make_unique<NativeCodeRegistry>(std::make_shared<GameCodeBackend>(vm_));
    for (const auto& [name, entry] : registry_->Entries()) {
        if (name.starts_with("Convert") && entry.signature.arguments.size() == 1
            && entry.signature.arguments[0].kind == NativeKind::Integer && entry.signature.returns.kind == NativeKind::Handle
            && !types_->IsA(entry.signature.returns.handleType, "agent") && IdentityConverter(entry.function))
            scalarTypes_.insert(entry.signature.returns.handleType);
    }
}
std::uint64_t GameNativeProvider::Generation() const noexcept {
    return vm_ && registry_ && registry_->Valid() && types_ ? vm_->Generation() : 0;
}
std::optional<NativeSignature> GameNativeProvider::Lookup(std::string_view name) const {
    if (!Generation()) return {};
    const auto* entry = registry_->Find(name);
    return entry ? std::optional<NativeSignature>(entry->signature) : std::nullopt;
}
bool GameNativeProvider::IsHandleType(std::string_view actual, std::string_view expected) const noexcept {
    return types_ && types_->IsA(actual, expected);
}
std::optional<NativeCallbackReturn> GameNativeProvider::CallbackReturn(std::string_view name, std::size_t argument) const {
    if (!Generation() || !codes_) return {};
    if ((name == "Condition" || name == "Filter") && argument == 0) return NativeCallbackReturn::Boolean;
    if ((name == "TimerStart" && argument == 3) || (name == "TriggerAddAction" && argument == 1)
        || ((name == "ForGroup" || name == "ForForce") && argument == 1)
        || ((name == "EnumDestructablesInRect" || name == "EnumItemsInRect") && argument == 2)) return NativeCallbackReturn::Nothing;
    return {};
}
std::optional<NativeCallbackResult> GameNativeProvider::DispatchCode(std::uint32_t vmIndex, std::uint32_t code, bool parameters) {
    if (!codes_) return {};
    auto result = codes_->Dispatch(vmIndex, code, parameters);
    if (result && !result->succeeded) { ++callbackFailures_; callbackError_ = result->error; }
    return result;
}
void GameNativeProvider::BeforeVmDestroy(std::uintptr_t vm) noexcept {
    if (vm_ && vm_->IsMainVm(vm)) { vm_->Invalidate(); codes_.reset(); }
}
NativeCallResult GameNativeProvider::Invoke(std::string_view name, std::span<const NativeValue> supplied) {
    if (!Generation()) return Failure("游戏 native 提供者所属线程、地图代际或来源已经失效。");
    if (Unsupported(name)) return Failure("自有 Lua 运行时尚不支持此 native 的等待、地图切换或文件访问行为。");
    const auto* entry = registry_->Find(name);
    if (!entry || entry->signature.arguments.size() != supplied.size() || supplied.size() > 32)
        return Failure("实际 native 登记缺失或参数数量不符。");
    const auto& signature = entry->signature;
    if (codes_) codes_->Sweep();
    if (signature.returns.kind == NativeKind::Code) return Failure("native code 返回值尚未建立生命周期桥。");
    // TriggerAddAction 返回的声明类型虽是 handle，已核对 native 仍通过真实 agent→slot 路径返回。
    const bool actionObject = signature.returns.kind == NativeKind::Handle && signature.returns.handleType == "triggeraction";
    if (signature.returns.kind == NativeKind::Handle && !IsHandleType(signature.returns.handleType, "agent")
        && !actionObject && !scalarTypes_.contains(signature.returns.handleType))
        return Failure("此非 agent 对象尚未建立销毁与句柄复用的生命周期桥，未调用 native。");
    Arguments args{*vm_};
    std::shared_ptr<NativeCodeLease> callbackLease;
    std::optional<NativeCodeOwner> timerOwner;
    if (name == "TimerStart") {
        const auto* timer = supplied.empty() ? nullptr : std::get_if<NativeHandle>(&supplied[0]);
        if (!timer || !(timerOwner = vm_->CodeOwner(timer->value))) return Failure("TimerStart 需要当前地图内有效的 timer 对象。");
    }
    for (std::size_t index = 0; index < supplied.size(); ++index) {
        const auto& expected = signature.arguments[index];
        const auto& value = supplied[index];
        switch (expected.kind) {
        case NativeKind::Integer:
            if (!std::holds_alternative<std::int32_t>(value)) return Failure("native integer 参数类型不符。");
            args.words[index] = static_cast<std::uint32_t>(std::get<std::int32_t>(value)); break;
        case NativeKind::Real:
            if (!std::holds_alternative<float>(value) || !std::isfinite(std::get<float>(value))) return Failure("native real 参数不是有限 float32。");
            args.reals[index] = std::get<float>(value);
            args.words[index] = reinterpret_cast<std::uint32_t>(&args.reals[index]); break;
        case NativeKind::Boolean:
            if (!std::holds_alternative<bool>(value)) return Failure("native boolean 参数类型不符。");
            args.words[index] = std::get<bool>(value) ? 1 : 0; break;
        case NativeKind::String: {
            if (std::holds_alternative<std::monostate>(value)) break;
            if (!std::holds_alternative<std::string>(value)) return Failure("native string 参数类型不符。");
            const auto interned = vm_->InternString(std::get<std::string>(value));
            if (!interned) return Failure("无法在当前 VM 中登记有界 UTF-8 字符串。");
            args.strings[index] = *interned;
            const auto object = vm_->StringObject(*interned);
            if (!object) return Failure("当前 VM 字符串对象无效。");
            args.words[index] = static_cast<std::uint32_t>(object); break;
        }
        case NativeKind::Handle: {
            if (std::holds_alternative<std::monostate>(value)) break;
            const auto* handle = std::get_if<NativeHandle>(&value);
            if (!handle || handle->generation != Generation() || !handle->owner || !handle->owner->Alive()
                || !IsHandleType(handle->type, expected.handleType)) return Failure("native handle 不属于当前有效地图或类型不符。");
            args.words[index] = handle->value; break;
        }
        case NativeKind::Code: {
            const auto returns = CallbackReturn(name, index);
            if (!returns) return Failure("此 native 的 code 消费方式尚未核对，未调用 native。");
            if (std::holds_alternative<std::monostate>(value)) break;
            const auto* callback = std::get_if<std::shared_ptr<NativeCallback>>(&value);
            if (!callback || !*callback || (*callback)->Returns() != *returns || callbackLease)
                return Failure("native code 回调为空、返回类型不符或消费者数量未知。");
            auto created = codes_->Create(*callback);
            if (!created.lease) return Failure(std::move(created.error));
            callbackLease = std::move(created.lease); args.words[index] = callbackLease->Id();
            break;
        }
        case NativeKind::Nothing: return Failure("native 参数签名包含无效 nothing。");
        }
    }
    const auto failureBefore = callbackFailures_;
    const auto call = InvokeNativeX86(entry->function, std::span(args.words).first(supplied.size()));
    if (!call.succeeded) {
        vm_->Invalidate();
        return Failure("游戏 native 触发异常，会话已停止：" + std::to_string(call.exception));
    }
    ++invocationCount_;
    if (!vm_->Alive()) return Failure("native 返回时地图 VM 已经结束或变化。");
    if (callbackLease && (timerOwner || name == "Condition" || name == "Filter" || name == "TriggerAddAction")) {
        const auto owner = timerOwner ? timerOwner : vm_->CodeOwner(call.value);
        if (!owner || !codes_->RetainFor(*owner, callbackLease)) {
            // native 已收到该 code ID；不能释放后让游戏读到悬空 token。
            vm_->Invalidate();
            return Failure("native 已执行但无法核对回调对象生命期，运行时已停止，必须重新测试。");
        }
    } else if (timerOwner && !callbackLease) codes_->Forget(*timerOwner);
    if (codes_) codes_->Sweep();
    if (callbackFailures_ != failureBefore) return Failure(callbackError_);
    switch (signature.returns.kind) {
    case NativeKind::Nothing: return {true, {}, {}};
    case NativeKind::Integer: return {true, std::bit_cast<std::int32_t>(call.value), {}};
    case NativeKind::Real: {
        const auto real = std::bit_cast<float>(call.value);
        return std::isfinite(real) ? NativeCallResult{true, real, {}} : Failure("游戏 native 返回非有限 real。");
    }
    case NativeKind::Boolean: return {true, call.value != 0, {}};
    case NativeKind::String: {
        if (!call.value) return {true, {}, {}};
        const auto text = vm_->ReadString(call.value);
        vm_->ReleaseString(call.value);
        return text ? NativeCallResult{true, *text, {}} : Failure("游戏 native 返回无效或超预算字符串。");
    }
    case NativeKind::Handle: {
        const auto& type = signature.returns.handleType;
        if (!IsHandleType(type, type) || type == "handle") return Failure("native 返回的 handle 缺少可信具体类型。");
        const bool reference = IsHandleType(type, "agent") || actionObject;
        if (reference && !call.value) return {true, {}, {}};
        const auto object = reference ? vm_->HandleObject(call.value) : 0;
        if (reference && (!object || !vm_->RetainHandle(call.value))) return Failure("无法持有游戏 native 返回的有效 agent 引用。");
        std::shared_ptr<NativeHandleOwner> owner;
        try { owner = std::make_shared<HandleOwner>(vm_, call.value, object); }
        catch (...) { if (reference) vm_->ReleaseHandle(call.value, object); throw; }
        return {true, NativeHandle{call.value, Generation(), type, std::move(owner),
            reference ? NativeHandleKind::Reference : NativeHandleKind::Scalar}, {}};
    }
    case NativeKind::Code: return Failure("native code 返回值尚未接通。");
    }
    return Failure("native 返回类型未知。");
}

} // namespace Audastra::Runtime32::OwnLua
