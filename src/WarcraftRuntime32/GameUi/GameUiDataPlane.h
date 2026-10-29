#pragma once

#include "GameUiLibrary.h"
#include "GameUiTextFormat.h"
#include <atomic>
#include <array>
#include <functional>
#include <string>
#include <unordered_map>

namespace Audastra::Runtime32::GameUi {

// 固定版本原版 Hashtable 类型解析器，含类型与弱身份检查；参数是 JASS 句柄。
using FnResolveUiTable = Address(__fastcall*)(std::uint32_t handle);
struct GameUiDataContext {
    FnResolveUiTable resolveTable = nullptr;
    std::function<bool()> currentVm;
    // 仅为原版 native 的无锁否定快路；命中后仍在宿主锁内完整校验。
    std::atomic<std::int32_t>* boundTable = nullptr;
    // 只调用已核验原版入口的 trampoline；输出固定表槽，不创建 JASS 字符串。
    std::function<void(std::int32_t, std::int32_t, std::int32_t, std::int32_t)> storeInteger;
    // 仅测试宿主设置。观察本方参数和已提交结果，不读取引擎文字表，不影响写入结果。
    std::function<void(std::uint32_t, FrameHandle, int, bool, std::uint32_t)> observeTextArgument;
    std::function<void(std::uint32_t, FrameHandle, std::string_view)> observeTextCommit;
};
struct UiDataWrite {
    bool intercepted = false;
    bool applied = false;
};

// 一个实例只属于一个 Host 会话。宿主串行调用；普通表不解引用值，也不产生诊断。
// 私有表是命令通道，成功写入不保存副本，不承诺 Load* 可读到属性值。
class GameUiDataPlane {
public:
    using EventPump = std::function<UiError(bool refresh, UiFrameEvent& event, bool& found)>;
    explicit GameUiDataPlane(GameUiDataContext context, UiFaultObserver observer = nullptr);
    void Attach(GameUiLibrary& ui, EventPump events = {});
    void Stop() noexcept;
    [[nodiscard]] std::int32_t Bind(std::int32_t table, std::uint32_t epoch, std::uint32_t request) noexcept;
    [[nodiscard]] std::string_view BindReason() const noexcept { return bindReason_; }
    [[nodiscard]] bool Owns(std::int32_t table) const noexcept;
    [[nodiscard]] std::int32_t RegisterTextFormat(FrameHandle frame, std::string_view format) noexcept;
    [[nodiscard]] std::int32_t EnableEventTable() noexcept;
    [[nodiscard]] std::string_view ControlReason() const noexcept { return controlReason_; }
    [[nodiscard]] UiDataWrite SaveInteger(std::int32_t table, FrameHandle frame, std::int32_t property, std::int32_t value) noexcept;
    [[nodiscard]] UiDataWrite SaveReal(std::int32_t table, FrameHandle frame, std::int32_t property, const float* value) noexcept;
    [[nodiscard]] UiDataWrite SaveStr(std::int32_t table, FrameHandle frame, std::int32_t property, Address stringObject) noexcept;
    [[nodiscard]] UiDataWrite SaveBoolean(std::int32_t table, FrameHandle frame, std::int32_t property, std::int32_t value) noexcept;
    [[nodiscard]] std::string Errors() const;

private:
    enum class Error { None, Thread, Session, Table, Reentrant, Property, Value, Internal, Ui, Format };
    struct FormattedFrame {
        CompiledTextFormat format;
        std::array<TextFormatArgument, 8> arguments{};
    };
    static constexpr std::size_t kMaxFormats = 1024;
    [[nodiscard]] std::int32_t BeginControl() noexcept;
    [[nodiscard]] bool StoreEventInteger(std::int32_t child, std::int32_t value) noexcept;
    [[nodiscard]] UiDataWrite PollEvent(bool refresh);
    [[nodiscard]] UiDataWrite CommitText(FrameHandle frame);
    [[nodiscard]] UiDataWrite AssignArgument(FrameHandle frame, int index, TextFormatArgument argument);
    [[nodiscard]] bool Begin(std::int32_t table) noexcept;
    void Record(Error error, UiError ui = UiError::None) noexcept;
    [[nodiscard]] UiDataWrite Reject(Error error) noexcept;
    [[nodiscard]] UiDataWrite Finish(UiError error) noexcept;
    [[nodiscard]] bool ResolveTable(std::int32_t handle, Address& out, const char* entry, std::uint32_t request = 0) noexcept;
    GameUiDataContext context_;
    UiFaultObserver observer_ = nullptr;
    GameUiLibrary* ui_ = nullptr;
    Address tableObject_ = 0;
    Address tableKeyA_ = 0, tableKeyB_ = 0;
    std::int32_t table_ = 0;
    std::uint32_t thread_ = 0, epoch_ = 0, errors_ = 0;
    Error last_ = Error::None;
    UiError lastUi_ = UiError::None;
    std::string_view bindReason_;
    std::string_view controlReason_;
    std::unordered_map<FrameHandle, FormattedFrame> formats_;
    EventPump eventPump_;
    bool eventsEnabled_ = false;
    std::uint32_t observedArguments_ = 0, observedCommits_ = 0;
    bool stopped_ = false, inFlight_ = false, faulted_ = false;
};

} // namespace Audastra::Runtime32::GameUi
