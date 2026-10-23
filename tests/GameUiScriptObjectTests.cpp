#include "War3MapCore/GameUi/FrameDefinition.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/GameUi/ScriptSimulation.h"
#include "GameUiTestSupport.h"

#include <cstring>
#include <iostream>
#include <string>

// 地图脚本创建的原版界面对象：计时器窗口、对话框、多面板与排行榜的识别、模拟和按游戏规则的排布。
namespace {

using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::GameUi::Testing;

std::uint32_t ChildNamed(const FrameTree& tree, std::uint32_t node, std::string_view name) {
    for (const std::uint32_t child : tree.Nodes()[node].children) {
        if (tree.Nodes()[child].name == name) return child;
        if (const std::uint32_t found = ChildNamed(tree, child, name); found != kNoNode) return found;
    }
    return kNoNode;
}

const FrameNode& Descendant(const FrameTree& tree, std::string_view root, std::string_view name) {
    const std::uint32_t found = ChildNamed(tree, *tree.FindByName(root), name);
    if (found == kNoNode) throw std::runtime_error("descendant missing: " + std::string(name));
    return tree.Nodes()[found];
}

void TestScriptClassicObjects() {
    // 原创脚本：BJ 计时器窗口（创建即显示，经 GetLastCreated 存入变量）、原生计时器窗口与对话框。
    const MapScriptText script{"war3map.j", ScriptLanguage::Jass, R"(globals
    timerdialog array udg_Bars
    timerdialog udg_Wait = null
    dialog udg_Pick = null
    timer udg_T = null
endglobals
function InitBoards takes nothing returns nothing
    call CreateTimerDialogBJ(udg_T, "TRIGSTR_007")
    set udg_Bars[0] = GetLastCreatedTimerDialogBJ()
    set udg_Wait = CreateTimerDialog(udg_T)
    call TimerDialogSetTitle(udg_Wait, "Wait")
    set udg_Pick = DialogCreate()
    call DialogSetMessage(udg_Pick, "Pick")
    call DialogAddButton(udg_Pick, "TRIGSTR_008", 0)
    call DialogAddButton(udg_Pick, I2S(2), 0)
    call DialogDisplay(Player(0), udg_Pick, true)
endfunction
function Later takes nothing returns nothing
    call TimerDialogDisplay(udg_Wait, true)
endfunction
)"};
    auto plan = FindScriptFrames({script});
    Require(plan.frames.size() == 3, "three classic objects");
    const ScriptFrame& boss = FrameCreatedAt(plan, 8);
    Require(boss.object == ScriptObjectKind::TimerDialog && boss.variable == "udg_Bars[0]" && boss.name == "udg_Bars[0]" &&
            boss.templateName == "TimerDialog",
        "BJ timer dialog bound through GetLastCreated");
    Require(boss.visible && *boss.visible && boss.text && *boss.text == "TRIGSTR_007", "BJ creation shows with its title");
    const ScriptFrame& wait = FrameCreatedAt(plan, 10);
    Require(wait.visible && !*wait.visible && wait.text && *wait.text == "Wait" && wait.runtimeCalls == 1,
        "native timer dialog stays hidden until displayed");
    const ScriptFrame& pick = FrameCreatedAt(plan, 12);
    Require(pick.object == ScriptObjectKind::Dialog && pick.visible && *pick.visible && pick.text && *pick.text == "Pick",
        "dialog displayed in the same block; display takes the dialog as its second argument");
    Require(pick.buttons == std::vector<std::string>{"TRIGSTR_008", ""} && pick.dynamic.size() == 1,
        "buttons kept in order; runtime text reported");

    plan.strings[7] = "Boss";
    plan.strings[8] = "Easy";
    const std::string text =
        "Frame \"FRAME\" \"TimerDialog\" { Width 0.17, Height 0.03,\n"
        "    Frame \"TEXT\" \"TimerDialogValue\" { Anchor RIGHT, -0.01, 0.0, }\n"
        "    Frame \"TEXT\" \"TimerDialogTitle\" { Anchor LEFT, 0.01, 0.0, }\n"
        "}\n"
        "Frame \"DIALOG\" \"ScriptDialog\" { Width 0.288, Height 0.112,\n"
        "    Frame \"TEXT\" \"ScriptDialogText\" { Font \"MasterFont\", 0.015, Anchor TOP, 0.0, -0.03, }\n"
        "}\n"
        "Frame \"GLUETEXTBUTTON\" \"ScriptDialogButton\" { Width 0.228, Height 0.035,\n"
        "    Frame \"TEXT\" \"ScriptDialogButtonText\" { SetAllPoints, }\n"
        "}\n";
    auto parsed = ParseFdf(text, "UI\\Classic.fdf");
    Require(parsed.Succeeded(), "classic fixture parses");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    FrameTree tree;
    std::vector<Diagnostic> diagnostics;
    (void)AddScriptFrames(tree, library, nullptr, plan, diagnostics);
    (void)tree.Layout();

    // 计时器窗口在世界画面右上角，标题按字符串表显示。
    RequireRect(NodeNamed(tree, "udg_Bars[0]"), 0.625f, 0.535f, 0.795f, 0.565f, "timer dialog at its game position");
    Require(NodeNamed(tree, "TimerDialogTitle").text->text == "Boss", "title resolved from the string table");
    Require(NodeNamed(tree, "udg_Wait").hidden && !NodeNamed(tree, "udg_Bars[0]").hidden, "visibility from creation");

    // 对话框：消息 1 行（0.015）+ 2 个按钮，合计 0.159，取到 0.192；上下边距平分 0.0465。
    RequireRect(NodeNamed(tree, "udg_Pick"), 0.256f, 0.259f, 0.544f, 0.451f, "dialog grows to the next 0.048 step");
    std::vector<const FrameNode*> buttons;
    for (const auto& node : tree.Nodes()) {
        if (node.name == "ScriptDialogButton") buttons.push_back(&node);
    }
    Require(buttons.size() == 2, "one button node per added button");
    RequireRect(*buttons[0], 0.286f, 0.3435f, 0.514f, 0.3785f, "first button below the message");
    RequireRect(*buttons[1], 0.286f, 0.3055f, 0.514f, 0.3405f, "next button 0.003 lower");
    const FrameNode& label = tree.Nodes()[buttons[0]->children.front()];
    Require(label.text && label.text->text == "Easy", "button text resolved");
}

// 原创多面板脚本：BJ 创建并按列、按格子设置，原生创建并最小化；另有计时器窗口与排行榜参与右上角排开。
const MapScriptText kBoardScript{"war3map.j", ScriptLanguage::Jass, R"(globals
    multiboard udg_Board = null
    multiboard udg_Raw = null
    timerdialog udg_Clock = null
    leaderboard udg_Rank = null
    timer udg_T = null
    integer udg_Count = 0
endglobals
function InitBoards takes nothing returns nothing
    local multiboarditem cell
    call CreateMultiboardBJ(2, 3, "TRIGSTR_009")
    set udg_Board = GetLastCreatedMultiboard()
    call MultiboardSetItemsStyle(udg_Board, true, false)
    call MultiboardSetItemWidthBJ(udg_Board, 1, 0, 10.00)
    call MultiboardSetItemValueBJ(udg_Board, 1, 1, "Name")
    call MultiboardSetItemColorBJ(udg_Board, 2, 0, 100, 0, 0, 0)
    set cell = MultiboardGetItem(udg_Board, 2, 1)
    call MultiboardSetItemValue(cell, "7")
    call MultiboardSetItemStyle(cell, true, true)
    call MultiboardSetItemIcon(cell, "Icons/Test.blp")
    call MultiboardReleaseItem(cell)
    call MultiboardSetItemValue(MultiboardGetItem(udg_Board, 0, 1), I2S(udg_Count))
    set udg_Raw = CreateMultiboard()
    call MultiboardSetRowCount(udg_Raw, 1)
    call MultiboardSetColumnCount(udg_Raw, 1)
    call MultiboardMinimize(udg_Raw, true)
    call CreateTimerDialogBJ(udg_T, "Wave")
    set udg_Clock = GetLastCreatedTimerDialogBJ()
    call CreateLeaderboardBJ(GetPlayersAll(), "Rank")
    set udg_Rank = GetLastCreatedLeaderboard()
endfunction
function UpdateBoard takes integer row returns nothing
    local multiboarditem cell = MultiboardGetItem(udg_Board, row, 1)
    call MultiboardSetItemValue(cell, I2S(udg_Count))
    call MultiboardSetItemValueBJ(udg_Board, 1, 0, "P")
endfunction
)"};

void TestScriptMultiboard() {
    auto plan = FindScriptFrames({kBoardScript});
    Require(plan.frames.size() == 4, "two multiboards, a timer dialog and a leaderboard");
    const ScriptFrame& frame = FrameCreatedAt(plan, 11);
    Require(frame.object == ScriptObjectKind::Multiboard && frame.variable == "udg_Board" && frame.visible && *frame.visible &&
            frame.text && *frame.text == "TRIGSTR_009",
        "BJ multiboard shown with its title");
    Require(frame.board && frame.board->rows == 3 && frame.board->columns == 2, "BJ creation sets rows and columns");
    const ScriptBoard& board = *frame.board;
    Require(!board.defaults.showIcon && board.defaults.showValue, "SetItemsStyle changes the defaults");
    for (std::uint32_t r = 0; r < 3; ++r) {
        Require(Near(board.At(r, 0)->width, 0.1f) && Near(board.At(r, 1)->width, 0.03f), "BJ width is a percentage per column");
        Require((board.At(r, 1)->color == std::array<std::uint8_t, 4>{255, 0, 0, 255}) &&
                (board.At(r, 0)->color == std::array<std::uint8_t, 4>{255, 255, 255, 255}),
            "BJ colour converts percentages and transparency");
        Require(board.At(r, 0)->showIcon == false, "style applied to existing items");
    }
    Require(board.At(0, 0)->value == "Name" && board.At(1, 0)->value.empty(), "BJ value addresses column 1, row 1");
    const ScriptBoardItem& seven = *board.At(2, 1);
    Require(seven.value == "7" && seven.showIcon && seven.icon && *seven.icon == "Icons/Test.blp",
        "item handle from MultiboardGetItem");
    Require(board.At(0, 1)->value.empty() && frame.dynamic.size() == 1, "runtime value on an inline item is reported");
    Require(frame.runtimeCalls == 2 && frame.runtimeFunctions == std::vector<std::string>{"UpdateBoard"},
        "item and BJ calls in another function count as runtime changes of the board");
    const ScriptFrame& raw = FrameCreatedAt(plan, 23);
    Require(raw.board && raw.board->rows == 1 && raw.board->columns == 1 && raw.board->minimized && raw.visible &&
            !*raw.visible,
        "native multiboard hidden, minimized");

    // 模拟更新函数：参数行号与全局计数作为输入。
    auto prepared = ScriptSimulator::Prepare({kBoardScript}, plan, "UpdateBoard");
    Require(prepared.value.has_value(), "simulator prepared");
    const auto result = prepared.value->Run({{"row", "1"}, {"udg_Count", "5"}});
    const ScriptBoard& updated = *result.frames[0].board;
    Require(updated.At(1, 1)->value == "5", "item variable bound from a parameter row");
    Require(updated.At(0, 0)->value == "P" && updated.At(2, 0)->value == "P", "BJ row 0 means every row");
    Require(result.touched == std::vector<std::uint32_t>{0}, "only the board touched");

    // 排布：计时器窗口在右上角，多面板在它下面 0.001，排行榜在多面板背板下 0.005。
    plan.strings[9] = "Score";
    const std::string text =
        "Frame \"FRAME\" \"Multiboard\" { Width 0.024, Height 0.024,\n"
        "    Frame \"GLUETEXTBUTTON\" \"MultiboardMinimizeButton\" { SetAllPoints,\n"
        "        ControlBackdrop \"ButtonBackdropTemplate\",\n"
        "        Frame \"BACKDROP\" \"ButtonBackdropTemplate\" { DecorateFileNames, BackdropBackground \"MultiboardMinimizeButtonEnabled\", }\n"
        "    }\n"
        "    Frame \"BACKDROP\" \"MultiboardTitleBackdrop\" { Width 0.2,\n"
        "        SetPoint TOPRIGHT, \"MultiboardMinimizeButton\", TOPLEFT, 0.0057, 0.0,\n"
        "        SetPoint BOTTOMRIGHT, \"MultiboardMinimizeButton\", BOTTOMLEFT, 0.0057, 0.0,\n"
        "        SetAllPoints,\n"
        "    }\n"
        "    Frame \"TEXT\" \"MultiboardTitle\" { Font \"MasterFont\", 0.011,\n"
        "        SetPoint TOPLEFT, \"MultiboardTitleBackdrop\", TOPLEFT, 0.0, 0.0,\n"
        "        SetPoint BOTTOMRIGHT, \"MultiboardTitleBackdrop\", BOTTOMRIGHT, 0.0, 0.0,\n"
        "    }\n"
        "    Frame \"BACKDROP\" \"MultiboardBackdrop\" {\n"
        "        SetPoint TOPRIGHT, \"MultiboardMinimizeButton\", BOTTOMRIGHT, 0.0, 0.0057,\n"
        "        SetPoint TOPLEFT, \"MultiboardTitleBackdrop\", BOTTOMLEFT, 0.0, 0.0057,\n"
        "    }\n"
        "    Frame \"FRAME\" \"MultiboardListContainer\" {\n"
        "        SetPoint TOPLEFT, \"MultiboardBackdrop\", TOPLEFT, 0.001, -0.0048,\n"
        "        SetPoint BOTTOMRIGHT, \"MultiboardBackdrop\", BOTTOMRIGHT, -0.001, 0.001,\n"
        "    }\n"
        "}\n"
        "Frame \"FRAME\" \"TimerDialog\" { Width 0.17, Height 0.03, }\n"
        "Frame \"FRAME\" \"Leaderboard\" { Width 0.17, Height 0.2, }\n";
    auto parsed = ParseFdf(text, "UI\\Boards.fdf");
    Require(parsed.Succeeded(), "board fixture parses");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    UiSkin skin;
    skin.SetGameSkins("[Default]\nMasterFont=Fonts\\Test.ttf\nMultiboardDefaultIcon=UI\\Icons\\Default\n"
                      "MultiboardMinimizeButtonEnabled=UI\\Min\nMultiboardMaximizeButtonEnabled=UI\\Max\n");
    skin.SetMiscUi("[FontHeights]\nLeaderBoard=0.009               // board text\nBroken=abc\n");
    Require(skin.FontHeight("leaderboard") && Near(*skin.FontHeight("LeaderBoard"), 0.009f), "font height before comment");
    Require(!skin.FontHeight("Broken") && !skin.FontHeight("Missing"), "invalid or missing font height");
    FrameTree tree;
    std::vector<Diagnostic> diagnostics;
    (void)AddScriptFrames(tree, library, &skin, plan, diagnostics);
    (void)tree.Layout();

    RequireRect(NodeNamed(tree, "udg_Clock"), 0.625f, 0.535f, 0.795f, 0.565f, "timer dialog alone at the top right");
    RequireRect(NodeNamed(tree, "udg_Board"), 0.771f, 0.510f, 0.795f, 0.534f, "multiboard below the first timer dialog");
    // 列表宽 (0.1+0.002)+(0.03+0.002)=0.134，标题栏宽 0.134-0.024=0.110；SetPoint 在时忽略 SetAllPoints。
    RequireRect(Descendant(tree, "udg_Board", "MultiboardTitleBackdrop"), 0.6667f, 0.510f, 0.7767f, 0.534f,
        "title bar left of the minimize button");
    // 背板高 0.014×3+0.012=0.054。
    RequireRect(Descendant(tree, "udg_Board", "MultiboardBackdrop"), 0.6667f, 0.4617f, 0.795f, 0.5157f,
        "backdrop height follows the rows");
    RequireRect(NodeNamed(tree, "MultiboardItem_r1_c1"), 0.6787f, 0.4969f, 0.7787f, 0.5099f, "first cell 0.01 in");
    RequireRect(NodeNamed(tree, "MultiboardItem_r1_c2"), 0.7797f, 0.4969f, 0.8097f, 0.5099f, "next cell 0.001 right");
    RequireRect(NodeNamed(tree, "MultiboardItem_r3_c2"), 0.7797f, 0.4689f, 0.8097f, 0.4819f, "third row 0.014 lower");
    const FrameNode& icon = NodeNamed(tree, "MultiboardItem_r3_c2_Icon");
    RequireRect(icon, 0.7792f, 0.4699f, 0.7902f, 0.4809f, "icon centred 0.005 inside the cell");
    Require(icon.texture && icon.texture->path == "Icons/Test.blp", "script icon");
    Require(!tree.FindByName("MultiboardItem_r1_c1_Icon"), "icons hidden by the style");
    const FrameNode& sevenText = NodeNamed(tree, "MultiboardItem_r3_c2_Text");
    Require(sevenText.text && sevenText.text->text == "7" && Near(sevenText.text->fontHeight, 0.009f) &&
            sevenText.text->fontPath == "Fonts\\Test.ttf" && Near(sevenText.text->color[1], 0.0f) && Near(sevenText.text->color[0], 1.0f),
        "cell text uses the LeaderBoard font height and the item colour");
    Require(Near(sevenText.rect.left, 0.7952f) && Near(sevenText.rect.right, 0.8097f), "text starts 0.005 right of the icon");
    Require(NodeNamed(tree, "MultiboardItem_r1_c1_Text").text->text == "Name", "text without icon");
    Require(Descendant(tree, "udg_Board", "MultiboardTitle").text->text == "Score", "title from the string table");
    RequireRect(NodeNamed(tree, "udg_Rank"), 0.625f, 0.2567f, 0.795f, 0.4567f, "leaderboard below the multiboard backdrop");

    const FrameNode& rawNode = NodeNamed(tree, "udg_Raw");
    Require(rawNode.hidden && Descendant(tree, "udg_Raw", "MultiboardBackdrop").hidden &&
            Descendant(tree, "udg_Raw", "MultiboardListContainer").hidden,
        "minimized board hides its backdrop and list");
    const FrameNode& face = Descendant(tree, "udg_Raw", "ButtonBackdropTemplate");
    Require(face.texture && face.texture->path == "UI\\Max.blp", "minimized board shows the maximize button");
}

// 原创排行榜脚本：BJ 添加（同一玩家再加时替换）、按玩家改样式与颜色、排序；原生创建、样式与行数。
const MapScriptText kLeaderboardScript{"war3map.j", ScriptLanguage::Jass, R"(globals
    leaderboard udg_Rank = null
    leaderboard udg_Raw = null
    integer udg_Kills = 0
endglobals
function InitRank takes nothing returns nothing
    call CreateLeaderboardBJ(GetPlayersAll(), "Kills")
    set udg_Rank = GetLastCreatedLeaderboard()
    call LeaderboardAddItemBJ(Player(0), udg_Rank, "Red", 3)
    call LeaderboardAddItemBJ(Player(1), udg_Rank, "Blue", 8)
    call LeaderboardAddItemBJ(Player(PLAYER_NEUTRAL_AGGRESSIVE), udg_Rank, "Creeps", 5)
    call LeaderboardAddItemBJ(Player(0), udg_Rank, "Red", 6)
    call LeaderboardSetPlayerItemStyleBJ(Player(1), udg_Rank, true, true, true)
    call LeaderboardSetPlayerItemValueColorBJ(Player(12), udg_Rank, 0, 100, 0, 0)
    call LeaderboardSortItemsBJ(udg_Rank, bj_SORTTYPE_SORTBYVALUE, false)
    set udg_Raw = CreateLeaderboard()
    call LeaderboardDisplay(udg_Raw, true)
    call LeaderboardAddItem(udg_Raw, "A", 1, Player(2))
    call LeaderboardAddItem(udg_Raw, "B", 1, Player(3))
    call LeaderboardSortItemsByValue(udg_Raw, true)
    call LeaderboardSetStyle(udg_Raw, false, false, true, false)
    call LeaderboardSetSizeByItemCount(udg_Raw, 2)
    call LeaderboardRemoveItem(udg_Raw, 5)
    call LeaderboardAddItemBJ(GetTriggerPlayer(), udg_Raw, "X", 1)
endfunction
function Score takes player p returns nothing
    call LeaderboardSetPlayerItemValueBJ(p, udg_Rank, udg_Kills)
endfunction
)"};

void TestScriptLeaderboard() {
    auto plan = FindScriptFrames({kLeaderboardScript});
    Require(plan.frames.size() == 2, "two leaderboards");
    const ScriptFrame& rank = FrameCreatedAt(plan, 7);
    Require(rank.leaderboard && rank.text && *rank.text == "Kills" && rank.leaderboard->showTitle, "BJ label shows the title");
    const auto& items = rank.leaderboard->items;
    Require(items.size() == 3 && rank.leaderboard->size == 3, "re-adding a player replaces its item; BJ resizes to the count");
    Require(items[0].label == "Blue" && items[0].value == 8 && items[1].label == "Red" && items[1].value == 6 &&
            items[2].label == "Creeps" && items[2].player == 12u,
        "sorted by value, descending");
    Require(items[0].showIcon && !items[1].showIcon && items[0].showLabel && items[0].showValue, "item style by player");
    Require((items[2].valueColor == std::array<std::uint8_t, 4>{0, 255, 0, 255}) && !items[2].labelColor,
        "value colour by player, percentages converted");
    Require(!rank.leaderboard->sortTies && rank.dynamic.empty() && rank.runtimeCalls == 1, "init fully applied");

    const ScriptFrame& raw = FrameCreatedAt(plan, 16);
    Require(raw.leaderboard && raw.text && raw.text->empty() && raw.visible && *raw.visible, "native leaderboard, empty label");
    const ScriptLeaderboard& rawBoard = *raw.leaderboard;
    Require(rawBoard.items.size() == 3 && rawBoard.items[0].label == "A" && rawBoard.items[1].label == "B" &&
            rawBoard.sortTies,
        "equal values keep their order and are reported");
    Require(!rawBoard.showTitle && !rawBoard.items[0].showLabel && rawBoard.items[0].showValue && !rawBoard.items[2].showLabel,
        "style hides the title and names, also for later items");
    Require(!rawBoard.items[2].player && rawBoard.size == 2 && raw.dynamic.size() == 1,
        "unknown player still adds an item; BJ resize subtracts one for an empty label");

    // 标签是字符串表里的空字符串：游戏载入时替换为空，标题不显示，BJ 行数减一；计划保留原文。
    const MapScriptText emptyLabel{"war3map.j", ScriptLanguage::Jass, R"(function Init takes nothing returns nothing
    call CreateLeaderboardBJ(GetPlayersAll(), "TRIGSTR_005")
    call LeaderboardAddItemBJ(Player(0), GetLastCreatedLeaderboard(), "TRIGSTR_007", 0)
    call LeaderboardAddItemBJ(Player(1), GetLastCreatedLeaderboard(), "TRIGSTR_006", 0)
    call LeaderboardSortItemsByLabel(GetLastCreatedLeaderboard(), true)
endfunction
)"};
    const auto untitled = FindScriptFrames({emptyLabel}, {{5, ""}, {6, "Alpha"}, {7, "Beta"}});
    const ScriptLeaderboard& hidden = *untitled.frames.at(0).leaderboard;
    Require(!hidden.showTitle && hidden.size == 1 && *untitled.frames[0].text == "TRIGSTR_005",
        "empty table string hides the title and shortens the BJ size");
    Require(hidden.items[0].label == "TRIGSTR_006" && hidden.items[1].label == "TRIGSTR_007", "label sort uses table text");

    // 模拟：参数玩家与击杀数作为输入，按玩家找到项。
    auto prepared = ScriptSimulator::Prepare({kLeaderboardScript}, plan, "Score");
    Require(prepared.value.has_value(), "simulator prepared");
    const auto result = prepared.value->Run({{"p", "1"}, {"udg_Kills", "9"}});
    Require(result.frames[0].leaderboard->items[0].value == 9 && result.appliedCalls == 1, "player input picks Blue's item");

    // 队伍颜色：先找 .tga 再找 .blp，取第一个像素；序号 16 及以上取最后一种。
    UiSkin colorSkin;
    colorSkin.SetGameSkins("[Default]\nTeamColors=2\nTeamColor=Test\\TC\n");
    const std::string tga{"\0\0\2\0\0\0\0\0\0\0\0\0\1\0\1\0\x20\x28\x30\x20\x10\xff", 22};
    std::vector<Diagnostic> colorDiagnostics;
    const auto colors = ReadTeamColors(colorSkin, [&](const std::string& path) -> std::optional<std::vector<std::byte>> {
        if (path != "Test\\TC00.tga") return std::nullopt;
        std::vector<std::byte> bytes(tga.size());
        std::memcpy(bytes.data(), tga.data(), bytes.size());
        return bytes;
    }, colorDiagnostics);
    Require(colors.size() == 2 && (colors[0].rgba == std::array<std::uint8_t, 4>{0x10, 0x20, 0x30, 0xff}) &&
            colors[0].path == "Test\\TC00.tga",
        "first pixel of the tga");
    Require(colors[1].path.empty() && HasCode(colorDiagnostics, "GAMEUI_TEAM_COLOR"), "missing texture reported");
    colorSkin.SetTeamColors(colors);
    Require(colorSkin.PlayerTeamColor(2) == colorSkin.PlayerTeamColor(0) && colorSkin.PlayerTeamColor(15) == colorSkin.PlayerTeamColor(1) &&
            colorSkin.PlayerTeamColor(20) == colorSkin.PlayerTeamColor(1),
        "player colour index wraps below 16; 16 and above take the last colour");

    // 排布：两个排行榜都单独位于右上角（不与其他对象排开），按项数与标题定高。
    const std::string text =
        "Frame \"FRAME\" \"Leaderboard\" { Width 0.17, Height 0.2,\n"
        "    Frame \"BACKDROP\" \"LeaderboardBackdrop\" { SetAllPoints, }\n"
        "    Frame \"TEXT\" \"LeaderboardTitle\" { SetPoint TOPLEFT, \"LeaderboardBackdrop\", TOPLEFT, 0.02, -0.015,\n"
        "        FrameFont \"MasterFont\", 0.011, \"\", }\n"
        "    Frame \"FRAME\" \"LeaderboardListContainer\" {\n"
        "        SetPoint TOPLEFT, \"LeaderboardTitle\", BOTTOMLEFT, -0.02, -0.002,\n"
        "        SetPoint BOTTOMRIGHT, \"LeaderboardBackdrop\", BOTTOMRIGHT, 0.0, 0.001,\n"
        "    }\n"
        "}\n";
    auto parsed = ParseFdf(text, "UI\\Leaderboard.fdf");
    Require(parsed.Succeeded(), "leaderboard fixture parses");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    UiSkin skin;
    skin.SetGameSkins("[Default]\nMasterFont=Fonts\\Test.ttf\nEscMenuCheckBoxPushedBackground=UI\\Check\n");
    skin.SetMiscUi("[FontHeights]\nLeaderBoard=0.009\n");
    std::vector<TeamColor> team(16);
    for (std::uint32_t i = 0; i < team.size(); ++i) {
        team[i].rgba = {static_cast<std::uint8_t>(i * 10), 0, 0, 255};
        team[i].path = "TC" + std::to_string(i) + ".blp";
    }
    skin.SetTeamColors(team);
    FrameTree tree;
    std::vector<Diagnostic> diagnostics;
    (void)AddScriptFrames(tree, library, &skin, plan, diagnostics);
    (void)tree.Layout();
    Require(HasCode(diagnostics, "GAMEUI_SCRIPT_LEADERBOARD_SORT"), "tie reported");

    // 3 行、显示标题：高 0.013×3+0.001+0.043=0.083。列表 0.056 高，(0.056−0.001)÷0.013 取 4 个行位。
    RequireRect(NodeNamed(tree, "udg_Rank"), 0.625f, 0.482f, 0.795f, 0.565f, "height from size and title");
    RequireRect(Descendant(tree, "udg_Rank", "LeaderboardListContainer"), 0.625f, 0.481f, 0.795f, 0.537f,
        "list below the title, 0.001 past the backdrop");
    RequireRect(Descendant(tree, "udg_Rank", "LeaderboardItem_1"), 0.635f, 0.524f, 0.785f, 0.536f, "first row 0.01 in");
    RequireRect(Descendant(tree, "udg_Rank", "LeaderboardItem_3"), 0.635f, 0.498f, 0.785f, 0.510f, "third row 0.026 lower");
    const FrameNode& icon = Descendant(tree, "udg_Rank", "LeaderboardItem_1_Icon");
    RequireRect(icon, 0.775f, 0.525f, 0.785f, 0.535f, "icon centred 0.005 left of the row end");
    Require(icon.texture && icon.texture->path == "TC1.blp", "icon is the player's team colour texture");
    RequireRect(Descendant(tree, "udg_Rank", "LeaderboardItem_1_IconBackground"), 0.7725f, 0.5225f, 0.7875f, 0.5375f,
        "0.015 icon backing");
    const FrameNode& value = Descendant(tree, "udg_Rank", "LeaderboardItem_1_Value");
    Require(value.text && value.text->text == "8" && value.text->justifyH == "JUSTIFYRIGHT" && Near(value.rect.right, 0.771f),
        "value right-aligned 0.004 left of the icon");
    const FrameNode& label = Descendant(tree, "udg_Rank", "LeaderboardItem_1_Label");
    Require(label.text && label.text->text == "Blue" && Near(label.rect.left, 0.640f) &&
            Near(label.rect.right, value.rect.left - 0.02f) && Near(label.text->color[0], 10.0f / 255.0f),
        "label from the row start to 0.02 before the value, in the player's colour");
    const FrameNode& creeps = Descendant(tree, "udg_Rank", "LeaderboardItem_3_Value");
    Require(Near(creeps.text->color[1], 1.0f) && Near(creeps.text->color[0], 0.0f) && Near(creeps.rect.right, 0.780f),
        "own value colour; without icon the value ends 0.005 before the row end");
    Require(Near(Descendant(tree, "udg_Rank", "LeaderboardItem_3_Label").text->color[0], 120.0f / 255.0f),
        "neutral player colour");

    // 行数 2、不显示标题：高 0.026+0.001+0.035=0.062；列表在背板顶下 0.011，行位 3 个，三项都显示。
    RequireRect(NodeNamed(tree, "udg_Raw"), 0.625f, 0.503f, 0.795f, 0.565f, "untitled height");
    Require(Descendant(tree, "udg_Raw", "LeaderboardTitle").hidden, "title hidden");
    RequireRect(Descendant(tree, "udg_Raw", "LeaderboardItem_1"), 0.635f, 0.541f, 0.785f, 0.553f, "rows start under the top");
    Require(ChildNamed(tree, *tree.FindByName("udg_Raw"), "LeaderboardItem_3") != kNoNode &&
            ChildNamed(tree, *tree.FindByName("udg_Raw"), "LeaderboardItem_1_Label") == kNoNode,
        "a third slot shows the third item; names hidden");
}

} // namespace

int main() {
    try {
        TestScriptClassicObjects();
        TestScriptMultiboard();
        TestScriptLeaderboard();
        std::cout << "Game UI object tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
