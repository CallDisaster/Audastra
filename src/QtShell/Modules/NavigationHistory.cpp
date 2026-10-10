#include "Modules/NavigationHistory.h"

namespace War3Map::Shell {
void NavigationHistory::Append(std::vector<NavigationBookmark>& stack, const NavigationBookmark& position) {
    if (!limit_ || (!stack.empty() && stack.back() == position)) return;
    stack.push_back(position);
    if (stack.size() > limit_) stack.erase(stack.begin());
}
void NavigationHistory::Push(const NavigationBookmark& from) {
    forward_.clear();
    Append(back_, from);
}
std::optional<NavigationBookmark> NavigationHistory::Peek(bool forward) const {
    const auto& stack = forward ? forward_ : back_;
    if (stack.empty()) return std::nullopt;
    return stack.back();
}
void NavigationHistory::Commit(bool forward, const NavigationBookmark& current) {
    auto& source = forward ? forward_ : back_;
    if (source.empty()) return;
    source.pop_back();
    Append(forward ? back_ : forward_, current);
}
void NavigationHistory::SkipInvalid(bool forward) {
    auto& source = forward ? forward_ : back_;
    if (!source.empty()) source.pop_back();
}
} // namespace War3Map::Shell
