#include "War3MapCore/GameUi/GameCodeLayout.h"

#include "War3MapCore/Text/TextUtil.h"

#include <string>

// 以下数值取自 1.27a 游戏程序中 CGameUI 初始化及各界面类构造时的定位调用（锚点、相对点、偏移、尺寸），
// 屏幕原点在左下。只收录已确认的项；其余由游戏代码定位的界面在取得前保持“位置未知”。
namespace War3Map::GameUi {
namespace {

FrameAnchor Anchor(FramePoint point, FramePoint relativePoint, float x, float y, std::string relative = {}) {
    return FrameAnchor{point, std::move(relative), relativePoint, x, y};
}

GameCodeArea Area(std::string_view id, std::string_view label, std::string_view className,
    std::vector<FrameAnchor> anchors, std::optional<float> width = std::nullopt,
    std::optional<float> height = std::nullopt) {
    return GameCodeArea{id, label, className, std::move(anchors), width, height};
}

std::vector<GameCodeArea> BuildAreas() {
    using P = FramePoint;
    std::vector<GameCodeArea> areas;
    areas.push_back(Area("WorldFrame", "世界画面", "CWorldFrameWar3",
        {Anchor(P::BottomLeft, P::BottomLeft, 0.0f, 0.13f), Anchor(P::TopRight, P::TopRight, 0.0f, -0.02f)}));
    areas.push_back(Area("Minimap", "小地图", "CMinimap",
        {Anchor(P::BottomLeft, P::BottomLeft, 0.009375f, 0.006875f),
            Anchor(P::TopRight, P::BottomLeft, 0.14875f, 0.145f)}));
    // 五个小地图按钮的左上角；第一个是信号按钮（游戏为它设置了 MiniMapSignalButton 贴图）。
    constexpr float kMinimapButtonY[] = {0.14375f, 0.121875f, 0.1f, 0.0775f, 0.05f};
    constexpr std::string_view kMinimapButtonIds[] = {
        "MinimapButton1", "MinimapButton2", "MinimapButton3", "MinimapButton4", "MinimapButton5"};
    constexpr std::string_view kMinimapButtonLabels[] = {
        "小地图信号按钮", "小地图按钮 2", "小地图按钮 3", "小地图按钮 4", "小地图按钮 5"};
    for (int i = 0; i < 5; ++i) {
        const float x = i == 4 ? 0.155f : 0.154375f;
        areas.push_back(Area(kMinimapButtonIds[i], kMinimapButtonLabels[i], "CSimpleButton",
            {Anchor(P::TopLeft, P::BottomLeft, x, kMinimapButtonY[i])}, 0.023125f, 0.020625f));
    }
    areas.push_back(Area("Portrait", "头像", "CPortraitButton",
        {Anchor(P::BottomLeft, P::BottomLeft, 0.2075f, 0.030625f)}, 0.09f, 0.0856f));
    areas.push_back(Area("InfoBar", "信息面板", "CInfoBar",
        {Anchor(P::BottomLeft, P::BottomLeft, 0.309375f, 0.0f)}, 0.188125f, 0.1140625f));
    // 物品栏 2 列 × 3 行，相对信息面板右上角。
    constexpr std::string_view kInventoryIds[] = {
        "InventoryButton1", "InventoryButton2", "InventoryButton3", "InventoryButton4", "InventoryButton5",
        "InventoryButton6"};
    constexpr std::string_view kInventoryLabels[] = {
        "物品栏 1", "物品栏 2", "物品栏 3", "物品栏 4", "物品栏 5", "物品栏 6"};
    for (int i = 0; i < 6; ++i) {
        const float x = 0.01875f + static_cast<float>(i & 1) * 0.04f;
        const float y = -0.0021875f - static_cast<float>(i >> 1) * 0.038125f;
        areas.push_back(Area(kInventoryIds[i], kInventoryLabels[i], "",
            {Anchor(P::TopLeft, P::TopRight, x, y, "InfoBar")}, 0.03125f, 0.03125f));
    }
    areas.push_back(Area("InventoryTitle", "物品栏标题", "",
        {Anchor(P::TopLeft, P::TopRight, 0.015625f, 0.0165625f, "InfoBar")}, 0.076875f, 0.01125f));
    areas.push_back(Area("InventoryCover", "物品栏遮挡", "",
        {Anchor(P::BottomRight, P::BottomLeft, 0.6f, 0.0f)}, 0.128f, 0.175f));
    areas.push_back(Area("CommandBar", "命令面板", "CCommandBar",
        {Anchor(P::BottomRight, P::BottomRight, -0.007375f, 0.002f)}, 0.1745f, 0.129f));
    areas.push_back(Area("HeroBar", "英雄栏", "CHeroBar",
        {Anchor(P::TopLeft, P::TopLeft, 0.00375f, -0.02625f)}, 0.038f, 0.35825f));
    areas.push_back(Area("IdleWorkerButton", "闲置工人", "CPeonBar",
        {Anchor(P::BottomLeft, P::BottomLeft, 0.00375f, 0.17f)}, 0.038f, 0.038f));
    areas.push_back(Area("TimeOfDay", "昼夜指示", "CTimeOfDayIndicator",
        {Anchor(P::Top, P::Top, 0.0f, 0.0f)}, 0.04125f, 0.04125f));
    areas.push_back(Area("Message", "提示消息", "",
        {Anchor(P::Bottom, P::Bottom, 0.0f, 0.18f)}, 0.38f, 0.05f));
    areas.push_back(Area("UnitMessage", "单位消息", "",
        {Anchor(P::Bottom, P::Top, -0.098125f, 0.0005f, "Message")}, 0.5f, 0.25f));
    areas.push_back(Area("TopMessage", "顶部消息", "",
        {Anchor(P::Top, P::Top, 0.0f, -0.026f)}, 0.38f, 0.05f));
    areas.push_back(Area("ChatMessage", "聊天消息", "",
        {Anchor(P::Top, P::Top, 0.0f, 0.0f), Anchor(P::Bottom, P::Bottom, 0.0f, 0.2f),
            Anchor(P::Left, P::Left, 0.00625f, 0.0f), Anchor(P::Right, P::Right, -0.02f, 0.0f)}));
    areas.push_back(Area("ChatEditBar", "聊天输入框", "CChatEditBar",
        {Anchor(P::Left, P::BottomLeft, 0.2f, 0.145f), Anchor(P::Right, P::BottomRight, -0.2f, 0.145f)},
        std::nullopt, 0.03f));
    return areas;
}

} // namespace

const std::vector<GameCodeArea>& GameCodeAreas() {
    static const std::vector<GameCodeArea> areas = BuildAreas();
    return areas;
}

std::optional<RootPlacement> KnownRootPlacement(std::string_view templateName) {
    using P = FramePoint;
    const std::string key = Text::LowerAscii(templateName);
    const auto at = [](P point, P relativePoint, float x, float y) {
        RootPlacement placement;
        placement.anchors.push_back(Anchor(point, relativePoint, x, y));
        return placement;
    };
    // ConsoleUI 在 fdf 中没有尺寸，游戏创建后铺满整个屏幕。
    if (key == "consoleui") return RootPlacement{true, {}, std::nullopt, std::nullopt};
    if (key == "resourcebarframe") return at(P::TopRight, P::TopRight, 0.0f, 0.0f);
    if (key == "upperbuttonbarframe") return at(P::TopLeft, P::TopLeft, 0.0f, 0.0f);
    if (key == "escmenubackdrop") return at(P::Center, P::Center, 0.0f, 0.06f);
    // 游戏把 Esc 菜单背景框调成当前子面板的尺寸，菜单面板随背景框；默认子面板 MainPanel 为 0.288×0.384。
    // 子面板按钮相对 EscMenuMainPanel 的左下/右下排布、总宽等于子面板宽，可佐证面板与背景框同框。
    if (key == "escmenumainpanel") {
        auto placement = at(P::Center, P::Center, 0.0f, 0.06f);
        placement.width = 0.288f;
        placement.height = 0.384f;
        return placement;
    }
    if (key == "questdialog") return at(P::Center, P::Center, 0.0f, 0.064f);
    if (key == "chatdialog" || key == "logdialog") return at(P::Center, P::Center, 0.0f, 0.05f);
    return std::nullopt;
}

} // namespace War3Map::GameUi
