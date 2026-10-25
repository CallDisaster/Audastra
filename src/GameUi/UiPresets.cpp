#include "War3MapCore/GameUi/UiPresets.h"

#include "UiDescriptionText.h"

#include <utility>

// 起始模板直接拼描述结构，再走与读取相同的检查；贴图全部取游戏自带的。
namespace War3Map::GameUi {
namespace {

constexpr std::string_view kTooltip = "UI\\Widgets\\ToolTips\\Human\\human-tooltip-background";
std::string TeamColor(int index) {
    return std::string("ReplaceableTextures\\TeamColor\\TeamColor") + (index < 10 ? "0" : "") + std::to_string(index);
}

UiAnchor Anchor(FramePoint point, UiAnchorTarget target = UiAnchorTarget::Parent, std::optional<FramePoint> targetPoint = std::nullopt,
    double x = 0.0, double y = 0.0) {
    UiAnchor anchor;
    anchor.point = point;
    anchor.target = target;
    anchor.targetPoint = targetPoint.value_or(point);
    anchor.x = x;
    anchor.y = y;
    return anchor;
}

UiBinding Bind(std::string value, bool perInstance = false) {
    UiBinding binding;
    binding.value = std::move(value);
    binding.perInstance = perInstance;
    return binding;
}

UiValue Value(std::string id, std::string label, UiValueType type, std::string initial, std::uint32_t size = 0) {
    UiValue value;
    value.id = std::move(id);
    value.label = std::move(label);
    value.type = type;
    value.size = size;
    value.defaultValue = std::move(initial);
    if (type == UiValueType::Real && value.id != CastBarPreset::kRemaining) {
        value.min = 0.0;
        value.max = 1.0;
    }
    return value;
}

UiFrame Text(std::string id, std::string label, std::string text, JustifyH justify, UiAnchor anchor, double width, double height) {
    UiFrame frame;
    frame.id = std::move(id);
    frame.label = std::move(label);
    frame.type = UiFrameType::Text;
    frame.text = std::move(text);
    frame.justifyH = justify;
    frame.anchors = {anchor};
    frame.width = width;
    frame.height = height;
    return frame;
}

UiFrame Bar(std::string id, std::string label, int color, std::string value, bool perInstance, UiAnchor anchor, double width, double height) {
    UiFrame frame;
    frame.id = std::move(id);
    frame.label = std::move(label);
    frame.type = UiFrameType::Bar;
    frame.texture = TeamColor(color);
    frame.background = TeamColor(8);
    frame.value.binding = Bind(std::move(value), perInstance);
    frame.anchors = {anchor};
    frame.width = width;
    frame.height = height;
    return frame;
}

// 读条条：技能栏上方居中。进度条黄色，引导绿色，打断红色并显示“打断”。
UiDescription CastBar() {
    UiDescription d;
    using namespace CastBarPreset;
    d.values = {Value(std::string(kVisible), "显示", UiValueType::Boolean, "false"), Value(std::string(kProgress), "进度", UiValueType::Real, "0"),
        Value(std::string(kText), "技能名", UiValueType::String, ""), Value(std::string(kRemaining), "剩余秒数", UiValueType::Real, "0")};
    UiFrame back;
    back.id = "back";
    back.label = "底框";
    back.type = UiFrameType::Backdrop;
    back.texture = std::string(kTooltip);
    back.width = 0.25;
    back.height = 0.032;
    back.anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::Screen, std::nullopt, 0.0, 0.17)};
    back.visible.binding = Bind(std::string(kVisible));
    UiFrame fill = Bar("fill", "进度", 4, std::string(kProgress), false, Anchor(FramePoint::Center), 0.234, 0.016);
    fill.children = {Text("name", "技能名", "{" + std::string(kText) + "}", JustifyH::Left, Anchor(FramePoint::Left, UiAnchorTarget::Parent, std::nullopt, 0.004), 0.16, 0.016),
        Text("time", "剩余", "{" + std::string(kRemaining) + ":1}", JustifyH::Right, Anchor(FramePoint::Right, UiAnchorTarget::Parent, std::nullopt, -0.004), 0.06, 0.016)};
    back.children = {std::move(fill)};
    d.frames = {std::move(back)};
    UiStateOverride channelFill;
    channelFill.frame = "fill";
    channelFill.texture = TeamColor(6);
    UiStateOverride interruptFill;
    interruptFill.frame = "fill";
    interruptFill.texture = TeamColor(0);
    UiStateOverride interruptTime;
    interruptTime.frame = "time";
    interruptTime.text = "打断";
    d.states = {UiState{std::string(kChannel), "引导", {channelFill}}, UiState{std::string(kInterrupted), "打断", {interruptFill, interruptTime}}};
    return d;
}

// 团队框架：屏幕左侧一列 5 格，每格名字、血条（绿）、蓝条（蓝），死亡时血条上显示“死亡”，过远时右上角显示“远”；
// 各值按格取数组。点格子发 select 事件。
UiDescription Party() {
    using namespace PartyPreset;
    const auto id = [](std::string_view value) { return std::string(value); };
    UiDescription d;
    d.values = {Value(id(kShown), "显示", UiValueType::Boolean, "false", kSlots), Value(id(kHealth), "生命", UiValueType::Real, "1", kSlots),
        Value(id(kMana), "法力", UiValueType::Real, "1", kSlots), Value(id(kName), "名字", UiValueType::String, "", kSlots),
        Value(id(kDead), "死亡", UiValueType::Boolean, "false", kSlots), Value(id(kFar), "过远", UiValueType::Boolean, "false", kSlots)};
    for (std::size_t i = 0; i < kBuffs.size(); ++i) {
        d.values.push_back(Value(id(kBuffs[i]), "增减益 " + std::to_string(i + 1), UiValueType::String, "", kSlots));
    }
    UiFrame slot;
    slot.id = "slot";
    slot.label = "队员";
    slot.type = UiFrameType::Backdrop;
    slot.texture = std::string(kTooltip);
    slot.width = 0.12;
    slot.height = 0.04;
    // 让开左上角的英雄头像栏（右边缘约 0.041）。
    slot.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Screen, std::nullopt, 0.05, -0.13)};
    slot.visible.binding = Bind(id(kShown), true);
    slot.repeat = UiRepeat{kSlots, 0.0, -0.044};
    UiFrame hp = Bar("hp", "生命", 6, id(kHealth), true, Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.005, -0.016), 0.11, 0.012);
    UiFrame dead = Text("dead", "死亡", "死亡", JustifyH::Center, Anchor(FramePoint::Center), 0.11, 0.012);
    dead.visible.binding = Bind(id(kDead), true);
    hp.children = {std::move(dead)};
    UiFrame distant = Text("far", "过远", "远", JustifyH::Right, Anchor(FramePoint::TopRight, UiAnchorTarget::Parent, std::nullopt, -0.005, -0.003), 0.03, 0.012);
    distant.visible.binding = Bind(id(kFar), true);
    slot.children = {Text("name", "名字", "{" + id(kName) + "[]}", JustifyH::Left,
                         Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.005, -0.003), 0.08, 0.012),
        std::move(distant), std::move(hp),
        Bar("mp", "法力", 1, id(kMana), true, Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.005, -0.029), 0.11, 0.006)};
    // 点击事件只能挂在按钮上：一个无贴图的按钮盖满整格。
    UiFrame pick;
    pick.id = "pick";
    pick.label = "点选";
    pick.type = UiFrameType::Button;
    pick.anchors = {Anchor(FramePoint::TopLeft), Anchor(FramePoint::BottomRight)};
    slot.children.push_back(std::move(pick));
    // 增减益图标排在格子右侧；图标数值为空时不显示。
    for (std::size_t i = 0; i < kBuffs.size(); ++i) {
        UiFrame icon;
        icon.id = "icon" + std::to_string(i + 1);
        icon.label = "增减益 " + std::to_string(i + 1);
        icon.type = UiFrameType::Backdrop;
        icon.texture = "{" + id(kBuffs[i]) + "[]}";
        icon.width = 0.016;
        icon.height = 0.016;
        icon.anchors = {i == 0 ? Anchor(FramePoint::Left, UiAnchorTarget::Parent, FramePoint::Right, 0.002)
                               : Anchor(FramePoint::Left, UiAnchorTarget::Frame, FramePoint::Right, 0.001)};
        if (i > 0) icon.anchors.front().targetId = "icon" + std::to_string(i);
        icon.visible.binding = Bind(id(kBuffs[i]), true);
        slot.children.push_back(std::move(icon));
    }
    d.frames = {std::move(slot)};
    d.events = {UiEvent{id(kSelect), "pick", UiEventKind::Click}};
    return d;
}

// 统一天赋树：技能栏上方右侧的入口按钮（有可选天赋时带个数），屏幕中间偏左的面板。
// 面板每层一行：左侧解锁等级，右侧每列一个选项（图标 + 名字，已选时图标外有青色描边）；选项名字为空时整格不显示。
// 鼠标移到选项上，面板右侧显示说明框。
UiDescription Talents() {
    using namespace TalentPreset;
    const auto id = [](std::string_view value) { return std::string(value); };
    UiDescription d;
    d.values = {Value(id(kShown), "面板显示", UiValueType::Boolean, "false"), Value(id(kEntryShown), "入口显示", UiValueType::Boolean, "false"),
        Value(id(kEntry), "入口文字", UiValueType::String, "天赋"), Value(id(kTitle), "标题", UiValueType::String, "天赋"),
        Value(id(kPoints), "可选数", UiValueType::String, ""), Value(id(kLevel), "解锁等级", UiValueType::String, "", kTiers)};
    for (std::size_t c = 0; c < kOptions; ++c) {
        const std::string column = " " + std::to_string(c + 1);
        d.values.push_back(Value(id(kIcons[c]), "图标" + column, UiValueType::String, "", kTiers));
        d.values.push_back(Value(id(kNames[c]), "名字" + column, UiValueType::String, "", kTiers));
        d.values.push_back(Value(id(kPicked[c]), "已选" + column, UiValueType::Boolean, "false", kTiers));
    }
    d.values.push_back(Value(id(kTipShown), "说明显示", UiValueType::Boolean, "false"));
    d.values.push_back(Value(id(kTipTitle), "说明标题", UiValueType::String, ""));
    d.values.push_back(Value(id(kTipText), "说明正文", UiValueType::String, ""));

    UiFrame entry;
    entry.id = "entry";
    entry.label = "入口";
    entry.type = UiFrameType::Button;
    entry.texture = std::string(kTooltip);
    entry.text = "{" + id(kEntry) + "}";
    entry.width = 0.07;
    entry.height = 0.026;
    entry.anchors = {Anchor(FramePoint::BottomRight, UiAnchorTarget::Screen, std::nullopt, -0.006, 0.17)};
    entry.visible.binding = Bind(id(kEntryShown));

    UiFrame panel;
    panel.id = "panel";
    panel.label = "面板";
    panel.type = UiFrameType::Backdrop;
    panel.texture = std::string(kTooltip);
    panel.width = 0.34;
    panel.height = 0.36;
    panel.anchors = {Anchor(FramePoint::Center, UiAnchorTarget::Screen, std::nullopt, -0.08, 0.04)};
    panel.visible.binding = Bind(id(kShown));
    UiFrame title = Text("title", "标题", "{" + id(kTitle) + "}", JustifyH::Left, Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.014, -0.012), 0.2, 0.016);
    UiFrame close;
    close.id = "close";
    close.label = "关闭";
    close.type = UiFrameType::Button;
    close.text = "×";
    close.width = 0.022;
    close.height = 0.022;
    close.anchors = {Anchor(FramePoint::TopRight, UiAnchorTarget::Parent, std::nullopt, -0.008, -0.008)};

    UiFrame row;
    row.id = "row";
    row.label = "层";
    row.width = 0.312;
    row.height = 0.054;
    row.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.014, -0.036)};
    row.repeat = UiRepeat{kTiers, 0.0, -0.058};
    UiFrame level = Text("level", "解锁等级", "{" + id(kLevel) + "[]}", JustifyH::Left, Anchor(FramePoint::Left), 0.042, 0.03);
    row.children = {std::move(level)};
    for (std::size_t c = 0; c < kOptions; ++c) {
        const std::string n = std::to_string(c + 1);
        UiFrame cell;
        cell.id = "cell" + n;
        cell.label = "选项 " + n;
        cell.width = 0.086;
        cell.height = 0.05;
        cell.anchors = {Anchor(FramePoint::Left, UiAnchorTarget::Parent, std::nullopt, 0.046 + 0.089 * static_cast<double>(c))};
        cell.visible.binding = Bind(id(kNames[c]), true);
        UiFrame ring;
        ring.id = "ring" + n;
        ring.label = "已选描边 " + n;
        ring.type = UiFrameType::Backdrop;
        ring.texture = TeamColor(2);
        ring.width = 0.036;
        ring.height = 0.036;
        ring.anchors = {Anchor(FramePoint::Left)};
        ring.visible.binding = Bind(id(kPicked[c]), true);
        UiFrame art;
        art.id = "art" + n;
        art.label = "图标 " + n;
        art.type = UiFrameType::Backdrop;
        art.texture = "{" + id(kIcons[c]) + "[]}";
        art.width = 0.032;
        art.height = 0.032;
        art.anchors = {Anchor(FramePoint::Left, UiAnchorTarget::Parent, std::nullopt, 0.002)};
        UiFrame name = Text("label" + n, "名字 " + n, "{" + id(kNames[c]) + "[]}", JustifyH::Left,
            Anchor(FramePoint::Left, UiAnchorTarget::Parent, std::nullopt, 0.038), 0.048, 0.044);
        // 点击、移入、移出事件挂在盖满整格的无贴图按钮上。
        UiFrame hit;
        hit.id = "hit" + n;
        hit.label = "点选 " + n;
        hit.type = UiFrameType::Button;
        hit.anchors = {Anchor(FramePoint::TopLeft), Anchor(FramePoint::BottomRight)};
        cell.children = {std::move(ring), std::move(art), std::move(name), std::move(hit)};
        row.children.push_back(std::move(cell));
    }

    UiFrame tip;
    tip.id = "tip";
    tip.label = "说明";
    tip.type = UiFrameType::Backdrop;
    tip.texture = std::string(kTooltip);
    tip.width = 0.2;
    tip.height = 0.13;
    tip.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, FramePoint::TopRight, 0.004)};
    tip.visible.binding = Bind(id(kTipShown));
    UiFrame tipText = Text("tip_body", "说明正文", "{" + id(kTipText) + "}", JustifyH::Left,
        Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.01, -0.03), 0.18, 0.09);
    tipText.justifyV = JustifyV::Top;
    tip.children = {Text("tip_head", "说明标题", "{" + id(kTipTitle) + "}", JustifyH::Left,
                        Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, std::nullopt, 0.01, -0.01), 0.18, 0.016),
        std::move(tipText)};

    panel.children = {std::move(title),
        Text("points", "可选数", "{" + id(kPoints) + "}", JustifyH::Right, Anchor(FramePoint::TopRight, UiAnchorTarget::Parent, std::nullopt, -0.036, -0.012), 0.08, 0.016),
        std::move(close), std::move(row), std::move(tip)};
    d.frames = {std::move(entry), std::move(panel)};
    d.events = {UiEvent{id(kOpen), "entry", UiEventKind::Click}, UiEvent{id(kClose), "close", UiEventKind::Click}};
    for (std::size_t c = 0; c < kOptions; ++c) {
        const std::string hit = "hit" + std::to_string(c + 1);
        d.events.push_back(UiEvent{id(kPicks[c]), hit, UiEventKind::Click});
        d.events.push_back(UiEvent{id(kEnters[c]), hit, UiEventKind::Enter});
        d.events.push_back(UiEvent{id(kLeaves[c]), hit, UiEventKind::Leave});
    }
    return d;
}

} // namespace

const std::vector<UiPreset>& UiPresets() {
    static const std::vector<UiPreset> presets{{"castbar", "读条条"}, {"party", "团队框架"}, {"talents", "天赋树"}};
    return presets;
}

Result<UiDescription> NewUiDescriptionFromPreset(std::string_view preset, std::string_view id, std::string_view label) {
    UiDescription description;
    std::string_view name;
    if (preset == "castbar") {
        description = CastBar();
        name = "读条条";
    } else if (preset == "party") {
        description = Party();
        name = "团队框架";
    } else if (preset == "talents") {
        description = Talents();
        name = "天赋树";
    } else {
        Result<UiDescription> failure;
        failure.diagnostics.push_back(detail::MakeUiError("AUI_PRESET_UNKNOWN", "没有界面模板 " + std::string(preset)));
        return failure;
    }
    description.id = std::string(id);
    description.label = std::string(label.empty() ? name : label);
    Result<UiDescription> result;
    result.diagnostics = ValidateUiDescription(description);
    if (result.diagnostics.empty()) result.value = std::move(description);
    return result;
}

} // namespace War3Map::GameUi
