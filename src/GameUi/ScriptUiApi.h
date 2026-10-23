#pragma once

#include "War3MapCore/GameUi/ScriptFrames.h"
#include "ScriptValue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 地图脚本里的界面函数表：暴雪原生（含计时器窗口、对话框、排行榜、多面板）、DzAPI 与 MemHack 的公开函数名及参数位置。
// 脚本界面的静态识别与预览模拟共用；只记函数名与参数含义。
namespace War3Map::GameUi::detail {

enum class CallKind : std::uint8_t {
    Create,
    LastCreated,  // GetLastCreated…：同一语句块里最近一次 BJ 创建的原版界面对象
    GameUi,       // 返回游戏界面根
    Origin,       // BlzGetOriginFrame(类型, 序号)
    ByName,       // 按名字取框架
    CodeArea,     // 取游戏代码创建的区域
    SetAbsPoint,  // 框架, 定位点, x, y
    SetPoint,     // 框架, 定位点, 目标框架, 目标定位点, x, y
    ClearPoints,
    SetAllPoints,  // 框架, 目标框架
    SetSize,
    SetWidth,
    SetHeight,
    SetTexture,  // 框架, 路径, …
    SetText,
    SetVisible,  // 框架, 是否显示
    SetHidden,   // 框架, 是否隐藏
    AddButton,   // 对话框, 按钮文字
    ClearButtons,
    BoardRows,      // 多面板, 行数
    BoardColumns,   // 多面板, 列数
    BoardMinimize,  // 多面板与是否最小化（参数位置见 targetArg / valueArg）
    BoardItems,     // 多面板格子设置，作用范围见 CallSpec::board
    Leaderboard,    // 排行榜内容的修改，见 CallSpec::leaderboard
    BoardGetItem,   // MultiboardGetItem(多面板, 行, 列)：取格子
    Query,       // 只读取，不改变外观
    LoadToc,     // 由 MapUiFiles 处理
    FrameOther,  // 作用于框架，预览不还原
    Other,       // 其他界面调用（原版界面开关等），预览不还原
    Unknown,
};

// 多面板格子设置的作用范围。
enum class BoardScope : std::uint8_t {
    All,        // 多面板, 取值…：全部格子与新格子的默认值
    Item,       // 格子, 取值…：MultiboardGetItem 取到的格子
    ColumnRow,  // 多面板, 列, 行, 取值…：BJ 版本，列、行从 1 起，0 表示整列/整行
};
// 多面板格子的属性：文字、显示文字与图标（两个布尔）、宽度、图标、文字颜色（四个分量）。
enum class BoardProperty : std::uint8_t { Value, Style, Width, Icon, Color };

// 排行榜内容的修改。作用的项由 CallSpec::indexArg（序号，从 0 起）或 playerArg（该玩家的第一项）指定。
enum class LeaderboardOp : std::uint8_t {
    Add,         // 标签, 数值；玩家见 playerArg
    Remove,      // 移除指定的项
    Clear,
    Size,        // 显示的行数
    Style,       // 显示标题、名字、数值、图标（四个布尔，名字以后作用于全部项）
    Label,       // 项的标签
    Value,       // 项的数值
    ItemStyle,   // 项的显示名字、数值、图标（三个布尔）
    LabelColor,  // 标签颜色（四个分量）；没有指定项时作用于全部项
    ValueColor,
    Sort,        // 是否升序；排序依据见 CallSpec::sort
};
// 排行榜排序依据；ByArgument 为 LeaderboardSortItemsBJ 的排序方式参数（0 数值、1 玩家、2 标签）。
enum class LeaderboardSort : std::uint8_t { Value, Player, Label, ByArgument };

// 多面板格子位置（行、列从 0 起）。
struct BoardCell {
    std::uint32_t row = 0;
    std::uint32_t column = 0;
};

struct CallSpec {
    std::string_view name;
    UiApiFamily family = UiApiFamily::Blizzard;
    CallKind kind = CallKind::Unknown;
    std::int8_t typeArg = -1;
    std::int8_t nameArg = -1;
    std::int8_t templateArg = -1;
    std::int8_t parentArg = -1;
    std::string_view area = {};  // CodeArea：区域标识；indexed 时接第 0 个参数加 1
    bool indexed = false;
    ScriptObjectKind object = ScriptObjectKind::Frame;  // Create / LastCreated 的对象种类
    std::int8_t titleArg = -1;                          // 原版界面对象创建时的标题参数
    std::int8_t targetArg = 0;                          // 设置作用的对象参数
    std::int8_t valueArg = 1;                           // 取值参数（多个取值时为第一个）
    bool shown = false;                                 // 创建即显示（BJ 版本）
    std::int8_t rowsArg = -1;                           // CreateMultiboardBJ 的行数、列数参数
    std::int8_t columnsArg = -1;
    BoardScope board = BoardScope::All;                 // BoardItems 的作用范围
    BoardProperty property = BoardProperty::Value;      // BoardItems 的属性
    bool percent = false;  // BJ 版本：宽度为屏幕宽度的百分比，颜色为百分比且第四个分量是透明度
    LeaderboardOp leaderboard = LeaderboardOp::Add;     // Leaderboard 的操作
    LeaderboardSort sort = LeaderboardSort::Value;
    std::int8_t indexArg = -1;                          // 排行榜项的序号参数
    std::int8_t playerArg = -1;                         // 排行榜项的玩家参数
    // BJ 版本的排行榜调用：添加前先移除该玩家已有的项，改完按项数重设行数（LeaderboardResizeBJ）。
    bool leaderboardBj = false;
};

// 取值参数的类型（预览模拟求值用）。
enum class ArgumentType : std::uint8_t { Integer, Real, String, Boolean, Player };
// 一次设置调用的取值参数位置与类型，按参数顺序；作用对象参数不在其中。
struct ArgumentSlot {
    std::uint32_t index = 0;
    ArgumentType type = ArgumentType::String;
};
[[nodiscard]] std::vector<ArgumentSlot> ValueArguments(const CallSpec& spec);

// 表内函数，或属于界面函数族前缀的函数（Get/Is 开头视为只读，其余为不认识）；其他函数为空。
[[nodiscard]] std::optional<CallSpec> LookupCall(std::string_view name);
[[nodiscard]] bool IsSetter(CallKind kind) noexcept;
// BlzGetOriginFrame 的类型常量对应的游戏代码区域标识；按序号区分的区域接序号加 1。不认识或序号无效时为空。
[[nodiscard]] std::optional<std::string> OriginArea(std::string_view constant, std::optional<float> index);
// CodeArea 类调用对应的区域标识；index 为第 0 个参数。
[[nodiscard]] std::optional<std::string> CodeAreaOf(const CallSpec& spec, std::optional<float> index);

// 返回框架的调用（游戏界面根、原版区域、按名字取框架）对应的引用；args 为已求值的调用参数。
// 按名字取时 target 为 Named，由调用方决定是否对应脚本框架。其他调用或参数不全时为空。
[[nodiscard]] std::optional<ScriptFrameRef> FrameRefOfCall(const CallSpec& spec,
    const std::vector<std::optional<ScriptValue>>& args);

// 把一次设置调用作用到框架上。args 与调用参数同序（作用对象那一个可为空）；relative 为参数里
// 引用的框架（SetPoint 第 2 个、SetAllPoints 第 1 个）。参数不全或无法确定时返回 false，不改框架；
// 对话框按钮文字无法确定时仍添加一个空文字按钮（按钮数不变），并返回 false。
// strings 为地图字符串表（排行榜标签按替换后的文字判断）。
[[nodiscard]] bool ApplyFrameCall(ScriptFrame& frame, const CallSpec& spec,
    const std::vector<std::optional<ScriptValue>>& args, const ScriptFrameRef& relative, const TriggerStrings& strings);

// 多面板设置（BoardRows、BoardColumns、BoardMinimize、BoardItems）；Item 范围的格子由 item 给出。
// 框架不是多面板、参数无法确定或格子超出行列时返回 false，不改内容。
[[nodiscard]] bool ApplyBoardCall(ScriptFrame& frame, const CallSpec& spec,
    const std::vector<std::optional<ScriptValue>>& args, std::optional<BoardCell> item = std::nullopt);
// 排行榜标签改变之后：标签非空时显示标题，BJ 版本（LeaderboardSetLabelBJ）再按项数重设行数。
void LeaderboardLabelChanged(ScriptFrame& frame, const CallSpec& spec, const TriggerStrings& strings);
// 排行榜修改（Leaderboard 类调用）。
// 框架不是排行榜或参数无法确定时返回 false；添加时玩家无法确定仍添加该项（不替换已有的项）并返回 false。
[[nodiscard]] bool ApplyLeaderboardCall(ScriptFrame& frame, const CallSpec& spec,
    const std::vector<std::optional<ScriptValue>>& args, const TriggerStrings& strings);
// MultiboardGetItem 的行、列参数；不是常量或为负时为空。
[[nodiscard]] std::optional<BoardCell> BoardCellOf(const std::optional<ScriptValue>& row,
    const std::optional<ScriptValue>& column);

} // namespace War3Map::GameUi::detail
