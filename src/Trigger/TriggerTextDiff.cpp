#include "War3MapCore/Trigger/TriggerTextDiff.h"

#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <algorithm>
#include <map>
#include <string_view>

namespace War3Map::Trigger {
namespace {

bool SameEca(const TriggerDocument& left, std::uint32_t a, const TriggerDocument& right, std::uint32_t b, std::size_t depth);

bool SameArgument(const TriggerDocument& left, std::uint32_t a, const TriggerDocument& right, std::uint32_t b, std::size_t depth) {
    if (a >= left.argumentNodes.size() || b >= right.argumentNodes.size() || depth > 256) return false;
    const auto& x = left.argumentNodes[a];
    const auto& y = right.argumentNodes[b];
    if (x.type != y.type || x.value.text != y.value.text || x.callEca.has_value() != y.callEca.has_value()
        || x.indexArgument.has_value() != y.indexArgument.has_value()) return false;
    if (x.callEca && !SameEca(left, *x.callEca, right, *y.callEca, depth + 1)) return false;
    return !x.indexArgument || SameArgument(left, *x.indexArgument, right, *y.indexArgument, depth + 1);
}

bool SameEca(const TriggerDocument& left, std::uint32_t a, const TriggerDocument& right, std::uint32_t b, std::size_t depth) {
    if (a >= left.ecaNodes.size() || b >= right.ecaNodes.size() || depth > 256) return false;
    const auto& x = left.ecaNodes[a];
    const auto& y = right.ecaNodes[b];
    if (x.type != y.type || x.childId != y.childId || x.name.text != y.name.text || x.enabled != y.enabled
        || x.arguments.size() != y.arguments.size() || x.children.size() != y.children.size()) return false;
    for (std::size_t i = 0; i < x.arguments.size(); ++i)
        if (!SameArgument(left, x.arguments[i], right, y.arguments[i], depth + 1)) return false;
    for (std::size_t i = 0; i < x.children.size(); ++i)
        if (!SameEca(left, x.children[i], right, y.children[i], depth + 1)) return false;
    return true;
}

bool SameTrigger(const TriggerDocument& left, const TriggerRecord& x, const TriggerDocument& right, const TriggerRecord& y) {
    if (x.name.text != y.name.text || x.description.text != y.description.text || x.type != y.type || x.enabled != y.enabled
        || x.customTextFlag != y.customTextFlag || x.initiallyClosed != y.initiallyClosed || x.runOnInit != y.runOnInit
        || x.categoryId != y.categoryId || x.customText.text != y.customText.text || x.ecas.size() != y.ecas.size()) return false;
    for (std::size_t i = 0; i < x.ecas.size(); ++i)
        if (!SameEca(left, x.ecas[i], right, y.ecas[i], 0)) return false;
    return true;
}

std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find('\n');
        lines.push_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return lines;
}

// 最长公共子序列逐行差异；只保留改动附近两行上下文。规模过大时退回“整段删除 + 整段新增”。
std::string LineDiff(std::string_view before, std::string_view after) {
    const auto a = Lines(before), b = Lines(after);
    const auto n = a.size(), m = b.size();
    enum class Op : char { Same, Delete, Insert };
    std::vector<std::pair<Op, std::string_view>> ops;
    if (n * m <= 2'000'000) {
        std::vector<std::uint32_t> table((n + 1) * (m + 1), 0);
        const auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return table[i * (m + 1) + j]; };
        for (std::size_t i = n; i-- > 0;)
            for (std::size_t j = m; j-- > 0;)
                at(i, j) = a[i] == b[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        std::size_t i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[i] == b[j]) { ops.emplace_back(Op::Same, a[i]); ++i; ++j; }
            else if (j < m && (i == n || at(i, j + 1) >= at(i + 1, j))) ops.emplace_back(Op::Insert, b[j++]);
            else ops.emplace_back(Op::Delete, a[i++]);
        }
    } else {
        for (const auto line : a) ops.emplace_back(Op::Delete, line);
        for (const auto line : b) ops.emplace_back(Op::Insert, line);
    }
    std::vector<bool> keep(ops.size(), false);
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].first == Op::Same) continue;
        for (std::size_t c = k >= 2 ? k - 2 : 0; c <= std::min(ops.size() - 1, k + 2); ++c) keep[c] = true;
    }
    std::string out;
    std::size_t afterLine = 1;
    bool inHunk = false;
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (!keep[k]) { inHunk = false; if (ops[k].first != Op::Delete) ++afterLine; continue; }
        if (!inHunk) { out += "@@ 第 " + std::to_string(afterLine) + " 行\n"; inHunk = true; }
        out += ops[k].first == Op::Same ? "  " : ops[k].first == Op::Delete ? "- " : "+ ";
        out.append(ops[k].second);
        out += '\n';
        if (ops[k].first != Op::Delete) ++afterLine;
    }
    return out;
}

std::string Render(const TriggerDocument& document, const UiCatalog& catalog, const TriggerRecord& trigger, std::string& note) {
    if (trigger.customTextFlag != 0) { note = "自定义代码触发器，没有紧凑 T 文本。"; return {}; }
    auto text = RenderTriggerText(document, catalog, trigger.id);
    if (!text.Succeeded() || !text.value) {
        note = text.diagnostics.empty() ? "无法导出紧凑 T 文本。" : text.diagnostics.front().message;
        return {};
    }
    return std::move(*text.value);
}

} // namespace

std::vector<TriggerTextChange> DiffTriggerTexts(const TriggerDocument& before, const TriggerDocument& after,
    const UiCatalog& catalog, std::size_t maxBytes) {
    std::map<StableId, const TriggerRecord*> old;
    for (const auto& trigger : before.triggers) old.emplace(trigger.id, &trigger);
    std::vector<TriggerTextChange> changes;
    std::size_t bytes = 0;
    bool truncated = false;
    const auto add = [&](TriggerTextChange change) {
        if (truncated) return;
        const auto size = change.diff.size() + change.name.size() + change.note.size();
        if (size > maxBytes - std::min(bytes, maxBytes)) {
            truncated = true;
            change.diff.clear();
            change.note = "差异总量超过上限，此后的触发器未列出差异。";
        }
        bytes += size;
        changes.push_back(std::move(change));
    };
    for (const auto& trigger : after.triggers) {
        const auto found = old.find(trigger.id);
        if (found != old.end() && SameTrigger(before, *found->second, after, trigger)) { old.erase(found); continue; }
        TriggerTextChange change{trigger.id, trigger.name.text, found == old.end() ? "added" : "changed", {}, {}};
        std::string beforeNote, afterNote;
        const auto next = Render(after, catalog, trigger, afterNote);
        const auto previous = found == old.end() ? std::string{} : Render(before, catalog, *found->second, beforeNote);
        change.note = !afterNote.empty() ? afterNote : beforeNote;
        if (change.note.empty()) change.diff = LineDiff(previous, next);
        if (found != old.end()) old.erase(found);
        add(std::move(change));
    }
    for (const auto& [id, trigger] : old) {
        TriggerTextChange change{id, trigger->name.text, "removed", {}, {}};
        const auto previous = Render(before, catalog, *trigger, change.note);
        if (change.note.empty()) change.diff = LineDiff(previous, {});
        add(std::move(change));
    }
    return changes;
}

} // namespace War3Map::Trigger
