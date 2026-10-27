#include "War3MapCore/GameUi/UiFrameKinds.h"

#include <array>

namespace War3Map::GameUi {
namespace {

using Slot = UiFrameSlot;
using SlotKind = UiFrameSlotKind;

constexpr std::string_view kTooltipBackground = "UI\\Widgets\\ToolTips\\Human\\human-tooltip-background";

constexpr std::array<Slot, 1> kPanelSlots{{{"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 2> kBackdropSlots{{{"texture", "贴图", SlotKind::Texture, true}, {"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 2> kIconSlots{{{"texture", "图标", SlotKind::Icon, true}, {"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 2> kModelSlots{{{"model", "模型", SlotKind::Model, true}, {"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 2> kTextSlots{{{"text", "文字", SlotKind::Text, true}, {"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 4> kBarSlots{{{"value", "进度", SlotKind::Number, true}, {"texture", "填充贴图", SlotKind::Texture, true},
    {"background", "底图", SlotKind::Texture, false}, {"visible", "显示", SlotKind::Flag, true}}};
constexpr std::array<Slot, 4> kButtonSlots{{{"texture", "常态贴图", SlotKind::Texture, true}, {"pushed", "按下贴图", SlotKind::Texture, false},
    {"text", "文字", SlotKind::Text, true}, {"visible", "显示", SlotKind::Flag, true}}};

constexpr std::array<UiFrameKind, 8> kKinds{{
    {"panel", "布局", "容器", "把一组界面放在一起，整体移动、显示或隐藏。",
        "在容器上右键创建的界面放进容器里，默认对齐容器中心。容器本身不画东西。", UiFrameType::Panel, kPanelSlots},
    {"backdrop", "图片", "背景", "显示一张贴图，做面板底板、边框或装饰。",
        "贴图写成 {编号} 或 {编号[]} 时按文字数值换图。", UiFrameType::Backdrop, kBackdropSlots},
    {"icon", "图片", "图标", "显示技能、物品或增益的方形图标。",
        "选命令按钮图标；和背景相同，只是默认为方形图标。", UiFrameType::Backdrop, kIconSlots},
    {"model", "图片", "模型", "显示一个模型或特效，做头像、光圈、技能特效一类会动的装饰。",
        "选游戏或地图里的模型；模型写成 {编号} 时按文字数值换模型。编辑器预览只标出模型名，游戏里播放模型。", UiFrameType::Model, kModelSlots},
    {"text", "文字", "文字", "显示一段文字或数值。",
        "文字里写 {编号} 显示数值，{编号:1} 保留一位小数，{编号[]} 取重复实例自己的格。", UiFrameType::Text, kTextSlots},
    {"bar", "数值", "进度条", "按比例显示填充，做血条、蓝条、读条。",
        "进度取 0 到 1 的实数数值；填充与底图都可换贴图。", UiFrameType::Bar, kBarSlots},
    {"button", "交互", "按钮", "可以点击；点击、移入、移出时发出界面事件。",
        "在事件页登记事件，再写触发器响应。界面事件只在本机发生，影响游戏的处理要先同步。", UiFrameType::Button, kButtonSlots},
    {"template", "模板", "模板", "使用游戏或地图界面文件里已有的框架模板。",
        "外观由模板决定；模板里的子框架不能单独编辑。", UiFrameType::Template, kPanelSlots},
}};

UiAnchor CenterOnParent() { return UiAnchor{}; }

} // namespace

std::span<const UiFrameKind> UiFrameKinds() { return kKinds; }

const UiFrameKind* FindUiFrameKind(std::string_view id) {
    for (const auto& kind : kKinds) {
        if (kind.id == id) return &kind;
    }
    return nullptr;
}

std::string_view UiFrameIdPrefix(const UiFrameKind& kind) {
    return kind.id == "backdrop" ? std::string_view("back") : kind.id;
}

UiFrame DefaultUiFrame(const UiFrameKind& kind, std::string id, std::string templateName) {
    UiFrame frame;
    frame.id = std::move(id);
    frame.type = kind.type;
    frame.anchors.push_back(CenterOnParent());
    if (kind.id == "backdrop") {
        frame.width = 0.12;
        frame.height = 0.06;
        frame.texture = kTooltipBackground;
    } else if (kind.id == "icon") {
        frame.width = 0.032;
        frame.height = 0.032;
        frame.texture = "ReplaceableTextures\\CommandButtons\\BTNSelectHeroOn";
    } else if (kind.id == "model") {
        frame.width = 0.06;
        frame.height = 0.06;
        frame.model = "Abilities\\Spells\\Other\\TalkToMe\\TalkToMe.mdx";
    } else if (kind.id == "text") {
        frame.text = "文字";
    } else if (kind.id == "bar") {
        frame.width = 0.15;
        frame.height = 0.015;
        frame.texture = "ReplaceableTextures\\TeamColor\\TeamColor06";
        frame.background = "ReplaceableTextures\\TeamColor\\TeamColor13";
    } else if (kind.id == "button") {
        frame.width = 0.08;
        frame.height = 0.03;
        frame.texture = kTooltipBackground;
        frame.text = "按钮";
    } else if (kind.id == "template") {
        frame.templateName = std::move(templateName);
    } else {
        frame.width = 0.12;
        frame.height = 0.06;
    }
    return frame;
}

} // namespace War3Map::GameUi
