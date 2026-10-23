#include "War3MapCore/GameUi/ScriptFrames.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <algorithm>
#include <cmath>

// 脚本框架挂到界面树上，以及预览未还原的界面调用的问题行。
namespace War3Map::GameUi {
namespace {

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, "GameUi", code, std::move(message), std::nullopt};
}

std::string DisplayText(const ScriptFramePlan& plan, const std::string& text) {
    return ResolveTriggerString(plan.strings, text);
}

std::optional<std::uint32_t> FindDescendant(const FrameTree& tree, std::uint32_t node, std::string_view name) {
    for (const std::uint32_t child : tree.Nodes()[node].children) {
        if (tree.Nodes()[child].name == name) return child;
        if (const auto found = FindDescendant(tree, child, name)) return found;
    }
    return std::nullopt;
}

void SetText(FrameTree& tree, std::uint32_t node, std::string text) {
    FrameNode& target = tree.MutableNode(node);
    if (!target.text && !IsTextNode(target)) return;
    if (!target.text) target.text = TextInfo{};
    target.text->key.clear();
    target.text->text = std::move(text);
    target.text->source = "script";
}

// 原版界面对象标题所在的文字节点。
std::string_view TitleNode(ScriptObjectKind object) {
    switch (object) {
    case ScriptObjectKind::TimerDialog: return "TimerDialogTitle";
    case ScriptObjectKind::Dialog: return "ScriptDialogText";
    case ScriptObjectKind::Leaderboard: return "LeaderboardTitle";
    case ScriptObjectKind::Multiboard: return "MultiboardTitle";
    default: return {};
    }
}

FrameAnchor TopTo(std::uint32_t target, std::string name, float y) {
    FrameAnchor anchor{FramePoint::Top, std::move(name), FramePoint::Top, 0.0f, y};
    anchor.targetNode = target;
    return anchor;
}

// 对话框按游戏规则排布（1.27a 添加按钮与重排对话框的代码）：消息在上，按钮间隔 0.003 自上而下；
// 高度 = 上下边距各 0.03 + 消息（文字高 + 0.011）+ 按钮，不足 0.096 取 0.096，否则向上取到 0.048 的整数倍，
// 多出的高度上下平分。消息只按 |n 与换行计行数，不按宽度折行。
void LayoutDialog(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin, std::uint32_t dialog,
    const ScriptFrame& frame, const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    const std::string name = tree.Nodes()[dialog].name;
    const std::string message = frame.text ? DisplayText(plan, *frame.text) : std::string();
    const auto textNode = FindDescendant(tree, dialog, "ScriptDialogText");
    float textHeight = 0.0f;
    if (!message.empty()) {
        float font = textNode && tree.Nodes()[*textNode].text ? tree.Nodes()[*textNode].text->fontHeight : 0.0f;
        if (font <= 0.0f) font = 0.013f;
        std::size_t lines = 1 + static_cast<std::size_t>(std::count(message.begin(), message.end(), '\n'));
        for (std::size_t at = message.find('|'); at != std::string::npos; at = message.find('|', at + 2)) {
            if (at + 1 < message.size() && (message[at + 1] == 'n' || message[at + 1] == 'N')) ++lines;
        }
        textHeight = font * static_cast<float>(lines);
    }
    std::vector<std::uint32_t> buttons;
    for (const auto& text : frame.buttons) {
        auto built = tree.InstantiateUnder(library, skin, "ScriptDialogButton", dialog);
        if (!built.value) {
            diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
                name + " 的按钮模板 ScriptDialogButton 不在已读入的界面目录里"));
            break;
        }
        buttons.push_back(*built.value);
        if (const auto label = FindDescendant(tree, *built.value, "ScriptDialogButtonText")) {
            SetText(tree, *label, DisplayText(plan, text));
        }
    }
    const float buttonHeight = buttons.empty() ? 0.0f : tree.Nodes()[buttons.front()].height.value_or(0.035f);
    const float messagePart = message.empty() ? 0.03f : textHeight + 0.008f + 0.003f + 0.03f;
    const float buttonPart = buttons.empty()
        ? 0.03f
        : (buttonHeight + 0.003f) * static_cast<float>(buttons.size()) - 0.003f + 0.03f;
    const float total = messagePart + buttonPart;
    float height = 0.096f;
    float messageY = -0.03f;
    if (total >= 0.096f) {
        const float rest = std::fmod(total, 0.048f);
        height = rest == 0.0f ? total : total - rest + 0.048f;
        messageY = -(height - (total - 0.06f)) * 0.5f;
    }
    tree.MutableNode(dialog).height = height;
    if (textNode) {
        FrameNode& text = tree.MutableNode(*textNode);
        text.anchors.clear();
        text.anchors.push_back(TopTo(dialog, name, messageY));
    }
    float top = message.empty() ? -0.027f : messageY - textHeight - 0.011f;
    for (const std::uint32_t button : buttons) {
        FrameNode& node = tree.MutableNode(button);
        node.anchors.clear();
        node.anchors.push_back(TopTo(dialog, name, top));
        top -= buttonHeight + 0.003f;
    }
}

FrameAnchor AnchorTo(const FrameTree& tree, FramePoint point, std::uint32_t target, FramePoint relativePoint, float x, float y) {
    FrameAnchor anchor{point, tree.Nodes()[target].name, relativePoint, x, y};
    anchor.targetNode = target;
    return anchor;
}

void PlaceAt(FrameTree& tree, std::uint32_t node, FrameAnchor anchor) {
    FrameNode& target = tree.MutableNode(node);
    target.anchors.clear();
    target.anchors.push_back(std::move(anchor));
}

// 文字宽度的近似值（核心层没有字体度量）：中日韩等多字节字符按一个字号宽，其余按 0.55 个字号；
// |c 颜色码、|r 不占宽度。
float ApproxTextWidth(std::string_view text, float fontHeight) {
    float width = 0.0f;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '|' && i + 1 < text.size()) {
            const char code = static_cast<char>(text[i + 1] | 0x20);
            if (code == 'c' && i + 10 <= text.size()) {
                i += 10;
                continue;
            }
            if (code == 'r' || code == 'n') {
                i += 2;
                continue;
            }
        }
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
        width += length == 1 ? fontHeight * 0.55f : fontHeight;
        i += length;
    }
    return width;
}

TextureInfo SkinTexture(const FrameTree& tree, const UiSkin* skin, const std::string& key) {
    TextureInfo texture;
    texture.key = key;
    texture.path = key;
    texture.source = "literal";
    if (skin) {
        if (const auto found = skin->Lookup(key, tree.Options().race, tree.Options().expansion)) {
            texture.path = found->value;
            texture.source = found->source;
        }
    }
    texture.path = EnsureTextureExtension(texture.path);
    return texture;
}

// 多面板与排行榜的文字：MasterFont，字号取 MiscUI 的 FontHeights LeaderBoard。
TextInfo BoardFont(const FrameTree& tree, const UiSkin* skin, std::string justifyH) {
    TextInfo font;
    font.fontKey = "MasterFont";
    font.fontPath = font.fontKey;
    if (skin) {
        if (const auto found = skin->Lookup(font.fontKey, tree.Options().race, tree.Options().expansion)) font.fontPath = found->value;
    }
    font.fontHeight = skin ? skin->FontHeight("LeaderBoard").value_or(0.011f) : 0.011f;
    font.justifyH = std::move(justifyH);
    font.justifyV = "JUSTIFYMIDDLE";
    font.source = "script";
    return font;
}

// 多面板按游戏代码排布（1.27a 多面板刷新与列表行布局）：
// 列表宽 = Σ(第一行各格宽 + 0.002)，格宽默认 0.03；标题栏宽取 “标题文字宽 + 0.03” 与 “列表宽 − 最小化按钮宽”
// 的较大者（最小化时只看文字）；背板高 = 0.014 × 行数 + 0.012。第 r 行的左上角在列表左上角右 0.01、下
// 0.001 + 0.014r，行高 0.013；同一行的格子依次右接，间隔 0.001。显示图标时图标 0.011 见方、中心在格子左边
// 右 0.005，文字从图标右侧 0.005 开始；文字字号取 MiscUI 的 FontHeights LeaderBoard。最小化时背板与列表不显示。
void LayoutMultiboard(FrameTree& tree, const UiSkin* skin, std::uint32_t root, const ScriptFrame& frame,
    const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    const std::string name = tree.Nodes()[root].name;
    const auto button = FindDescendant(tree, root, "MultiboardMinimizeButton");
    const auto titleBar = FindDescendant(tree, root, "MultiboardTitleBackdrop");
    const auto title = FindDescendant(tree, root, "MultiboardTitle");
    const auto backdrop = FindDescendant(tree, root, "MultiboardBackdrop");
    const auto list = FindDescendant(tree, root, "MultiboardListContainer");
    if (!frame.board || !button || !titleBar || !title || !backdrop || !list) {
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
            name + " 的多面板模板缺少标题栏、背板或列表，未画出内容"));
        return;
    }
    const ScriptBoard& board = *frame.board;
    if (board.clipped) {
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_BOARD_CLIPPED",
            name + " 的行列超过预览上限，只画前 " + std::to_string(kMaxBoardRows) + " 行、" +
                std::to_string(kMaxBoardColumns) + " 列"));
    }
    float listWidth = 0.0f;
    for (std::uint32_t c = 0; c < board.columns; ++c) listWidth += (board.rows > 0 ? board.At(0, c)->width : 0.0f) + 0.002f;
    const auto& titleText = tree.Nodes()[*title].text;
    const float titleFont = titleText && titleText->fontHeight > 0.0f ? titleText->fontHeight : 0.011f;
    const std::string caption = frame.text ? DisplayText(plan, *frame.text) : std::string();
    const float buttonWidth = tree.Nodes()[root].width.value_or(0.024f);
    tree.MutableNode(*titleBar).width =
        std::max(ApproxTextWidth(caption, titleFont) + 0.03f, board.minimized ? 0.0f : listWidth - buttonWidth);
    tree.MutableNode(*backdrop).height = 0.014f * static_cast<float>(board.rows) + 0.012f;
    if (board.minimized) {
        tree.MutableNode(*backdrop).hidden = true;
        tree.MutableNode(*list).hidden = true;
        if (const auto face = FindDescendant(tree, *button, "ButtonBackdropTemplate")) {
            tree.MutableNode(*face).texture = SkinTexture(tree, skin, "MultiboardMaximizeButtonEnabled");
        }
        return;
    }

    const TextInfo font = BoardFont(tree, skin, "JUSTIFYLEFT");
    const TextureInfo defaultIcon = SkinTexture(tree, skin, "MultiboardDefaultIcon");
    for (std::uint32_t r = 0; r < board.rows; ++r) {
        float x = 0.01f + 0.001f;
        const float y = -(0.001f + 0.014f * static_cast<float>(r));
        for (std::uint32_t c = 0; c < board.columns; ++c) {
            const ScriptBoardItem& item = *board.At(r, c);
            FrameNode cell;
            cell.parent = *list;
            cell.frameType = "FRAME";
            cell.name = "MultiboardItem_r" + std::to_string(r + 1) + "_c" + std::to_string(c + 1);
            cell.width = item.width;
            cell.height = 0.013f;
            cell.anchors.push_back(AnchorTo(tree, FramePoint::TopLeft, *list, FramePoint::TopLeft, x, y));
            x += item.width + 0.001f;
            const std::uint32_t cellId = tree.AddFrame(std::move(cell));
            std::optional<std::uint32_t> iconId;
            if (item.showIcon) {
                FrameNode icon;
                icon.parent = cellId;
                icon.frameType = "BACKDROP";
                icon.name = tree.Nodes()[cellId].name + "_Icon";
                icon.width = 0.011f;
                icon.height = 0.011f;
                icon.anchors.push_back(AnchorTo(tree, FramePoint::Center, cellId, FramePoint::Left, 0.005f, 0.0f));
                if (item.icon) {
                    TextureInfo texture;
                    texture.key = *item.icon;
                    texture.path = EnsureTextureExtension(*item.icon);
                    texture.source = "script";
                    icon.texture = std::move(texture);
                } else {
                    icon.texture = defaultIcon;
                }
                iconId = tree.AddFrame(std::move(icon));
            }
            if (!item.showValue || item.value.empty()) continue;
            FrameNode text;
            text.parent = cellId;
            text.frameType = "TEXT";
            text.name = tree.Nodes()[cellId].name + "_Text";
            text.text = font;
            text.text->text = DisplayText(plan, item.value);
            for (std::size_t i = 0; i < 4; ++i) text.text->color[i] = static_cast<float>(item.color[i]) / 255.0f;
            text.anchors.push_back(iconId ? AnchorTo(tree, FramePoint::Left, *iconId, FramePoint::Right, 0.005f, 0.00001f)
                                          : AnchorTo(tree, FramePoint::Left, cellId, FramePoint::Left, 0.0f, 0.00001f));
            text.anchors.push_back(AnchorTo(tree, FramePoint::Right, cellId, FramePoint::Right, 0.0f, 0.00001f));
            (void)tree.AddFrame(std::move(text));
        }
    }
}

// 排行榜按游戏代码排布（1.27a 排行榜与列表框）：高 = 0.013 × 行数 + 0.001 + 0.043（不显示标题时 0.035），
// 行数是 LeaderboardSetSizeByItemCount 的值按无符号数与 24 取小（负数即 24）。显示标题时列表左上角在标题左下
// 角左 0.02、下 0.002，否则在背板左上角下 0.011；列表右下角在背板右下角下 0.001。列表的行位数 =
// (列表高 − 0.001) ÷ 0.013 取整，每行高 0.012、左右各缩进 0.01，第 r 行在列表顶下 0.001 + 0.013r。
// 一项里名字左端在行左 0.005；数值右对齐，右端在图标左 0.004 或行右 0.005；名字右端在数值左 0.02（没有数值
// 时在图标左 0.02 或行右 0.005）。图标 0.01 见方、中心在行右端左 0.005，衬底为 0.015 见方的
// EscMenuCheckBoxPushedBackground，图标是玩家的队伍颜色贴图；名字与数值默认用玩家的队伍颜色。
void LayoutLeaderboard(FrameTree& tree, const UiSkin* skin, std::uint32_t root, const ScriptFrame& frame,
    const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    const std::string name = tree.Nodes()[root].name;
    const auto title = FindDescendant(tree, root, "LeaderboardTitle");
    const auto backdrop = FindDescendant(tree, root, "LeaderboardBackdrop");
    const auto list = FindDescendant(tree, root, "LeaderboardListContainer");
    if (!frame.leaderboard || !title || !backdrop || !list) {
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
            name + " 的排行榜模板缺少标题、背板或列表，未画出内容"));
        return;
    }
    const ScriptLeaderboard& board = *frame.leaderboard;
    if (board.clipped) {
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_BOARD_CLIPPED",
            name + " 的项数超过预览上限，只保留前 " + std::to_string(kMaxLeaderboardItems) + " 项"));
    }
    if (board.sortTies) {
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_LEADERBOARD_SORT",
            name + " 排序时有相同的键，游戏里这些项的先后可能与预览不同"));
    }
    const auto size = static_cast<std::uint32_t>(board.size);
    const float height = 0.013f * static_cast<float>(std::min(size, 24u)) + 0.001f + (board.showTitle ? 0.043f : 0.035f);
    tree.MutableNode(root).height = height;
    FrameNode& titleNode = tree.MutableNode(*title);
    titleNode.hidden = !board.showTitle;
    // 核心层不量文字：标题高按字号算。
    const float titleHeight = titleNode.text && titleNode.text->fontHeight > 0.0f ? titleNode.text->fontHeight : 0.011f;
    titleNode.height = titleHeight;
    FrameNode& listNode = tree.MutableNode(*list);
    listNode.anchors.clear();
    listNode.anchors.push_back(board.showTitle
            ? AnchorTo(tree, FramePoint::TopLeft, *title, FramePoint::BottomLeft, -0.02f, -0.002f)
            : AnchorTo(tree, FramePoint::TopLeft, *backdrop, FramePoint::TopLeft, 0.0f, -0.011f));
    listNode.anchors.push_back(AnchorTo(tree, FramePoint::BottomRight, *backdrop, FramePoint::BottomRight, 0.0f, -0.001f));
    const float listHeight = height - (board.showTitle ? 0.015f + titleHeight + 0.002f : 0.011f) + 0.001f;
    const auto slots = static_cast<std::size_t>(std::max(0.0f, (listHeight - 0.001f) / 0.013f));

    const TextInfo labelFont = BoardFont(tree, skin, "JUSTIFYLEFT");
    const TextInfo valueFont = BoardFont(tree, skin, "JUSTIFYRIGHT");
    const TextureInfo iconBack = SkinTexture(tree, skin, "EscMenuCheckBoxPushedBackground");
    const auto colorOf = [](const std::optional<std::array<std::uint8_t, 4>>& own, const TeamColor* team) {
        const std::array<std::uint8_t, 4> rgba = own ? *own : team ? team->rgba : std::array<std::uint8_t, 4>{255, 255, 255, 255};
        std::array<float, 4> color{};
        for (std::size_t i = 0; i < 4; ++i) color[i] = static_cast<float>(rgba[i]) / 255.0f;
        return color;
    };
    const auto square = [&](std::uint32_t parent, std::string nodeName, float side, std::uint32_t row) {
        FrameNode node;
        node.parent = parent;
        node.frameType = "BACKDROP";
        node.name = std::move(nodeName);
        node.width = side;
        node.height = side;
        node.anchors.push_back(AnchorTo(tree, FramePoint::Center, row, FramePoint::Right, -0.005f, 0.0f));
        return node;
    };
    for (std::size_t r = 0; r < std::min(slots, board.items.size()); ++r) {
        const ScriptLeaderboardItem& item = board.items[r];
        const TeamColor* team = item.player && skin ? skin->PlayerTeamColor(*item.player) : nullptr;
        const float y = -(0.001f + 0.013f * static_cast<float>(r));
        FrameNode row;
        row.parent = *list;
        row.frameType = "FRAME";
        row.name = "LeaderboardItem_" + std::to_string(r + 1);
        row.height = 0.012f;
        row.anchors.push_back(AnchorTo(tree, FramePoint::TopLeft, *list, FramePoint::TopLeft, 0.01f, y));
        row.anchors.push_back(AnchorTo(tree, FramePoint::TopRight, *list, FramePoint::TopRight, -0.01f, y));
        const std::uint32_t rowId = tree.AddFrame(std::move(row));
        const std::string rowName = tree.Nodes()[rowId].name;
        std::optional<std::uint32_t> iconId;
        if (item.showIcon) {
            FrameNode back = square(rowId, rowName + "_IconBackground", 0.015f, rowId);
            back.texture = iconBack;
            (void)tree.AddFrame(std::move(back));
            FrameNode icon = square(rowId, rowName + "_Icon", 0.01f, rowId);
            if (team && !team->path.empty()) {
                TextureInfo texture;
                texture.key = "TeamColor";
                texture.path = team->path;
                texture.source = "skin";
                icon.texture = std::move(texture);
            }
            iconId = tree.AddFrame(std::move(icon));
        }
        std::optional<std::uint32_t> valueId;
        if (item.showValue) {
            const std::string number = std::to_string(item.value);
            FrameNode value;
            value.parent = rowId;
            value.frameType = "TEXT";
            value.name = rowName + "_Value";
            value.text = valueFont;
            value.text->text = number;
            value.text->color = colorOf(item.valueColor, team);
            value.width = ApproxTextWidth(number, valueFont.fontHeight);
            value.height = 0.012f;
            value.anchors.push_back(iconId ? AnchorTo(tree, FramePoint::Right, *iconId, FramePoint::Left, -0.004f, 0.00002f)
                                           : AnchorTo(tree, FramePoint::Right, rowId, FramePoint::Right, -0.005f, 0.0f));
            valueId = tree.AddFrame(std::move(value));
        }
        if (!item.showLabel) continue;
        FrameNode label;
        label.parent = rowId;
        label.frameType = "TEXT";
        label.name = rowName + "_Label";
        label.text = labelFont;
        label.text->text = DisplayText(plan, item.label);
        label.text->color = colorOf(item.labelColor, team);
        label.height = 0.012f;
        label.anchors.push_back(AnchorTo(tree, FramePoint::Left, rowId, FramePoint::Left, 0.005f, 0.0f));
        label.anchors.push_back(valueId ? AnchorTo(tree, FramePoint::Right, *valueId, FramePoint::Left, -0.02f, 0.0f)
                                : iconId ? AnchorTo(tree, FramePoint::Right, *iconId, FramePoint::Left, -0.02f, 0.0f)
                                         : AnchorTo(tree, FramePoint::Right, rowId, FramePoint::Right, -0.005f, 0.0f));
        (void)tree.AddFrame(std::move(label));
    }
}

// 显示中的原版界面对象按游戏规则排开（GameCodeLayout 里是各自单独出现时的位置）：计时器窗口自右向左
// 间隔 0.002；多面板在第一个计时器窗口下 0.001；排行榜在多面板背板下 0.005（多面板最小化时在其按钮下），
// 没有多面板时在第一个计时器窗口下 0.001。游戏同时只显示一个多面板、一个排行榜，取脚本中最后一个。
void StackShownObjects(FrameTree& tree, const ScriptFramePlan& plan, const std::vector<std::uint32_t>& nodeOf) {
    std::vector<std::uint32_t> timers;
    std::optional<std::uint32_t> board;
    std::optional<std::uint32_t> leaderboard;
    for (std::uint32_t i = 0; i < plan.frames.size(); ++i) {
        const ScriptFrame& frame = plan.frames[i];
        if (!frame.visible || !*frame.visible || nodeOf[i] == kNoNode) continue;
        if (frame.object == ScriptObjectKind::TimerDialog) timers.push_back(nodeOf[i]);
        else if (frame.object == ScriptObjectKind::Multiboard) board = i;
        else if (frame.object == ScriptObjectKind::Leaderboard) leaderboard = nodeOf[i];
    }
    for (std::size_t k = 1; k < timers.size(); ++k) {
        PlaceAt(tree, timers[k], AnchorTo(tree, FramePoint::Right, timers[k - 1], FramePoint::Left, -0.002f, 0.0f));
    }
    if (board && !timers.empty()) {
        PlaceAt(tree, nodeOf[*board], AnchorTo(tree, FramePoint::TopRight, timers.front(), FramePoint::BottomRight, 0.0f, -0.001f));
    }
    if (!leaderboard) return;
    if (board) {
        const ScriptFrame& frame = plan.frames[*board];
        const bool minimized = frame.board && frame.board->minimized;
        if (const auto under = FindDescendant(tree, nodeOf[*board], minimized ? "MultiboardMinimizeButton" : "MultiboardBackdrop")) {
            PlaceAt(tree, *leaderboard, AnchorTo(tree, FramePoint::TopRight, *under, FramePoint::BottomRight, 0.0f, -0.005f));
        }
    } else if (!timers.empty()) {
        PlaceAt(tree, *leaderboard, AnchorTo(tree, FramePoint::TopRight, timers.front(), FramePoint::BottomRight, 0.0f, -0.001f));
    }
}

} // namespace

std::vector<Diagnostic> ScriptCallDiagnostics(const ScriptFramePlan& plan) {
    std::vector<Diagnostic> diagnostics;
    for (const auto& group : plan.calls) {
        const char* code = "GAMEUI_SCRIPT_CALL_NOT_PREVIEWED";
        std::string reason = "预览不还原";
        if (group.reason == ScriptUiCallReason::UntracedTarget) {
            code = "GAMEUI_SCRIPT_CALL_UNTRACED";
            reason = "作用的框架不是脚本创建后保存的变量";
        } else if (group.reason == ScriptUiCallReason::Unknown) {
            code = "GAMEUI_SCRIPT_CALL_UNKNOWN";
            reason = "未识别";
        }
        std::string sites;
        for (const auto& sample : group.samples) {
            if (!sites.empty()) sites += "、";
            sites += sample.scriptFile + ":" + std::to_string(sample.line);
        }
        if (group.count > group.samples.size()) sites += " 等";
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, code,
            group.function + "（" + std::string(UiApiFamilyName(group.family)) + "）" + std::to_string(group.count) +
                " 处，" + reason + "：" + sites));
    }
    return diagnostics;
}

std::uint32_t AddScriptFrames(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin,
    const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    if (plan.frames.empty()) return kNoNode;
    FrameNode root;
    root.parent = kScreenNode;
    root.frameType = "SCRIPT";
    root.name = std::string(kScriptRoot);
    root.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
    root.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
    const std::uint32_t rootId = tree.AddFrame(std::move(root));

    const auto where = [](const ScriptFrame& frame) {
        return frame.created.scriptFile + " 第 " + std::to_string(frame.created.line) + " 行";
    };
    std::vector<std::uint32_t> nodeOf(plan.frames.size(), kNoNode);
    for (std::size_t i = 0; i < plan.frames.size(); ++i) {
        const ScriptFrame& frame = plan.frames[i];
        std::uint32_t parent = rootId;
        if (frame.parent.target == ScriptFrameTarget::Script && frame.parent.frame < i &&
            nodeOf[frame.parent.frame] != kNoNode) {
            parent = nodeOf[frame.parent.frame];
        }
        std::optional<std::uint32_t> id;
        if (!frame.templateName.empty()) {
            auto built = tree.InstantiateUnder(library, skin, frame.templateName, parent);
            if (built.value) {
                id = built.value;
                diagnostics.insert(diagnostics.end(), built.diagnostics.begin(), built.diagnostics.end());
            } else {
                diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
                    where(frame) + "创建框架用的模板 " + frame.templateName + " 不在已读入的界面目录里"));
            }
        }
        if (!id) {
            FrameNode node;
            node.parent = parent;
            node.frameType = frame.frameType.empty() ? "FRAME" : frame.frameType;
            id = tree.AddFrame(std::move(node));
        }
        nodeOf[i] = *id;
        FrameNode& node = tree.MutableNode(*id);
        node.name = frame.name;
        if (!frame.frameType.empty()) node.frameType = frame.frameType;
        node.sourceFile = frame.created.scriptFile;
        node.sourceLine = frame.created.line;
        node.scriptFrame = static_cast<std::uint32_t>(i);
        node.hidden = frame.visible && !*frame.visible;
        if (frame.width) node.width = frame.width;
        if (frame.height) node.height = frame.height;
        if (frame.texture) {
            TextureInfo texture;
            texture.key = *frame.texture;
            texture.path = EnsureTextureExtension(*frame.texture);
            texture.source = "script";
            node.texture = std::move(texture);
        }
        if (frame.object != ScriptObjectKind::Frame) {
            // 原版界面对象：先放到各自单独显示时的位置，全部建好后再按显示中的对象排开。
            node.anchors.clear();
            if (const auto placement = KnownRootPlacement(frame.templateName)) {
                for (FrameAnchor anchor : placement->anchors) {
                    anchor.targetNode = kScreenNode;
                    anchor.relativeName = "Screen";
                    node.anchors.push_back(std::move(anchor));
                }
            }
            if (frame.text) {
                if (const auto title = FindDescendant(tree, *id, TitleNode(frame.object))) SetText(tree, *title, DisplayText(plan, *frame.text));
            }
            if (frame.object == ScriptObjectKind::Dialog) LayoutDialog(tree, library, skin, *id, frame, plan, diagnostics);
            if (frame.object == ScriptObjectKind::Multiboard) LayoutMultiboard(tree, skin, *id, frame, plan, diagnostics);
            if (frame.object == ScriptObjectKind::Leaderboard) LayoutLeaderboard(tree, skin, *id, frame, plan, diagnostics);
        } else if (frame.text) {
            // 按钮类框架的文字在它的文字子框架上。
            std::uint32_t textNode = *id;
            if (!node.text && !IsTextNode(node)) {
                for (const std::uint32_t child : node.children) {
                    if (tree.Nodes()[child].text || IsTextNode(tree.Nodes()[child])) {
                        textNode = child;
                        break;
                    }
                }
            }
            SetText(tree, textNode, DisplayText(plan, *frame.text));
        }
    }
    // 锚点最后处理：可以引用后创建的脚本框架。
    for (std::size_t i = 0; i < plan.frames.size(); ++i) {
        const ScriptFrame& frame = plan.frames[i];
        FrameNode& node = tree.MutableNode(nodeOf[i]);
        if (frame.clearPoints) node.anchors.clear();
        for (const auto& anchor : frame.anchors) {
            FrameAnchor resolved{anchor.point, anchor.relative.name, anchor.relativePoint, anchor.x, anchor.y};
            switch (anchor.relative.target) {
            case ScriptFrameTarget::Screen:
                resolved.targetNode = kScreenNode;
                resolved.relativeName = "Screen";
                break;
            case ScriptFrameTarget::CodeArea:
                if (const auto area = tree.FindCodeArea(anchor.relative.name)) {
                    resolved.targetNode = *area;
                } else if (anchor.relative.name == kGameCodeRoot) {
                    resolved.targetNode = kScreenNode;  // 游戏界面根铺满屏幕
                } else {
                    diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_ANCHOR_AREA",
                        where(frame) + "的框架锚在游戏界面区域 " + anchor.relative.name + "，显示游戏界面后才能定位"));
                    continue;
                }
                break;
            case ScriptFrameTarget::Script:
                resolved.targetNode = nodeOf[anchor.relative.frame];
                resolved.relativeName = tree.Nodes()[resolved.targetNode].name;
                break;
            case ScriptFrameTarget::Named:
                break;  // 按名字查 fdf 框架
            default:
                continue;
            }
            const auto existing = std::find_if(node.anchors.begin(), node.anchors.end(),
                [&](const FrameAnchor& candidate) { return candidate.point == resolved.point; });
            if (existing != node.anchors.end()) *existing = std::move(resolved);
            else node.anchors.push_back(std::move(resolved));
        }
    }
    StackShownObjects(tree, plan, nodeOf);
    return rootId;
}

} // namespace War3Map::GameUi
