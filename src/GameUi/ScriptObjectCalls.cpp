#include "ScriptUiApi.h"

#include <algorithm>
#include <functional>

// 原版界面对象的内容：多面板的行列与格子、排行榜的项（地图脚本界面的静态识别与预览模拟共用）。
namespace War3Map::GameUi::detail {
namespace {

// 从 first 起的四个颜色参数：红、绿、蓝、不透明度。BJ 版本（percent）为百分比且第四个是透明度。
std::optional<std::array<std::uint8_t, 4>> ColorArguments(const std::vector<std::optional<ScriptValue>>& args, int first,
    bool percent) {
    std::array<std::uint8_t, 4> color{};
    for (int i = 0; i < 4; ++i) {
        const auto at = static_cast<std::size_t>(std::max(0, first + i));
        const auto component = NumberOf(at < args.size() ? args[at] : std::nullopt);
        if (!component) return std::nullopt;
        if (percent) {
            // PercentTo255：百分比乘 2.55 取整并限制在 0..255。
            const float value = i == 3 ? 100.0f - *component : *component;
            color[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(value * 255.0f * 0.01f), 0, 255));
        } else {
            // 原生函数只取每个分量的低 8 位。
            color[i] = static_cast<std::uint8_t>(static_cast<std::int64_t>(*component) & 0xFF);
        }
    }
    return color;
}

// LeaderboardResizeBJ：行数 = 项数，标签（按字符串表替换后）为空时减一。标签无法确定时返回 false。
bool ResizeLeaderboard(ScriptFrame& frame, const TriggerStrings& strings) {
    if (!frame.leaderboard || !frame.text) return false;
    const bool empty = ResolveTriggerString(strings, *frame.text).empty();
    frame.leaderboard->size = static_cast<std::int32_t>(frame.leaderboard->items.size()) - (empty ? 1 : 0);
    return true;
}

} // namespace

void LeaderboardLabelChanged(ScriptFrame& frame, const CallSpec& spec, const TriggerStrings& strings) {
    if (!frame.leaderboard || !frame.text) return;
    frame.leaderboard->showTitle = !ResolveTriggerString(strings, *frame.text).empty();
    if (spec.leaderboardBj) ResizeLeaderboard(frame, strings);
}

std::optional<BoardCell> BoardCellOf(const std::optional<ScriptValue>& row, const std::optional<ScriptValue>& column) {
    const auto r = NumberOf(row);
    const auto c = NumberOf(column);
    if (!r || !c || *r < 0.0f || *c < 0.0f) return std::nullopt;
    return BoardCell{static_cast<std::uint32_t>(*r), static_cast<std::uint32_t>(*c)};
}

bool ApplyBoardCall(ScriptFrame& frame, const CallSpec& spec, const std::vector<std::optional<ScriptValue>>& args,
    std::optional<BoardCell> item) {
    if (!frame.board) return false;
    ScriptBoard& board = *frame.board;
    const auto argument = [&](int i) { return i >= 0 && static_cast<std::size_t>(i) < args.size() ? args[i] : std::nullopt; };
    const auto value = [&](int offset) { return argument(spec.valueArg + offset); };
    switch (spec.kind) {
    case CallKind::BoardRows:
    case CallKind::BoardColumns: {
        const auto count = NumberOf(value(0));
        if (!count) return false;
        // 游戏按无符号数处理，负数视为 0 行（预览不画）。
        const auto n = static_cast<std::uint32_t>(std::max(0.0f, *count));
        if (spec.kind == CallKind::BoardRows) board.Resize(n, board.columns);
        else board.Resize(board.rows, n);
        return true;
    }
    case CallKind::BoardMinimize:
        if (const auto flag = BooleanOf(value(0))) {
            board.minimized = *flag;
            return true;
        }
        return false;
    case CallKind::BoardItems: break;
    default: return false;
    }

    // 先取齐参数，再作用到格子上。
    std::function<void(ScriptBoardItem&)> set;
    switch (spec.property) {
    case BoardProperty::Value:
    case BoardProperty::Icon: {
        auto text = StringOf(value(0));
        if (!text) return false;
        if (spec.property == BoardProperty::Value) set = [text = *text](ScriptBoardItem& cell) { cell.value = text; };
        else set = [text = *text](ScriptBoardItem& cell) { cell.icon = text; };
        break;
    }
    case BoardProperty::Width: {
        auto width = NumberOf(value(0));
        if (!width) return false;
        // BJ 版本的宽度是屏幕宽度的百分比（除以 100 后交给原生函数）。
        const float w = spec.percent ? *width / 100.0f : *width;
        set = [w](ScriptBoardItem& cell) { cell.width = w; };
        break;
    }
    case BoardProperty::Style: {
        const auto showValue = BooleanOf(value(0));
        const auto showIcon = BooleanOf(value(1));
        if (!showValue || !showIcon) return false;
        set = [v = *showValue, i = *showIcon](ScriptBoardItem& cell) {
            cell.showValue = v;
            cell.showIcon = i;
        };
        break;
    }
    case BoardProperty::Color: {
        const auto color = ColorArguments(args, spec.valueArg, spec.percent);
        if (!color) return false;
        set = [color = *color](ScriptBoardItem& cell) { cell.color = color; };
        break;
    }
    }

    switch (spec.board) {
    case BoardScope::All:
        set(board.defaults);
        for (auto& cell : board.items) set(cell);
        return true;
    case BoardScope::Item: {
        if (!item) return false;
        ScriptBoardItem* cell = board.At(item->row, item->column);
        if (!cell) return false;
        set(*cell);
        return true;
    }
    case BoardScope::ColumnRow: {
        // 列、行从 1 起，0 表示全部；超出现有行列时游戏不做任何事。
        const auto column = NumberOf(argument(1));
        const auto row = NumberOf(argument(2));
        if (!column || !row) return false;
        for (std::uint32_t r = 0; r < board.rows; ++r) {
            if (*row != 0.0f && static_cast<std::int64_t>(*row) != static_cast<std::int64_t>(r) + 1) continue;
            for (std::uint32_t c = 0; c < board.columns; ++c) {
                if (*column != 0.0f && static_cast<std::int64_t>(*column) != static_cast<std::int64_t>(c) + 1) continue;
                set(*board.At(r, c));
            }
        }
        return true;
    }
    }
    return false;
}

bool ApplyLeaderboardCall(ScriptFrame& frame, const CallSpec& spec, const std::vector<std::optional<ScriptValue>>& args,
    const TriggerStrings& strings) {
    if (!frame.leaderboard) return false;
    ScriptLeaderboard& board = *frame.leaderboard;
    auto& items = board.items;
    const auto argument = [&](int i) { return i >= 0 && static_cast<std::size_t>(i) < args.size() ? args[i] : std::nullopt; };
    const auto value = [&](int offset) { return argument(spec.valueArg + offset); };
    const std::optional<std::uint32_t> player = PlayerOf(argument(spec.playerArg));
    // 该玩家的第一项（与 LeaderboardGetPlayerIndex 一致）；前面有玩家不确定的项时无法判断。
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    const auto playerItem = [&]() -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i].player) return std::nullopt;
            if (*items[i].player == *player) return i;
        }
        return kNone;
    };

    if (spec.leaderboard == LeaderboardOp::Add) {
        bool known = true;
        if (spec.leaderboardBj) {
            // LeaderboardAddItemBJ 先移除该玩家已有的项。
            const auto existing = player ? playerItem() : std::nullopt;
            if (!existing) known = false;
            else if (*existing != kNone) items.erase(items.begin() + static_cast<std::ptrdiff_t>(*existing));
        }
        ScriptLeaderboardItem item;
        const auto label = StringOf(value(0));
        const auto number = NumberOf(value(1));
        item.label = label.value_or(std::string());
        item.value = number ? static_cast<std::int32_t>(*number) : 0;
        item.player = player;
        item.showLabel = board.showNames;
        item.showValue = board.showValues;
        if (items.size() < kMaxLeaderboardItems) items.push_back(std::move(item));
        else board.clipped = true;
        if (spec.leaderboardBj && !ResizeLeaderboard(frame, strings)) known = false;
        return known && label && number && player;
    }

    // 作用的项：按序号，或该玩家的第一项；没有这一项时游戏不做任何事（kNone）。不指定项的调用作用于全部项。
    std::optional<std::size_t> target;
    if (spec.indexArg >= 0) {
        const auto index = NumberOf(argument(spec.indexArg));
        if (!index) return false;
        target = *index >= 0.0f && *index < static_cast<float>(items.size()) ? static_cast<std::size_t>(*index) : kNone;
    } else if (spec.playerArg >= 0) {
        if (!player) return false;
        target = playerItem();
        if (!target) return false;
    }
    ScriptLeaderboardItem* item = target && *target != kNone ? &items[*target] : nullptr;
    const bool missing = target && !item;

    switch (spec.leaderboard) {
    case LeaderboardOp::Remove:
        if (item) items.erase(items.begin() + static_cast<std::ptrdiff_t>(*target));
        return !spec.leaderboardBj || ResizeLeaderboard(frame, strings);
    case LeaderboardOp::Clear:
        items.clear();
        return true;
    case LeaderboardOp::Size:
        if (const auto size = NumberOf(value(0))) {
            board.size = static_cast<std::int32_t>(*size);
            return true;
        }
        return false;
    case LeaderboardOp::Style: {
        bool flags[4]{};
        for (int i = 0; i < 4; ++i) {
            const auto flag = BooleanOf(value(i));
            if (!flag) return false;
            flags[i] = *flag;
        }
        // 第一个参数是标题显隐；其余三个改变全部项，名字与数值也成为新项的默认值。
        board.showTitle = flags[0];
        board.showNames = flags[1];
        board.showValues = flags[2];
        for (auto& each : items) {
            each.showLabel = flags[1];
            each.showValue = flags[2];
            each.showIcon = flags[3];
        }
        return true;
    }
    case LeaderboardOp::Label:
        if (auto label = StringOf(value(0))) {
            if (item) item->label = std::move(*label);
            return true;
        }
        return false;
    case LeaderboardOp::Value:
        if (const auto number = NumberOf(value(0))) {
            if (item) item->value = static_cast<std::int32_t>(*number);
            return true;
        }
        return false;
    case LeaderboardOp::ItemStyle: {
        const auto showLabel = BooleanOf(value(0));
        const auto showValue = BooleanOf(value(1));
        const auto showIcon = BooleanOf(value(2));
        if (!showLabel || !showValue || !showIcon) return false;
        if (item) {
            item->showLabel = *showLabel;
            item->showValue = *showValue;
            item->showIcon = *showIcon;
        }
        return true;
    }
    case LeaderboardOp::LabelColor:
    case LeaderboardOp::ValueColor: {
        const auto color = ColorArguments(args, spec.valueArg, spec.percent);
        if (!color) return false;
        const bool label = spec.leaderboard == LeaderboardOp::LabelColor;
        const auto paint = [&](ScriptLeaderboardItem& each) { (label ? each.labelColor : each.valueColor) = *color; };
        if (item) paint(*item);
        else if (!missing) std::for_each(items.begin(), items.end(), paint);
        return true;
    }
    case LeaderboardOp::Sort: {
        LeaderboardSort by = spec.sort;
        int ascendingAt = 0;
        if (by == LeaderboardSort::ByArgument) {
            // bj_SORTTYPE_SORTBYVALUE 0、SORTBYPLAYER 1、SORTBYLABEL 2，其他值游戏不做任何事。
            const auto type = NumberOf(value(0));
            if (!type) return false;
            if (*type != 0.0f && *type != 1.0f && *type != 2.0f) return true;
            by = static_cast<LeaderboardSort>(static_cast<int>(*type));
            ascendingAt = 1;
        }
        const auto ascending = BooleanOf(value(ascendingAt));
        if (!ascending) return false;
        if (by == LeaderboardSort::Player &&
            std::any_of(items.begin(), items.end(), [](const ScriptLeaderboardItem& each) { return !each.player; })) {
            return false;
        }
        // 游戏按数值差、玩家序号差或标签逐字节比较排序（不稳定排序）；相同的键记下，预览保持原先后。
        const auto compare = [by, &strings](const ScriptLeaderboardItem& a, const ScriptLeaderboardItem& b) {
            switch (by) {
            case LeaderboardSort::Player: return a.player < b.player ? -1 : a.player > b.player ? 1 : 0;
            case LeaderboardSort::Label:
                return ResolveTriggerString(strings, a.label).compare(ResolveTriggerString(strings, b.label));
            default: return a.value < b.value ? -1 : a.value > b.value ? 1 : 0;
            }
        };
        std::stable_sort(items.begin(), items.end(), [&](const auto& a, const auto& b) {
            return *ascending ? compare(a, b) < 0 : compare(b, a) < 0;
        });
        for (std::size_t i = 1; i < items.size(); ++i) {
            if (compare(items[i - 1], items[i]) == 0) board.sortTies = true;
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace War3Map::GameUi::detail
