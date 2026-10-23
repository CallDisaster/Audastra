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
    Query,       // 只读取，不改变外观
    LoadToc,     // 由 MapUiFiles 处理
    FrameOther,  // 作用于框架，预览不还原
    Other,       // 其他界面调用（原版界面开关等），预览不还原
    Unknown,
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
    std::int8_t valueArg = 1;                           // SetText、SetVisible、SetHidden、AddButton 的取值参数
    bool shown = false;                                 // 创建即显示（BJ 版本）
};

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
[[nodiscard]] bool ApplyFrameCall(ScriptFrame& frame, const CallSpec& spec,
    const std::vector<std::optional<ScriptValue>>& args, const ScriptFrameRef& relative);

} // namespace War3Map::GameUi::detail
