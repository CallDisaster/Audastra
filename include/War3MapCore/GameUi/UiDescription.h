#pragma once

#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 自有界面描述（audastra.ui）：框架树、数值、状态与事件。作为导入文件存进地图（AudastraUI\<编号>.json），
// 编辑器预览与游戏内运行库读同一份描述，用同一套布局与绘制规则。框架类型按游戏的框架语义取，
// 既能由运行库自绘，也能落成游戏自己的框架。
namespace War3Map::GameUi {

inline constexpr std::string_view kUiDescriptionFormat = "audastra.ui";
inline constexpr int kUiDescriptionVersion = 1;
inline constexpr std::string_view kUiDescriptionFolder = "AudastraUI\\";
inline constexpr std::string_view kUiDescriptionRootType = "AUDASTRAUI";

enum class UiValueType : std::uint8_t { Real, Integer, Boolean, String };

// 地图通过触发器写入、界面读取的数值。
struct UiValue {
    std::string id;
    std::string label;
    UiValueType type = UiValueType::Real;
    std::uint32_t size = 0;    // 0 为单值；N 为 N 格数组，按重复实例的下标取格
    std::string defaultValue;  // 规范文本，见 NormalizeUiValue
    std::optional<double> min;
    std::optional<double> max;

    friend bool operator==(const UiValue&, const UiValue&) = default;
};

// 属性取自数值：写作 "编号"、"编号[]"（重复实例自己的格）或 "编号[下标]"。
// 数值按 值×scale+offset 换算；布尔可取反。
struct UiBinding {
    std::string value;
    bool perInstance = false;
    std::optional<std::uint32_t> index;
    double scale = 1.0;
    double offset = 0.0;
    bool invert = false;

    friend bool operator==(const UiBinding&, const UiBinding&) = default;
};

struct UiNumber {
    double literal = 1.0;
    std::optional<UiBinding> binding;

    friend bool operator==(const UiNumber&, const UiNumber&) = default;
};

struct UiFlag {
    bool literal = true;
    std::optional<UiBinding> binding;

    friend bool operator==(const UiFlag&, const UiFlag&) = default;
};

enum class UiFrameType : std::uint8_t {
    Panel,     // 容器（游戏 FRAME）
    Backdrop,  // 背景图（BACKDROP）
    Text,      // 文字（TEXT）
    Bar,       // 进度条（SIMPLESTATUSBAR）：底图 + 按比例裁剪的填充
    Button,    // 按钮（GLUETEXTBUTTON）：常态/按下贴图 + 文字，可发点击事件
    Template,  // 游戏或地图界面文件里的模板
};

enum class UiAnchorTarget : std::uint8_t { Parent, Screen, Frame, GameArea };

struct UiAnchor {
    FramePoint point = FramePoint::Center;
    UiAnchorTarget target = UiAnchorTarget::Parent;
    std::string targetId;  // Frame：框架编号；GameArea：游戏代码区域标识（如 Minimap）
    FramePoint targetPoint = FramePoint::Center;
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const UiAnchor&, const UiAnchor&) = default;
};

struct UiRepeat {
    std::uint32_t count = 1;
    double dx = 0.0;  // 第 i 个实例的锚点整体偏移 (i×dx, i×dy)
    double dy = 0.0;

    friend bool operator==(const UiRepeat&, const UiRepeat&) = default;
};

struct UiFrame {
    std::string id;
    std::string label;
    UiFrameType type = UiFrameType::Panel;
    std::string templateName;
    std::vector<UiAnchor> anchors;
    std::optional<double> width;
    std::optional<double> height;
    std::string texture;     // Backdrop 背景、Bar 填充、Button 常态
    std::string background;  // Bar 底图
    std::string pushed;      // Button 按下
    std::string text;        // Text / Button 文字模板：{编号}、{编号:小数位}、{编号[]}；{{ }} 为花括号本身
    std::string font;        // 空为游戏默认字体
    double fontHeight = 0.011;
    std::array<double, 4> color{1.0, 1.0, 1.0, 1.0};
    JustifyH justifyH = JustifyH::Center;
    JustifyV justifyV = JustifyV::Middle;
    UiNumber value;  // Bar 填充比例 0..1
    UiFlag visible;
    std::optional<UiRepeat> repeat;
    std::vector<UiFrame> children;

    friend bool operator==(const UiFrame&, const UiFrame&) = default;
};

// 状态：整体切换的一组属性覆盖（例如读条的“打断”）。
struct UiStateOverride {
    std::string frame;
    std::optional<std::string> texture;
    std::optional<std::string> text;
    std::optional<bool> visible;
    std::optional<double> value;

    friend bool operator==(const UiStateOverride&, const UiStateOverride&) = default;
};

struct UiState {
    std::string id;
    std::string label;
    std::vector<UiStateOverride> overrides;

    friend bool operator==(const UiState&, const UiState&) = default;
};

enum class UiEventKind : std::uint8_t { Click, Enter, Leave };

// 界面事件只在本机发生；会影响游戏的处理须经同步后再触发。
struct UiEvent {
    std::string id;
    std::string frame;
    UiEventKind kind = UiEventKind::Click;

    friend bool operator==(const UiEvent&, const UiEvent&) = default;
};

struct UiDescription {
    std::string id;
    std::string label;
    std::vector<UiValue> values;
    std::vector<UiFrame> frames;
    std::vector<UiState> states;
    std::vector<UiEvent> events;

    friend bool operator==(const UiDescription&, const UiDescription&) = default;
};

// 编号：ASCII 字母或下划线开头，后接字母、数字、下划线，最长 64。
[[nodiscard]] bool IsValidUiId(std::string_view id);
[[nodiscard]] std::string_view UiFrameTypeName(UiFrameType type);
[[nodiscard]] std::string_view UiValueTypeName(UiValueType type);

// 解析 audastra.ui 文本。未知字段、类型不符、编号重复、引用不存在一律按 JSON 路径报错，不返回部分结果。
[[nodiscard]] Result<UiDescription> ParseUiDescription(std::string_view text, std::string_view source = {});
// 规范输出：固定字段顺序、省略默认值、两空格缩进、LF 换行；解析后再输出逐字节不变。
[[nodiscard]] std::string SerializeUiDescription(const UiDescription& description);
// 单个框架的规范 JSON（不含子框架，单行），供编辑时取出字段改写。
[[nodiscard]] std::string SerializeUiFrame(const UiFrame& frame);
// 检查编号、引用与取值。解析时已调用；编辑后保存前再调用。
[[nodiscard]] std::vector<Diagnostic> ValidateUiDescription(const UiDescription& description);

// 按编号查框架（含子框架）；没有时为空。
[[nodiscard]] const UiFrame* FindUiFrame(const UiDescription& description, std::string_view id);
[[nodiscard]] UiFrame* FindUiFrame(UiDescription& description, std::string_view id);

// 地图成员路径 AudastraUI\<编号>.json；IsUiDescriptionMember 不区分大小写，接受 / 与 \。
[[nodiscard]] std::string UiDescriptionMemberPath(std::string_view id);
[[nodiscard]] bool IsUiDescriptionMember(std::string_view path);

// 按类型规范化数值文本（实数最短往返写法、整数十进制、true/false、字符串原样）；不合法为空。
[[nodiscard]] std::optional<std::string> NormalizeUiValue(UiValueType type, std::string_view text);

// 预览与运行时的当前取值与状态。键为数值编号或 "编号[下标]"；缺省用默认值。
struct UiInstanceState {
    std::map<std::string, std::string, std::less<>> values;
    std::string state;  // 空为基础状态
};

// 数值当前文本；编号未知或下标越界为空，输入不合法时用默认值。
[[nodiscard]] std::optional<std::string> UiValueText(const UiDescription& description, const UiInstanceState& state,
    std::string_view id, std::optional<std::uint32_t> index = std::nullopt);
// 展开文字模板；instance 为重复实例下标（"编号[]" 用）。
[[nodiscard]] std::string FormatUiText(const UiDescription& description, const UiInstanceState& state,
    std::string_view text, std::optional<std::uint32_t> instance = std::nullopt);

// 把描述挂到屏幕下：根节点名为描述编号、类型 AUDASTRAUI，框架节点名为框架编号（重复实例为 编号#下标）。
// 引用游戏代码区域的锚点在界面树中没有该区域时记诊断并跳过。返回根节点编号。
std::uint32_t AddUiDescription(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin,
    const UiDescription& description, const UiInstanceState& state, std::vector<Diagnostic>& diagnostics);

} // namespace War3Map::GameUi
