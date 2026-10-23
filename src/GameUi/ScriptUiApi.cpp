#include "ScriptUiApi.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <algorithm>
#include <unordered_map>

namespace War3Map::GameUi::detail {
namespace {

constexpr auto kBlz = UiApiFamily::Blizzard;
constexpr auto kDz = UiApiFamily::Dz;
constexpr auto kMh = UiApiFamily::MemHack;
constexpr auto kTimerDialog = ScriptObjectKind::TimerDialog;
constexpr auto kDialog = ScriptObjectKind::Dialog;
constexpr auto kLeaderboard = ScriptObjectKind::Leaderboard;
constexpr auto kMultiboard = ScriptObjectKind::Multiboard;
constexpr auto kLbRemove = LeaderboardOp::Remove;
constexpr auto kLbLabel = LeaderboardOp::Label;
constexpr auto kLbValue = LeaderboardOp::Value;
constexpr auto kLbItemStyle = LeaderboardOp::ItemStyle;
constexpr auto kLbLabelColor = LeaderboardOp::LabelColor;
constexpr auto kLbValueColor = LeaderboardOp::ValueColor;
constexpr auto kLbSort = LeaderboardOp::Sort;

// 认识的界面函数。设置类调用的第 0 个参数都是框架；参数位置与各接口的公开声明一致。
constexpr CallSpec kCalls[] = {
    {.name = "BlzCreateFrame", .family = kBlz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "BlzCreateSimpleFrame", .family = kBlz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "BlzCreateFrameByType", .family = kBlz, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 3, .parentArg = 2},
    {.name = "BlzGetOriginFrame", .family = kBlz, .kind = CallKind::Origin},
    {.name = "BlzGetFrameByName", .family = kBlz, .kind = CallKind::ByName},
    {.name = "BlzFrameSetAbsPoint", .family = kBlz, .kind = CallKind::SetAbsPoint},
    {.name = "BlzFrameSetPoint", .family = kBlz, .kind = CallKind::SetPoint},
    {.name = "BlzFrameClearAllPoints", .family = kBlz, .kind = CallKind::ClearPoints},
    {.name = "BlzFrameSetAllPoints", .family = kBlz, .kind = CallKind::SetAllPoints},
    {.name = "BlzFrameSetSize", .family = kBlz, .kind = CallKind::SetSize},
    {.name = "BlzFrameSetTexture", .family = kBlz, .kind = CallKind::SetTexture},
    {.name = "BlzFrameSetText", .family = kBlz, .kind = CallKind::SetText},
    {.name = "BlzFrameSetVisible", .family = kBlz, .kind = CallKind::SetVisible},
    {.name = "BlzLoadTOCFile", .family = kBlz, .kind = CallKind::LoadToc},
    {.name = "BlzHideOriginFrames", .family = kBlz, .kind = CallKind::Other},
    {.name = "BlzEnableUIAutoPosition", .family = kBlz, .kind = CallKind::Other},
    {.name = "BlzDestroyFrame", .family = kBlz, .kind = CallKind::FrameOther},
    {.name = "BlzTriggerRegisterFrameEvent", .family = kBlz, .kind = CallKind::FrameOther},
    {.name = "BlzGetTriggerFrame", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameEvent", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameValue", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameText", .family = kBlz, .kind = CallKind::Query},

    {.name = "DzCreateFrame", .family = kDz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "DzCreateSimpleFrame", .family = kDz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "DzCreateFrameByTagName", .family = kDz, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 3, .parentArg = 2},
    {.name = "DzGetGameUI", .family = kDz, .kind = CallKind::GameUi},
    {.name = "DzFrameFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleFrameFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleFontStringFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleTextureFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzFrameGetMinimap", .family = kDz, .kind = CallKind::CodeArea, .area = "Minimap"},
    {.name = "DzFrameGetPortrait", .family = kDz, .kind = CallKind::CodeArea, .area = "Portrait"},
    {.name = "DzFrameGetChatMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "ChatMessage"},
    {.name = "DzFrameGetTopMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "TopMessage"},
    {.name = "DzFrameGetUnitMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "UnitMessage"},
    {.name = "DzFrameGetItemBarButton", .family = kDz, .kind = CallKind::CodeArea, .area = "InventoryButton",
        .indexed = true},
    {.name = "DzFrameGetMinimapButton", .family = kDz, .kind = CallKind::CodeArea, .area = "MinimapButton",
        .indexed = true},
    {.name = "DzFrameSetAbsolutePoint", .family = kDz, .kind = CallKind::SetAbsPoint},
    {.name = "DzFrameSetPoint", .family = kDz, .kind = CallKind::SetPoint},
    {.name = "DzFrameClearAllPoints", .family = kDz, .kind = CallKind::ClearPoints},
    {.name = "DzFrameSetAllPoints", .family = kDz, .kind = CallKind::SetAllPoints},
    {.name = "DzFrameSetSize", .family = kDz, .kind = CallKind::SetSize},
    {.name = "DzFrameSetTexture", .family = kDz, .kind = CallKind::SetTexture},
    {.name = "DzFrameSetText", .family = kDz, .kind = CallKind::SetText},
    {.name = "DzFrameShow", .family = kDz, .kind = CallKind::SetVisible},
    {.name = "DzLoadToc", .family = kDz, .kind = CallKind::LoadToc},
    {.name = "DzFrameHideInterface", .family = kDz, .kind = CallKind::Other},
    {.name = "DzFrameEditBlackBorders", .family = kDz, .kind = CallKind::Other},
    {.name = "DzDestroyFrame", .family = kDz, .kind = CallKind::FrameOther},

    {.name = "MHFrame_Create", .family = kMh, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "MHFrame_CreateEx", .family = kMh, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 2, .parentArg = 3},
    {.name = "MHFrame_CreateSimple", .family = kMh, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "MHUI_GetGameUI", .family = kMh, .kind = CallKind::GameUi},
    {.name = "MHFrame_GetByName", .family = kMh, .kind = CallKind::ByName},
    {.name = "MHFrame_SetAbsolutePoint", .family = kMh, .kind = CallKind::SetAbsPoint},
    {.name = "MHFrame_SetRelativePoint", .family = kMh, .kind = CallKind::SetPoint},
    {.name = "MHFrame_ClearAllPoints", .family = kMh, .kind = CallKind::ClearPoints},
    {.name = "MHFrame_SetAllPoints", .family = kMh, .kind = CallKind::SetAllPoints},
    {.name = "MHFrame_SetSize", .family = kMh, .kind = CallKind::SetSize},
    {.name = "MHFrame_SetWidth", .family = kMh, .kind = CallKind::SetWidth},
    {.name = "MHFrame_SetHeight", .family = kMh, .kind = CallKind::SetHeight},
    {.name = "MHFrame_SetTexture", .family = kMh, .kind = CallKind::SetTexture},
    {.name = "MHFrame_SetText", .family = kMh, .kind = CallKind::SetText},
    {.name = "MHFrame_Hide", .family = kMh, .kind = CallKind::SetHidden},
    {.name = "MHFrame_LoadTOC", .family = kMh, .kind = CallKind::LoadToc},
    {.name = "MHFrame_Destroy", .family = kMh, .kind = CallKind::FrameOther},

    // 原版界面对象：作用对象与取值参数按各函数的公开声明。
    {.name = "CreateTimerDialog", .kind = CallKind::Create, .object = kTimerDialog},
    {.name = "CreateTimerDialogBJ", .kind = CallKind::Create, .object = kTimerDialog, .titleArg = 1, .shown = true},
    {.name = "GetLastCreatedTimerDialogBJ", .kind = CallKind::LastCreated, .object = kTimerDialog},
    {.name = "TimerDialogSetTitle", .kind = CallKind::SetText},
    {.name = "TimerDialogSetTitleBJ", .kind = CallKind::SetText},
    {.name = "TimerDialogDisplay", .kind = CallKind::SetVisible},
    {.name = "TimerDialogDisplayBJ", .kind = CallKind::SetVisible, .targetArg = 1, .valueArg = 0},
    {.name = "DestroyTimerDialog", .kind = CallKind::FrameOther},
    {.name = "DialogCreate", .kind = CallKind::Create, .object = kDialog},
    {.name = "DialogSetMessage", .kind = CallKind::SetText},
    {.name = "DialogSetMessageBJ", .kind = CallKind::SetText},
    {.name = "DialogAddButton", .kind = CallKind::AddButton},
    {.name = "DialogAddButtonBJ", .kind = CallKind::AddButton},
    {.name = "DialogAddButtonWithHotkeyBJ", .kind = CallKind::AddButton},
    {.name = "DialogAddQuitButton", .kind = CallKind::AddButton, .valueArg = 2},
    {.name = "DialogClear", .kind = CallKind::ClearButtons},
    {.name = "DialogClearBJ", .kind = CallKind::ClearButtons},
    {.name = "DialogDisplay", .kind = CallKind::SetVisible, .targetArg = 1, .valueArg = 2},
    {.name = "DialogDisplayBJ", .kind = CallKind::SetVisible, .targetArg = 1, .valueArg = 0},
    {.name = "DialogDestroy", .kind = CallKind::FrameOther},
    {.name = "CreateLeaderboard", .kind = CallKind::Create, .object = kLeaderboard},
    {.name = "CreateLeaderboardBJ", .kind = CallKind::Create, .object = kLeaderboard, .titleArg = 1, .shown = true},
    {.name = "GetLastCreatedLeaderboard", .kind = CallKind::LastCreated, .object = kLeaderboard},
    {.name = "LeaderboardSetLabel", .kind = CallKind::SetText},
    {.name = "LeaderboardSetLabelBJ", .kind = CallKind::SetText, .leaderboardBj = true},
    {.name = "LeaderboardDisplay", .kind = CallKind::SetVisible},
    {.name = "LeaderboardDisplayBJ", .kind = CallKind::SetVisible, .targetArg = 1, .valueArg = 0},
    {.name = "LeaderboardAddItem", .kind = CallKind::Leaderboard, .playerArg = 3},
    {.name = "LeaderboardAddItemBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2, .playerArg = 0,
        .leaderboardBj = true},
    {.name = "LeaderboardRemoveItem", .kind = CallKind::Leaderboard, .leaderboard = kLbRemove, .indexArg = 1},
    {.name = "LeaderboardRemovePlayerItem", .kind = CallKind::Leaderboard, .leaderboard = kLbRemove, .playerArg = 1},
    {.name = "LeaderboardRemovePlayerItemBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .leaderboard = kLbRemove,
        .playerArg = 0, .leaderboardBj = true},
    {.name = "LeaderboardClear", .kind = CallKind::Leaderboard, .leaderboard = LeaderboardOp::Clear},
    {.name = "LeaderboardSetSizeByItemCount", .kind = CallKind::Leaderboard, .leaderboard = LeaderboardOp::Size},
    {.name = "LeaderboardSetStyle", .kind = CallKind::Leaderboard, .leaderboard = LeaderboardOp::Style},
    {.name = "LeaderboardSetStyleBJ", .kind = CallKind::Leaderboard, .leaderboard = LeaderboardOp::Style},
    {.name = "LeaderboardSetItemLabel", .kind = CallKind::Leaderboard, .valueArg = 2, .leaderboard = kLbLabel,
        .indexArg = 1},
    {.name = "LeaderboardSetItemValue", .kind = CallKind::Leaderboard, .valueArg = 2, .leaderboard = kLbValue,
        .indexArg = 1},
    {.name = "LeaderboardSetItemStyle", .kind = CallKind::Leaderboard, .valueArg = 2, .leaderboard = kLbItemStyle,
        .indexArg = 1},
    {.name = "LeaderboardSetItemLabelColor", .kind = CallKind::Leaderboard, .valueArg = 2,
        .leaderboard = kLbLabelColor, .indexArg = 1},
    {.name = "LeaderboardSetItemValueColor", .kind = CallKind::Leaderboard, .valueArg = 2,
        .leaderboard = kLbValueColor, .indexArg = 1},
    {.name = "LeaderboardSetLabelColor", .kind = CallKind::Leaderboard, .leaderboard = kLbLabelColor},
    {.name = "LeaderboardSetValueColor", .kind = CallKind::Leaderboard, .leaderboard = kLbValueColor},
    {.name = "LeaderboardSetLabelColorBJ", .kind = CallKind::Leaderboard, .percent = true,
        .leaderboard = kLbLabelColor},
    {.name = "LeaderboardSetValueColorBJ", .kind = CallKind::Leaderboard, .percent = true,
        .leaderboard = kLbValueColor},
    {.name = "LeaderboardSetPlayerItemLabelBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2,
        .leaderboard = kLbLabel, .playerArg = 0},
    {.name = "LeaderboardSetPlayerItemValueBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2,
        .leaderboard = kLbValue, .playerArg = 0},
    {.name = "LeaderboardSetPlayerItemStyleBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2,
        .leaderboard = kLbItemStyle, .playerArg = 0},
    {.name = "LeaderboardSetPlayerItemLabelColorBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2,
        .percent = true, .leaderboard = kLbLabelColor, .playerArg = 0},
    {.name = "LeaderboardSetPlayerItemValueColorBJ", .kind = CallKind::Leaderboard, .targetArg = 1, .valueArg = 2,
        .percent = true, .leaderboard = kLbValueColor, .playerArg = 0},
    {.name = "LeaderboardSortItemsByValue", .kind = CallKind::Leaderboard, .leaderboard = kLbSort},
    {.name = "LeaderboardSortItemsByPlayer", .kind = CallKind::Leaderboard, .leaderboard = kLbSort,
        .sort = LeaderboardSort::Player},
    {.name = "LeaderboardSortItemsByLabel", .kind = CallKind::Leaderboard, .leaderboard = kLbSort,
        .sort = LeaderboardSort::Label},
    {.name = "LeaderboardSortItemsBJ", .kind = CallKind::Leaderboard, .leaderboard = kLbSort,
        .sort = LeaderboardSort::ByArgument},
    {.name = "LeaderboardSortItemsByPlayerBJ", .kind = CallKind::Leaderboard, .leaderboard = kLbSort,
        .sort = LeaderboardSort::Player},
    {.name = "LeaderboardSortItemsByLabelBJ", .kind = CallKind::Leaderboard, .leaderboard = kLbSort,
        .sort = LeaderboardSort::Label},
    {.name = "LeaderboardGetItemCount", .kind = CallKind::Query},
    {.name = "LeaderboardGetItemCountBJ", .kind = CallKind::Query},
    {.name = "LeaderboardHasPlayerItem", .kind = CallKind::Query},
    {.name = "LeaderboardHasPlayerItemBJ", .kind = CallKind::Query},
    {.name = "LeaderboardGetPlayerIndex", .kind = CallKind::Query},
    {.name = "LeaderboardGetPlayerIndexBJ", .kind = CallKind::Query},
    {.name = "LeaderboardGetIndexedPlayerBJ", .kind = CallKind::Query},
    {.name = "LeaderboardGetLabelText", .kind = CallKind::Query},
    {.name = "IsLeaderboardDisplayed", .kind = CallKind::Query},
    {.name = "PlayerGetLeaderboard", .kind = CallKind::Query},
    {.name = "PlayerGetLeaderboardBJ", .kind = CallKind::Query},
    {.name = "PlayerSetLeaderboard", .kind = CallKind::Other},
    {.name = "ForceSetLeaderboardBJ", .kind = CallKind::Other},
    {.name = "DestroyLeaderboard", .kind = CallKind::FrameOther},
    {.name = "DestroyLeaderboardBJ", .kind = CallKind::FrameOther},
    {.name = "CreateMultiboard", .kind = CallKind::Create, .object = kMultiboard},
    {.name = "CreateMultiboardBJ", .kind = CallKind::Create, .object = kMultiboard, .titleArg = 2, .shown = true,
        .rowsArg = 1, .columnsArg = 0},
    {.name = "GetLastCreatedMultiboard", .kind = CallKind::LastCreated, .object = kMultiboard},
    {.name = "MultiboardSetTitleText", .kind = CallKind::SetText},
    {.name = "MultiboardDisplay", .kind = CallKind::SetVisible},
    {.name = "MultiboardDisplayBJ", .kind = CallKind::SetVisible, .targetArg = 1, .valueArg = 0},
    {.name = "MultiboardSetRowCount", .kind = CallKind::BoardRows},
    {.name = "MultiboardSetColumnCount", .kind = CallKind::BoardColumns},
    {.name = "MultiboardMinimize", .kind = CallKind::BoardMinimize},
    {.name = "MultiboardMinimizeBJ", .kind = CallKind::BoardMinimize, .targetArg = 1, .valueArg = 0},
    {.name = "MultiboardSetItemsValue", .kind = CallKind::BoardItems, .property = BoardProperty::Value},
    {.name = "MultiboardSetItemsStyle", .kind = CallKind::BoardItems, .property = BoardProperty::Style},
    {.name = "MultiboardSetItemsWidth", .kind = CallKind::BoardItems, .property = BoardProperty::Width},
    {.name = "MultiboardSetItemsIcon", .kind = CallKind::BoardItems, .property = BoardProperty::Icon},
    {.name = "MultiboardSetItemsValueColor", .kind = CallKind::BoardItems, .property = BoardProperty::Color},
    {.name = "MultiboardGetItem", .kind = CallKind::BoardGetItem},
    {.name = "MultiboardReleaseItem", .kind = CallKind::Query},
    {.name = "MultiboardSetItemValue", .kind = CallKind::BoardItems, .board = BoardScope::Item},
    {.name = "MultiboardSetItemStyle", .kind = CallKind::BoardItems, .board = BoardScope::Item,
        .property = BoardProperty::Style},
    {.name = "MultiboardSetItemWidth", .kind = CallKind::BoardItems, .board = BoardScope::Item,
        .property = BoardProperty::Width},
    {.name = "MultiboardSetItemIcon", .kind = CallKind::BoardItems, .board = BoardScope::Item,
        .property = BoardProperty::Icon},
    {.name = "MultiboardSetItemValueColor", .kind = CallKind::BoardItems, .board = BoardScope::Item,
        .property = BoardProperty::Color},
    {.name = "MultiboardSetItemValueBJ", .kind = CallKind::BoardItems, .valueArg = 3, .board = BoardScope::ColumnRow},
    {.name = "MultiboardSetItemStyleBJ", .kind = CallKind::BoardItems, .valueArg = 3, .board = BoardScope::ColumnRow,
        .property = BoardProperty::Style},
    {.name = "MultiboardSetItemWidthBJ", .kind = CallKind::BoardItems, .valueArg = 3, .board = BoardScope::ColumnRow,
        .property = BoardProperty::Width, .percent = true},
    {.name = "MultiboardSetItemIconBJ", .kind = CallKind::BoardItems, .valueArg = 3, .board = BoardScope::ColumnRow,
        .property = BoardProperty::Icon},
    {.name = "MultiboardSetItemColorBJ", .kind = CallKind::BoardItems, .valueArg = 3, .board = BoardScope::ColumnRow,
        .property = BoardProperty::Color, .percent = true},
    {.name = "MultiboardGetRowCount", .kind = CallKind::Query},
    {.name = "MultiboardGetColumnCount", .kind = CallKind::Query},
    {.name = "MultiboardGetTitleText", .kind = CallKind::Query},
    {.name = "IsMultiboardDisplayed", .kind = CallKind::Query},
    {.name = "IsMultiboardMinimized", .kind = CallKind::Query},
    {.name = "MultiboardSetTitleTextColor", .kind = CallKind::FrameOther},
    {.name = "MultiboardSetTitleTextColorBJ", .kind = CallKind::FrameOther},
    {.name = "MultiboardClear", .kind = CallKind::FrameOther},
    {.name = "MultiboardSuppressDisplay", .kind = CallKind::Other},
    {.name = "MultiboardAllowDisplayBJ", .kind = CallKind::Other},
    {.name = "DestroyMultiboard", .kind = CallKind::FrameOther},
};

// 表外但属于界面函数族的名字：Get/Is 开头视为只读，其余列为不认识。
struct FamilyPrefix {
    std::string_view prefix;
    UiApiFamily family;
};
constexpr FamilyPrefix kPrefixes[] = {
    {"BlzFrame", kBlz}, {"DzFrame", kDz}, {"MHFrame_", kMh}, {"MHUI_", kMh}};

struct OriginEntry {
    std::string_view constant;
    std::string_view area;
    bool indexed = false;
};
constexpr OriginEntry kOrigins[] = {
    {"ORIGIN_FRAME_GAME_UI", "GameUI"},
    {"ORIGIN_FRAME_WORLD_FRAME", "WorldFrame"},
    {"ORIGIN_FRAME_HERO_BAR", "HeroBar"},
    {"ORIGIN_FRAME_PORTRAIT", "Portrait"},
    {"ORIGIN_FRAME_MINIMAP", "Minimap"},
    {"ORIGIN_FRAME_MINIMAP_BUTTON", "MinimapButton", true},
    {"ORIGIN_FRAME_ITEM_BUTTON", "InventoryButton", true},
    {"ORIGIN_FRAME_CHAT_MSG", "ChatMessage"},
    {"ORIGIN_FRAME_UNIT_MSG", "UnitMessage"},
    {"ORIGIN_FRAME_TOP_MSG", "TopMessage"},
};

std::optional<std::string> IndexedArea(std::string_view area, bool indexed, std::optional<float> index) {
    std::string id(area);
    if (indexed) {
        if (!index || *index < 0.0f) return std::nullopt;
        id += std::to_string(static_cast<int>(*index) + 1);
    }
    return id;
}

} // namespace

std::optional<CallSpec> LookupCall(std::string_view name) {
    static const auto table = [] {
        std::unordered_map<std::string_view, const CallSpec*> map;
        for (const auto& spec : kCalls) map.emplace(spec.name, &spec);
        return map;
    }();
    if (const auto found = table.find(name); found != table.end()) return *found->second;
    for (const auto& prefix : kPrefixes) {
        if (!name.starts_with(prefix.prefix)) continue;
        const std::string_view rest = name.substr(prefix.prefix.size());
        CallSpec spec;
        spec.name = name;
        spec.family = prefix.family;
        spec.kind = rest.starts_with("Get") || rest.starts_with("Is") ? CallKind::Query : CallKind::Unknown;
        return spec;
    }
    return std::nullopt;
}

bool IsSetter(CallKind kind) noexcept {
    return kind >= CallKind::SetAbsPoint && kind <= CallKind::Leaderboard;
}

std::vector<ArgumentSlot> ValueArguments(const CallSpec& spec) {
    using T = ArgumentType;
    const auto at = [&](int offset) { return static_cast<std::uint32_t>(std::max(0, spec.valueArg + offset)); };
    switch (spec.kind) {
    case CallKind::SetAbsPoint: return {{1, T::Integer}, {2, T::Real}, {3, T::Real}};
    case CallKind::SetPoint: return {{1, T::Integer}, {3, T::Integer}, {4, T::Real}, {5, T::Real}};
    case CallKind::SetSize: return {{1, T::Real}, {2, T::Real}};
    case CallKind::SetWidth: case CallKind::SetHeight: return {{1, T::Real}};
    case CallKind::SetTexture: return {{1, T::String}};
    case CallKind::SetText: case CallKind::AddButton: return {{at(0), T::String}};
    case CallKind::SetVisible: case CallKind::SetHidden: case CallKind::BoardMinimize: return {{at(0), T::Boolean}};
    case CallKind::BoardRows: case CallKind::BoardColumns: return {{at(0), T::Integer}};
    case CallKind::BoardItems: {
        std::vector<ArgumentSlot> slots;
        if (spec.board == BoardScope::ColumnRow) slots = {{1, T::Integer}, {2, T::Integer}};
        switch (spec.property) {
        case BoardProperty::Value: case BoardProperty::Icon: slots.push_back({at(0), T::String}); break;
        case BoardProperty::Width: slots.push_back({at(0), T::Real}); break;
        case BoardProperty::Style: slots.push_back({at(0), T::Boolean}); slots.push_back({at(1), T::Boolean}); break;
        case BoardProperty::Color:
            for (int i = 0; i < 4; ++i) slots.push_back({at(i), spec.percent ? T::Real : T::Integer});
            break;
        }
        return slots;
    }
    case CallKind::Leaderboard: {
        std::vector<ArgumentSlot> slots;
        if (spec.indexArg >= 0) slots.push_back({static_cast<std::uint32_t>(spec.indexArg), T::Integer});
        if (spec.playerArg >= 0) slots.push_back({static_cast<std::uint32_t>(spec.playerArg), T::Player});
        const auto add = [&](int count, ArgumentType type) {
            for (int i = 0; i < count; ++i) slots.push_back({at(i), type});
        };
        switch (spec.leaderboard) {
        case LeaderboardOp::Add: add(1, T::String); slots.push_back({at(1), T::Integer}); break;
        case LeaderboardOp::Remove: case LeaderboardOp::Clear: break;
        case LeaderboardOp::Size: case LeaderboardOp::Value: add(1, T::Integer); break;
        case LeaderboardOp::Label: add(1, T::String); break;
        case LeaderboardOp::Style: add(4, T::Boolean); break;
        case LeaderboardOp::ItemStyle: add(3, T::Boolean); break;
        case LeaderboardOp::LabelColor: case LeaderboardOp::ValueColor: add(4, spec.percent ? T::Real : T::Integer); break;
        case LeaderboardOp::Sort:
            if (spec.sort == LeaderboardSort::ByArgument) add(1, T::Integer);
            slots.push_back({at(spec.sort == LeaderboardSort::ByArgument ? 1 : 0), T::Boolean});
            break;
        }
        std::sort(slots.begin(), slots.end(), [](const ArgumentSlot& a, const ArgumentSlot& b) { return a.index < b.index; });
        return slots;
    }
    default: return {};
    }
}

std::optional<std::string> OriginArea(std::string_view constant, std::optional<float> index) {
    for (const auto& known : kOrigins) {
        if (known.constant == constant) return IndexedArea(known.area, known.indexed, index);
    }
    return std::nullopt;
}

std::optional<std::string> CodeAreaOf(const CallSpec& spec, std::optional<float> index) {
    if (spec.kind != CallKind::CodeArea) return std::nullopt;
    return IndexedArea(spec.area, spec.indexed, index);
}

std::optional<ScriptFrameRef> FrameRefOfCall(const CallSpec& spec, const std::vector<std::optional<ScriptValue>>& args) {
    const auto argument = [&](std::size_t i) { return i < args.size() ? args[i] : std::nullopt; };
    std::optional<std::string> area;
    switch (spec.kind) {
    case CallKind::GameUi:
        area = std::string(kGameCodeRoot);
        break;
    case CallKind::Origin: {
        const auto origin = argument(0);
        if (origin && origin->kind == ScriptValue::Kind::Origin) area = OriginArea(origin->text, NumberOf(argument(1)));
        break;
    }
    case CallKind::CodeArea:
        area = CodeAreaOf(spec, NumberOf(argument(0)));
        break;
    case CallKind::ByName:
        if (auto name = StringOf(argument(0))) return ScriptFrameRef{ScriptFrameTarget::Named, std::move(*name), 0};
        return std::nullopt;
    default:
        return std::nullopt;
    }
    if (!area) return std::nullopt;
    return ScriptFrameRef{ScriptFrameTarget::CodeArea, std::move(*area), 0};
}

bool ApplyFrameCall(ScriptFrame& frame, const CallSpec& spec, const std::vector<std::optional<ScriptValue>>& args,
    const ScriptFrameRef& relative, const TriggerStrings& strings) {
    const CallKind kind = spec.kind;
    const auto argument = [&](std::size_t i) { return i < args.size() ? args[i] : std::nullopt; };
    const auto value = [&] { return spec.valueArg < 0 ? std::nullopt : argument(static_cast<std::size_t>(spec.valueArg)); };
    const auto upsert = [&](ScriptAnchor anchor) {
        for (auto& existing : frame.anchors) {
            if (existing.point == anchor.point) {
                existing = std::move(anchor);
                return;
            }
        }
        frame.anchors.push_back(std::move(anchor));
    };
    switch (kind) {
    case CallKind::SetAbsPoint: {
        const auto point = PointOf(argument(1));
        const auto x = NumberOf(argument(2));
        const auto y = NumberOf(argument(3));
        if (!point || !x || !y) return false;
        upsert(ScriptAnchor{*point, ScriptFrameRef{ScriptFrameTarget::Screen, {}, 0}, FramePoint::BottomLeft, *x, *y});
        return true;
    }
    case CallKind::SetPoint: {
        const auto point = PointOf(argument(1));
        const auto relativePoint = PointOf(argument(3));
        const auto x = NumberOf(argument(4));
        const auto y = NumberOf(argument(5));
        if (!point || relative.target == ScriptFrameTarget::Unknown || !relativePoint || !x || !y) return false;
        upsert(ScriptAnchor{*point, relative, *relativePoint, *x, *y});
        return true;
    }
    case CallKind::ClearPoints:
        frame.anchors.clear();
        frame.clearPoints = true;
        return true;
    case CallKind::SetAllPoints:
        if (relative.target == ScriptFrameTarget::Unknown) return false;
        upsert(ScriptAnchor{FramePoint::TopLeft, relative, FramePoint::TopLeft, 0.0f, 0.0f});
        upsert(ScriptAnchor{FramePoint::BottomRight, relative, FramePoint::BottomRight, 0.0f, 0.0f});
        return true;
    case CallKind::SetSize: {
        const auto width = NumberOf(argument(1));
        const auto height = NumberOf(argument(2));
        if (!width || !height) return false;
        frame.width = width;
        frame.height = height;
        return true;
    }
    case CallKind::SetWidth:
        if (const auto width = NumberOf(argument(1))) {
            frame.width = width;
            return true;
        }
        return false;
    case CallKind::SetHeight:
        if (const auto height = NumberOf(argument(1))) {
            frame.height = height;
            return true;
        }
        return false;
    case CallKind::SetTexture:
        if (auto path = StringOf(argument(1))) {
            frame.texture = std::move(*path);
            return true;
        }
        return false;
    case CallKind::SetText:
        if (auto text = StringOf(value())) {
            frame.text = std::move(*text);
            LeaderboardLabelChanged(frame, spec, strings);
            return true;
        }
        return false;
    case CallKind::SetVisible:
    case CallKind::SetHidden:
        if (const auto flag = BooleanOf(value())) {
            frame.visible = kind == CallKind::SetVisible ? *flag : !*flag;
            return true;
        }
        return false;
    case CallKind::AddButton: {
        auto text = StringOf(value());
        frame.buttons.push_back(text.value_or(std::string()));
        return text.has_value();
    }
    case CallKind::ClearButtons:
        frame.buttons.clear();
        return true;
    case CallKind::BoardRows:
    case CallKind::BoardColumns:
    case CallKind::BoardMinimize:
    case CallKind::BoardItems:
        return ApplyBoardCall(frame, spec, args);
    case CallKind::Leaderboard:
        return ApplyLeaderboardCall(frame, spec, args, strings);
    default:
        return false;
    }
}

} // namespace War3Map::GameUi::detail
