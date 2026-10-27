#include "GameUiDataPlane.h"
#include "GameUiMemory.h"
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
    __try {
        const auto linked = object ? *reinterpret_cast<const Address*>(object + 8) : 0;
        const auto* text = linked ? *reinterpret_cast<const char* const*>(linked + 0x1c) : nullptr;
        if (!text) return false;
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

void GameUiDataPlane::Attach(GameUiLibrary& ui) noexcept { if (!stopped_ && !ui_) ui_ = &ui; }
void GameUiDataPlane::Stop() noexcept {
    stopped_ = true;
    ui_ = nullptr;
    table_ = 0;
    tableObject_ = 0;
    tableKeyA_ = 0; tableKeyB_ = 0;
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

UiDataWrite GameUiDataPlane::SaveReal(std::int32_t table, FrameHandle frame, std::int32_t property, const float* value) noexcept {
    if (!Owns(table)) return {};
    if (!Begin(table)) return {true, false};
    if (property != 1 && property != 2) return Reject(Error::Property);
    Address bits = 0;
    if (!Detail::ReadWord(reinterpret_cast<Address>(value), bits) || !std::isfinite(std::bit_cast<float>(bits))) {
        return Reject(Error::Value);
    }
    try { return Finish(property == 1 ? ui_->SetValue(frame, std::bit_cast<float>(bits)) : ui_->SetWidth(frame, std::bit_cast<float>(bits))); }
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
    if (property != 5) return Reject(Error::Property);
    if (value != 0 && value != 1) return Reject(Error::Value);
    try { return Finish(ui_->SetVisible(frame, value != 0)); }
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
    }
    return std::to_string(errors_) + '|' + std::string(reason);
}
} // namespace Audastra::Runtime32::GameUi
