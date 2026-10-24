#pragma once

#include "GameUiAbi127.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

// 自有界面库：用游戏自己的界面函数创建和修改界面。
// 只在游戏主线程调用；地图侧通过稳定的整数句柄引用框架，句柄带代数，框架删除或会话重置后旧句柄一律失效。
// 不支持的组合（例如给背景图设文字）明确返回 Unsupported，不假装成功。
namespace Audastra::Runtime32::GameUi {

enum class UiError : std::uint8_t {
    None,
    NotReady,      // 入口表不完整或游戏界面不存在
    WrongThread,
    BadHandle,     // 句柄为 0、越界或已失效
    StaleFrame,    // 框架已被游戏销毁
    Unsupported,   // 这类框架没有这个操作
    NotFound,      // 模板、名字或 toc 不存在
    BadArgument,
    TableFull,
    NotOwned,      // 游戏自己的框架不能由地图删除
    Faulted,       // 游戏函数出现异常，界面库已停用
};

[[nodiscard]] std::string_view UiErrorText(UiError error) noexcept;

using FrameHandle = std::int32_t;

enum class FrameFamily : std::uint8_t { Classic, Simple, Region };

// 事件掩码。
inline constexpr std::uint8_t kUiEventEnter = 1;
inline constexpr std::uint8_t kUiEventLeave = 2;
inline constexpr std::uint8_t kUiEventClick = 4;

struct UiFrameEvent {
    FrameHandle frame = 0;
    std::uint8_t kind = 0;   // 上面三个值之一

    friend bool operator==(const UiFrameEvent&, const UiFrameEvent&) = default;
};

// 一次轮询读到的鼠标状态：鼠标下的框架（游戏自己算好的）和左键是否按着。
struct PointerSnapshot {
    Address hovered = 0;
    bool buttonDown = false;
};

struct UiCreated {
    FrameHandle handle = 0;
    UiError error = UiError::None;
};

class GameUiLibrary {
public:
    // entries 必须完整（ResolveGameUiEntryPoints127 的结果或测试替身）；缺项时所有操作返回 NotReady。
    explicit GameUiLibrary(const GameUiEntryPoints& entries) noexcept;

    // 地图开始或结束时调用：游戏会自己销毁全部界面，这里作废全部句柄和事件。
    void ResetSession() noexcept;

    [[nodiscard]] UiCreated Root();
    [[nodiscard]] UiCreated World();
    [[nodiscard]] UiCreated Find(std::string_view name, int context);
    // 按 fdf 模板创建；parent 为 0 时挂在游戏界面根下。
    [[nodiscard]] UiCreated Create(std::string_view templateName, FrameHandle parent, int level, int context);
    [[nodiscard]] UiCreated CreateSimple(std::string_view templateName, FrameHandle parent, int context);
    [[nodiscard]] UiError LoadToc(std::string_view path);

    // 锚点 0..8：左上、上、右上、左、中、右、左下、下、右下。坐标是 0.8×0.6 屏幕坐标。
    [[nodiscard]] UiError SetPoint(FrameHandle frame, int point, FrameHandle relative, int relativePoint, float x, float y);
    [[nodiscard]] UiError SetAbsPoint(FrameHandle frame, int point, float x, float y);
    [[nodiscard]] UiError SetAllPoints(FrameHandle frame, FrameHandle relative);
    [[nodiscard]] UiError ClearPoints(FrameHandle frame);
    [[nodiscard]] UiError SetWidth(FrameHandle frame, float width);
    [[nodiscard]] UiError SetHeight(FrameHandle frame, float height);
    [[nodiscard]] UiError SetScale(FrameHandle frame, float scale);

    [[nodiscard]] UiError SetVisible(FrameHandle frame, bool visible);
    [[nodiscard]] UiError SetAlpha(FrameHandle frame, int alpha);
    [[nodiscard]] UiError SetLevel(FrameHandle frame, int level);
    [[nodiscard]] UiError SetParent(FrameHandle frame, FrameHandle parent);

    [[nodiscard]] UiError SetText(FrameHandle frame, std::string_view text);
    [[nodiscard]] UiError SetFont(FrameHandle frame, std::string_view font, float height, int flags);
    // 水平 0 左 1 中 2 右；竖直 0 上 1 中 2 下。
    [[nodiscard]] UiError SetTextAlign(FrameHandle frame, int horizontal, int vertical);
    [[nodiscard]] UiError SetColor(FrameHandle frame, std::uint32_t argb);
    [[nodiscard]] UiError SetTexture(FrameHandle frame, std::string_view path, bool tile);
    [[nodiscard]] UiError SetValue(FrameHandle frame, float value);
    [[nodiscard]] UiError SetLimits(FrameHandle frame, float minimum, float maximum);
    [[nodiscard]] UiError SetModel(FrameHandle frame, std::string_view path, int modelType);
    [[nodiscard]] UiError SetEnabled(FrameHandle frame, bool enabled);
    // 只删地图创建的框架；同时作废挂在它下面的句柄。
    [[nodiscard]] UiError Destroy(FrameHandle frame);

    // 事件只在本机发生；会影响游戏的处理须由调用方同步后再执行。
    [[nodiscard]] UiError Watch(FrameHandle frame, std::uint8_t mask);
    [[nodiscard]] PointerSnapshot ReadPointer() const;
    void Poll(const PointerSnapshot& pointer);
    [[nodiscard]] std::vector<UiFrameEvent> TakeEvents();

    [[nodiscard]] std::size_t LiveCount() const noexcept;
    [[nodiscard]] bool Faulted() const noexcept { return faulted_; }

private:
    enum class Owner : std::uint8_t { Library, Game };
    struct Slot {
        Address object = 0;
        Address vtable = 0;
        FrameFamily family = FrameFamily::Classic;
        Owner owner = Owner::Game;
        std::uint16_t generation = 1;
        std::uint8_t watch = 0;
        bool live = false;
        bool hovered = false;     // 上次轮询时鼠标在它或它的子框架上
        bool armed = false;       // 左键在它上面按下，等待松开
    };

    [[nodiscard]] UiError Ready() const noexcept;
    [[nodiscard]] UiCreated Adopt(Address object, Owner owner);
    [[nodiscard]] Slot* Resolve(FrameHandle handle, UiError& error);
    [[nodiscard]] FrameHandle HandleOf(std::size_t index) const noexcept;
    [[nodiscard]] bool Classify(Address object, Address& vtable, FrameFamily& family) const noexcept;
    [[nodiscard]] Address LayoutOf(const Slot& slot) const noexcept;
    [[nodiscard]] Address ParentOf(Address object) const noexcept;
    [[nodiscard]] bool IsAncestorOrSelf(Address ancestor, Address object) const noexcept;
    void Kill(std::size_t index) noexcept;
    template <class Call> [[nodiscard]] UiError Run(Call&& call);

    GameUiEntryPoints entries_;
    bool complete_ = false;
    bool faulted_ = false;
    bool buttonDown_ = false;
    std::uint32_t thread_ = 0;
    std::vector<Slot> slots_;            // 下标 0 不用
    std::vector<std::uint16_t> free_;
    std::unordered_map<Address, std::uint16_t> byObject_;
    std::vector<UiFrameEvent> events_;
};

} // namespace Audastra::Runtime32::GameUi
