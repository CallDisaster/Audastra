#pragma once

#include "War3MapCore/GameUi/FrameDefinition.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 地图脚本创建的界面：在脚本里找创建框架的调用，以及同一语句块中紧随其后、参数为常量的设置，
// 得到每个框架刚创建完时的样子。条件分支和其他函数里的调用只记录位置；参数不是常量的设置
// 标为动态，不猜取值；不认识的界面调用按函数归并列出。
// 原版界面对象（计时器窗口、对话框、排行榜、多面板）同样按创建处识别，由游戏按固定规则摆放。
namespace War3Map::GameUi {

// 脚本界面根节点名：脚本创建的框架挂在它下面，根节点铺满屏幕。
inline constexpr std::string_view kScriptRoot = "MapScriptUI";

enum class UiApiFamily : std::uint8_t { Blizzard, Dz, MemHack };

[[nodiscard]] std::string_view UiApiFamilyName(UiApiFamily family) noexcept;

struct ScriptCallSite {
    std::string scriptFile;
    std::uint32_t line = 0;
    std::string function;  // 所在的脚本函数
    std::string call;      // 调用的界面函数
};

// 脚本里引用的框架。
enum class ScriptFrameTarget : std::uint8_t {
    Unknown,   // 无法静态确定
    Screen,    // 绝对坐标（相对屏幕左下角）
    CodeArea,  // 游戏代码创建的区域，name 为区域标识（GameUI、Minimap…）
    Script,    // 脚本创建的框架，frame 为下标
    Named,     // 按名字取的 fdf 框架
};

// 脚本创建的对象：界面框架，或游戏按固定规则摆放的原版界面对象。
enum class ScriptObjectKind : std::uint8_t { Frame, TimerDialog, Dialog, Leaderboard, Multiboard };

// 原版界面对象用的界面模板（TimerDialog、ScriptDialog、Leaderboard、Multiboard）；框架为空。
[[nodiscard]] std::string_view ScriptObjectTemplate(ScriptObjectKind object) noexcept;
// 稳定的英文标识：frame、timer-dialog、dialog、leaderboard、multiboard。
[[nodiscard]] std::string_view ScriptObjectKindName(ScriptObjectKind object) noexcept;

struct ScriptFrameRef {
    ScriptFrameTarget target = ScriptFrameTarget::Unknown;
    std::string name;
    std::uint32_t frame = 0;
};

struct ScriptAnchor {
    FramePoint point = FramePoint::TopLeft;
    ScriptFrameRef relative;
    FramePoint relativePoint = FramePoint::TopLeft;
    float x = 0.0f;
    float y = 0.0f;
};

// 多面板的一个格子（游戏多面板元素的数据）；默认值与游戏新建多面板一致。
struct ScriptBoardItem {
    std::string value;
    std::optional<std::string> icon;  // 为空时用皮肤键 MultiboardDefaultIcon 的图标
    float width = 0.03f;
    bool showValue = true;
    bool showIcon = true;
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};  // 红、绿、蓝、不透明度
};

// 多面板内容。新增的行复制默认值的全部内容；新增的列复制默认值但文字为空（与游戏一致）。
struct ScriptBoard {
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    bool minimized = false;
    bool clipped = false;                // 脚本设置的行列超过预览上限，只保留上限以内
    ScriptBoardItem defaults;            // SetItems… 设置的值，新格子沿用
    std::vector<ScriptBoardItem> items;  // 按行存放，rows × columns 个

    [[nodiscard]] ScriptBoardItem* At(std::uint32_t row, std::uint32_t column);
    [[nodiscard]] const ScriptBoardItem* At(std::uint32_t row, std::uint32_t column) const;
    // 改变行数、列数；超出上限时截到上限、记 clipped 并返回 false。
    bool Resize(std::uint32_t rows, std::uint32_t columns);
};

// 预览保留的多面板行列上限（游戏不限，超出部分不画）。
inline constexpr std::uint32_t kMaxBoardRows = 128;
inline constexpr std::uint32_t kMaxBoardColumns = 32;

struct ScriptFrame {
    UiApiFamily family = UiApiFamily::Blizzard;
    ScriptObjectKind object = ScriptObjectKind::Frame;
    ScriptCallSite created;
    // 保存框架的变量原文（去掉空白），如 udg_CastBar、udg_Bars[i]；结果未保存时为空。
    std::string variable;
    // 框架名：常量名字、模板名或变量；nameDynamic 表示脚本里的名字不是常量。
    std::string name;
    bool nameDynamic = false;
    std::string frameType;     // 按类型创建时的类型（BACKDROP、TEXT…）
    std::string templateName;  // 继承的 fdf 模板
    ScriptFrameRef parent;
    // 创建后同一语句块里常量参数的设置。
    bool clearPoints = false;
    std::vector<ScriptAnchor> anchors;
    std::optional<float> width;
    std::optional<float> height;
    std::optional<std::string> texture;
    // 框架文字；原版界面对象为计时器窗口标题、对话框消息、排行榜标签或多面板标题。
    std::optional<std::string> text;
    // 原版界面对象创建时不显示（BJ 版本创建即显示），显示与否由各自的 Display 调用决定。
    std::optional<bool> visible;
    // 对话框按钮文字，按添加顺序。
    std::vector<std::string> buttons;
    // 多面板的行列与格子；其他对象为空。
    std::optional<ScriptBoard> board;
    // 参数不是常量、没有应用的设置，形如 “SetAbsolutePoint 第 12 行”。
    std::vector<std::string> dynamic;
    // 创建语句块之外作用在同一变量上的调用（运行时改变外观）。
    std::uint32_t runtimeCalls = 0;
    std::vector<ScriptCallSite> runtimeSamples;
    std::vector<std::string> runtimeFunctions;  // 这些调用所在的函数，去重，最多 50 个
};

enum class ScriptUiCallReason : std::uint8_t {
    NotPreviewed,    // 认识的界面调用，预览不还原它的效果
    UntracedTarget,  // 设置作用的框架不是脚本创建并保存在变量里的（如按名字取到的原版框架）
    Unknown,         // 界面函数族里不认识的调用
};

struct ScriptUiCallGroup {
    std::string function;
    UiApiFamily family = UiApiFamily::Blizzard;
    ScriptUiCallReason reason = ScriptUiCallReason::NotPreviewed;
    std::uint32_t count = 0;
    std::vector<ScriptCallSite> samples;
};

struct ScriptFramePlan {
    std::vector<ScriptFrame> frames;
    std::vector<ScriptUiCallGroup> calls;
    std::vector<Diagnostic> diagnostics;
    // 地图字符串表（war3map.wts）：显示时把整段为 TRIGSTR_编号 的文字换成表里的内容，计划里保留原文。
    std::map<std::uint32_t, std::string> strings;
};

// 只分析 JASS 脚本；Lua 脚本记 Info 诊断。
[[nodiscard]] ScriptFramePlan FindScriptFrames(const std::vector<MapScriptText>& scripts);

// 预览没有还原的界面调用，每个函数一条 Info 诊断（问题列表用）。
[[nodiscard]] std::vector<Diagnostic> ScriptCallDiagnostics(const ScriptFramePlan& plan);

// 把脚本框架挂到界面树上（根节点名 kScriptRoot）；没有框架时不建根，返回 kNoNode。
// 引用游戏代码区域时须先调用 FrameTree::AddGameCodeAreas，否则 GameUI 按屏幕处理、其他区域报告缺失。
// 原版界面对象按游戏规则排布：显示中的计时器窗口在右上角向左排开，多面板、排行榜依次在其下，
// 对话框居中于世界画面，按钮自上而下排列、高度随内容；多面板按行列画出格子（图标与文字）。
std::uint32_t AddScriptFrames(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin,
    const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics);

} // namespace War3Map::GameUi
