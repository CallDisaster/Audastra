#include "GameUiDataPlane.h"
#include "GameUiMemory.h"
#include "GameUiStatus.h"
#include "aj/Protocol.h"
#include <array>
#include <bit>
#include <cmath>
#include <windows.h>

namespace Audastra::Runtime32::GameUi {
namespace {
int Capture(EXCEPTION_POINTERS* information, UiFault& fault) noexcept {
    if (information && information->ExceptionRecord) {
        fault.code = information->ExceptionRecord->ExceptionCode;
        fault.address = reinterpret_cast<Address>(information->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
template<class Call> bool Guard(Call& call, UiFault& fault) noexcept {
    __try { call(); return true; }
    __except (Capture(GetExceptionInformation(), fault)) { return false; }
}
bool CopyString(Address object, std::array<char, AJ_MAX_INPUT_TEXT + 1>& bytes, std::size_t& size) noexcept {
    size = 0;
    if (!object) { bytes[0] = 0; return true; }
    __try {
        const auto linked = object ? *reinterpret_cast<const Address*>(object + 8) : 0;
        const auto* text = linked ? *reinterpret_cast<const char* const*>(linked + 0x1c) : nullptr;
        // 已读取的中间描述仍存在、字符缓冲为空，是 VM 的空字符串表示。
        // 未连接或不可读的引用仍拒绝，不能把损坏指针当作空文字。
        if (!text) { if (!linked) return false; bytes[0] = 0; return true; }
        std::size_t encoded = 0;
        for (; size < bytes.size(); ++size) {
            const auto c = text[size];
            bytes[size] = c;
            if (!c) return true;
            encoded += c == '%' || c == ';' ? 3 : 1;
            if (encoded > AJ_MAX_INPUT_TEXT) return false;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

GameUiDataPlane::GameUiDataPlane(GameUiDataContext context, UiFaultObserver observer)
    : context_(std::move(context)), observer_(observer), thread_(GetCurrentThreadId()) {}

void GameUiDataPlane::Attach(GameUiLibrary& ui, EventPump events) {
    if (!stopped_ && !ui_) { ui_ = &ui; eventPump_ = std::move(events); }
}
void GameUiDataPlane::Stop() noexcept {
    if (stopped_) return;
    if (context_.boundTable && table_) context_.boundTable->store(0, std::memory_order_release);
    stopped_ = true;
    ui_ = nullptr;
    table_ = 0;
    tableObject_ = 0;
    tableKeyA_ = 0; tableKeyB_ = 0;
    formats_.clear(); eventPump_ = {}; eventsEnabled_ = false;
}
bool GameUiDataPlane::Owns(std::int32_t table) const noexcept { return !stopped_ && table_ && table == table_; }

bool GameUiDataPlane::ResolveTable(std::int32_t handle, Address& out, const char* entry, std::uint32_t request) noexcept {
    UiFault fault{};
    fault.epoch = epoch_; fault.request = request; fault.entry = entry;
    auto invoke = [&] { out = context_.resolveTable(static_cast<std::uint32_t>(handle)); };
    if (Guard(invoke, fault)) return true;
    faulted_ = true;
    if (observer_) observer_(fault);
    return false;
}

std::int32_t GameUiDataPlane::Bind(std::int32_t table, std::uint32_t epoch, std::uint32_t request) noexcept {
    bindReason_ = {};
    const auto fail = [&](std::int32_t status, std::string_view reason) { bindReason_ = reason; return status; };
    if (GetCurrentThreadId() != thread_) return fail(AJ_THREAD, "界面数据面只能在游戏线程绑定");
    if (stopped_) return fail(AJ_STALE, "界面数据面会话已结束");
    if (!context_.resolveTable || !context_.currentVm) return fail(AJ_UNSUPPORTED, "此运行库没有已核验的界面数据面能力");
    if (faulted_) return fail(AJ_INTERNAL, "界面数据面发生过原版异常，此会话不再绑定");
    if (inFlight_) return fail(AJ_REENTRANT, "界面数据面正在写入，拒绝重入绑定");
    inFlight_ = true;
    struct Clear { bool& value; ~Clear() { value = false; } } clear{inFlight_};
    try {
        if (!ui_ || !epoch) return fail(AJ_STALE, "界面数据面尚未附着到有效会话");
        if (!context_.currentVm()) { Stop(); return fail(AJ_STALE, "界面数据面 VM 身份已失效"); }
        if (table <= 0) return fail(AJ_ARGUMENT, "私有表句柄为空或无效");
        if (epoch_ && epoch_ != epoch) return fail(AJ_STALE, "界面数据面代际不匹配");
        epoch_ = epoch;
        Address object = 0;
        if (!ResolveTable(table, object, "audastraui.bindtable", request)) return fail(AJ_INTERNAL, "原版表句柄解析异常");
        Address keyA = 0, keyB = 0;
        if (!object || !Detail::ReadWord(object + 12, keyA) || !Detail::ReadWord(object + 16, keyB))
            return fail(AJ_ARGUMENT, "私有表句柄不是有效 Hashtable");
        if (stopped_ || !ui_) return fail(AJ_STALE, "界面数据面会话已结束");
        if (table_ && (table_ != table || tableObject_ != object || tableKeyA_ != keyA || tableKeyB_ != keyB))
            return fail(AJ_STATE, "此会话已绑定另一张私有表");
        table_ = table; tableObject_ = object; tableKeyA_ = keyA; tableKeyB_ = keyB;
        if (context_.boundTable) context_.boundTable->store(table, std::memory_order_release);
        return AJ_OK;
    } catch (...) { return fail(AJ_INTERNAL, "界面数据面绑定失败"); }
}

void GameUiDataPlane::Record(Error error, UiError ui) noexcept {
    if (errors_ < INT32_MAX) ++errors_;
    last_ = error; lastUi_ = ui;
}
bool GameUiDataPlane::Begin(std::int32_t table) noexcept {
    if (GetCurrentThreadId() != thread_) { Record(Error::Thread); return false; }
    if (inFlight_) { Record(Error::Reentrant); return false; }
    if (faulted_) { Record(Error::Internal); return false; }
    inFlight_ = true;
    try {
        if (!ui_ || !context_.currentVm()) { Record(Error::Session); Stop(); inFlight_ = false; return false; }
        Address object = 0;
        if (!ResolveTable(table, object, "audastraui.datawrite")) { Record(Error::Internal); inFlight_ = false; return false; }
        Address keyA = 0, keyB = 0;
        if (!object || object != tableObject_ || !Detail::ReadWord(object + 12, keyA) || !Detail::ReadWord(object + 16, keyB)
            || keyA != tableKeyA_ || keyB != tableKeyB_) { Record(Error::Table); inFlight_ = false; return false; }
        if (stopped_ || !ui_) { Record(Error::Session); inFlight_ = false; return false; }
    } catch (...) { Record(Error::Internal); inFlight_ = false; return false; }
    ui_->SetDiagnosticContext(epoch_, 0, "audastraui.datawrite");
    return true;
}
UiDataWrite GameUiDataPlane::Finish(UiError error) noexcept {
    inFlight_ = false;
    if (ui_) ui_->SetDiagnosticContext(0, 0, nullptr);
    if (stopped_) { Record(Error::Session); return {true, false}; }
    if (error != UiError::None) Record(Error::Ui, error);
    return {true, error == UiError::None};
}
UiDataWrite GameUiDataPlane::Reject(Error error) noexcept {
    Record(error);
    static_cast<void>(Finish(UiError::None));
    return {true, false};
}

std::int32_t GameUiDataPlane::BeginControl() noexcept {
    controlReason_ = {};
    if (GetCurrentThreadId() != thread_) { controlReason_ = "界面数据控制只能在游戏线程调用"; return AJ_THREAD; }
    if (stopped_) { controlReason_ = "界面数据会话已结束"; return AJ_STALE; }
    if (!table_ || !ui_) { controlReason_ = "界面私有表尚未绑定"; return AJ_UNSUPPORTED; }
    if (inFlight_) { controlReason_ = "界面数据正在写入，拒绝重入控制"; return AJ_REENTRANT; }
    if (!Begin(table_)) {
        controlReason_ = "界面私有表或VM会话不可用";
        return faulted_ ? AJ_INTERNAL : stopped_ ? AJ_STALE : AJ_STATE;
    }
    return AJ_OK;
}

std::int32_t GameUiDataPlane::RegisterTextFormat(FrameHandle frame, std::string_view format) noexcept {
    const auto ready = BeginControl();
    if (ready != AJ_OK) return ready;
    const auto finish = [&](std::int32_t status, std::string_view reason = {}) {
        controlReason_ = reason;
        (void)Finish(UiError::None);
        return status;
    };
    try {
        const auto frameStatus = ui_->TextStatus(frame);
        if (frameStatus != UiError::None) return finish(StatusOf(frameStatus), UiErrorText(frameStatus));
        auto compiled = CompiledTextFormat::Parse(format);
        if (!compiled) return finish(AJ_ARGUMENT, "文字格式无效或超过384编码字节预算");
        // 清掉本库销毁的框架（含子树）的缓存，不让重建界面积累历史格式。
        for (auto it = formats_.begin(); it != formats_.end();) {
            if (ui_->TextStatus(it->first) != UiError::None) it = formats_.erase(it);
            else ++it;
        }
        if (!formats_.contains(frame) && formats_.size() >= kMaxFormats)
            return finish(AJ_LIMIT, "本会话格式化文字框架超过1024个");
        FormattedFrame state{std::move(*compiled)};
        formats_.insert_or_assign(frame, std::move(state));
        return finish(AJ_OK);
    } catch (...) { return finish(AJ_INTERNAL, "文字格式注册失败"); }
}

std::int32_t GameUiDataPlane::EnableEventTable() noexcept {
    const auto ready = BeginControl();
    if (ready != AJ_OK) return ready;
    const bool supported = context_.storeInteger && eventPump_;
    if (supported) eventsEnabled_ = true;
    else controlReason_ = "当前运行库没有可核验的整数事件表能力";
    (void)Finish(UiError::None);
    return supported ? AJ_OK : AJ_UNSUPPORTED;
}

UiDataWrite GameUiDataPlane::AssignArgument(FrameHandle frame, int index, TextFormatArgument argument) {
    const auto status = ui_->TextStatus(frame);
    if (status != UiError::None) { formats_.erase(frame); return Finish(status); }
    const auto found = formats_.find(frame);
    if (found == formats_.end()) return Reject(Error::Format);
    found->second.arguments[static_cast<std::size_t>(index)] = argument;
    if (context_.observeTextArgument && observedArguments_ < 512) {
        const auto ordinal = ++observedArguments_;
        try {
            context_.observeTextArgument(ordinal, frame, index, argument.integer,
                argument.integer ? std::bit_cast<std::uint32_t>(argument.value) : std::bit_cast<std::uint32_t>(argument.real));
        } catch (...) {} // 观察失败不能把已接受的参数写入改成失败。
    }
    return Finish(UiError::None);
}

UiDataWrite GameUiDataPlane::CommitText(FrameHandle frame) {
    const auto status = ui_->TextStatus(frame);
    if (status != UiError::None) { formats_.erase(frame); return Finish(status); }
    const auto found = formats_.find(frame);
    if (found == formats_.end()) return Reject(Error::Format);
    auto& state = found->second;
    auto text = state.format.Render(state.arguments);
    if (!text) return Reject(Error::Format);
    // 每次显式提交都恢复完整结果，保留与其他SetText调用的先后顺序。
    const auto statusWritten = ui_->SetText(frame, *text);
    if (statusWritten == UiError::None && !stopped_ && context_.observeTextCommit && observedCommits_ < 512) {
        const auto ordinal = ++observedCommits_;
        try { context_.observeTextCommit(ordinal, frame, *text); } catch (...) {}
    }
    return Finish(statusWritten);
}

bool GameUiDataPlane::StoreEventInteger(std::int32_t child, std::int32_t value) noexcept {
    if (stopped_ || !context_.storeInteger) return false;
    UiFault fault{}; fault.epoch = epoch_; fault.entry = "audastraui.eventwrite";
    auto invoke = [&] { context_.storeInteger(table_, 0, child, value); };
    if (!Guard(invoke, fault)) {
        faulted_ = true;
        if (observer_) observer_(fault);
        return false;
    }
    return !stopped_;
}

UiDataWrite GameUiDataPlane::PollEvent(bool refresh) {
    if (!eventsEnabled_ || !eventPump_ || !context_.storeInteger) return Reject(Error::Property);
    // 地图先FlushChildHashtable(table,0)。完成标记最后写；故障时不重放旧事件。
    if (!StoreEventInteger(0, 0)) return Reject(Error::Internal);
    UiFrameEvent event{}; bool found = false;
    const auto pump = eventPump_; // Stop可能在同步回调内清空成员，当前调用仍持有自己的闭包。
    const auto status = pump(refresh, event, found);
    if (stopped_) return Finish(UiError::None);
    if (status != UiError::None) return Finish(status);
    if (found) {
        const auto frameStatus = ui_->FrameStatus(event.frame);
        if (frameStatus != UiError::None) return Finish(frameStatus);
        if (event.kind != kUiEventEnter && event.kind != kUiEventLeave && event.kind != kUiEventClick) return Reject(Error::Value);
    }
    if (found && (!StoreEventInteger(1, event.frame) || !StoreEventInteger(2, event.kind) || !StoreEventInteger(0, 1)))
        return Reject(Error::Internal);
    return Finish(UiError::None);
}

UiDataWrite GameUiDataPlane::SaveInteger(std::int32_t table, FrameHandle frame, std::int32_t property, std::int32_t value) noexcept {
    if (!Owns(table)) return {};
    if (!Begin(table)) return {true, false};
    if (property < 24 || property > 31) return Reject(Error::Property);
    try { return AssignArgument(frame, property - 24, {true, true, 0, value}); }
    catch (...) { return Reject(Error::Internal); }
}

UiDataWrite GameUiDataPlane::SaveReal(std::int32_t table, FrameHandle frame, std::int32_t property, const float* value) noexcept {
    if (!Owns(table)) return {};
    if (!Begin(table)) return {true, false};
    if (property != 1 && property != 2 && (property < 16 || property > 23)) return Reject(Error::Property);
    Address bits = 0;
    if (!Detail::ReadWord(reinterpret_cast<Address>(value), bits) || !std::isfinite(std::bit_cast<float>(bits))) {
        return Reject(Error::Value);
    }
    try {
        if (property >= 16) return AssignArgument(frame, property - 16, {true, false, std::bit_cast<float>(bits), 0});
        return Finish(property == 1 ? ui_->SetValue(frame, std::bit_cast<float>(bits)) : ui_->SetWidth(frame, std::bit_cast<float>(bits)));
    }
    catch (...) { return Reject(Error::Internal); }
}
UiDataWrite GameUiDataPlane::SaveStr(std::int32_t table, FrameHandle frame, std::int32_t property, Address object) noexcept {
    if (!Owns(table)) return {};
    if (!Begin(table)) return {true, false};
    if (property != 3 && property != 4) return Reject(Error::Property);
    std::array<char, AJ_MAX_INPUT_TEXT + 1> bytes{};
    std::size_t size = 0;
    if (!CopyString(object, bytes, size) || !audastra::japi::ValidText({bytes.data(), size})) {
        return Reject(Error::Value);
    }
    try { return Finish(property == 3 ? ui_->SetText(frame, {bytes.data(), size}) : ui_->SetTexture(frame, {bytes.data(), size}, false)); }
    catch (...) { return Reject(Error::Internal); }
}
UiDataWrite GameUiDataPlane::SaveBoolean(std::int32_t table, FrameHandle frame, std::int32_t property, std::int32_t value) noexcept {
    if (!Owns(table)) return {};
    if (!Begin(table)) return {true, false};
    if (property < 5 || property > 8) return Reject(Error::Property);
    if ((value != 0 && value != 1) || (property != 5 && value != 1)) return Reject(Error::Value);
    try {
        if (property == 6) return CommitText(frame);
        if (property >= 7) return frame == 0 ? PollEvent(property == 7) : Reject(Error::Property);
        return Finish(ui_->SetVisible(frame, value != 0));
    }
    catch (...) { return Reject(Error::Internal); }
}

std::string GameUiDataPlane::Errors() const {
    std::string_view reason;
    switch (last_) {
    case Error::None: break;
    case Error::Thread: reason = "界面数据面写入线程错误"; break;
    case Error::Session: reason = "界面数据面 VM 会话已失效"; break;
    case Error::Table: reason = "界面数据面私有表已失效或身份变化"; break;
    case Error::Reentrant: reason = "界面数据面拒绝重入写入"; break;
    case Error::Property: reason = "界面数据面属性码与 native 类型不匹配"; break;
    case Error::Value: reason = "界面数据面值无效或超过长度限制"; break;
    case Error::Internal: reason = "界面数据面内部异常"; break;
    case Error::Ui: reason = UiErrorText(lastUi_); break;
    case Error::Format: reason = "文字格式未注册、参数未齐全或格式化输出超限"; break;
    }
    return std::to_string(errors_) + '|' + std::string(reason);
}
} // namespace Audastra::Runtime32::GameUi
