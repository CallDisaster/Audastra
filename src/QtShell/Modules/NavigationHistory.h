#pragma once

#include "Modules/EditorModule.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace War3Map::Shell {
struct NavigationBookmark {
    ModuleId module;
    QJsonObject state;
    std::uint64_t epoch = 0;
    bool operator==(const NavigationBookmark&) const = default;
};

// 游标只在恢复成功后提交；无效目标可显式跳过，Blocked 不改变任一方向栈。
class NavigationHistory final {
public:
    explicit NavigationHistory(std::size_t limit = 50) : limit_(limit) {}
    void Push(const NavigationBookmark& from);
    void ClearForward() noexcept { forward_.clear(); }
    [[nodiscard]] std::optional<NavigationBookmark> Peek(bool forward) const;
    void Commit(bool forward, const NavigationBookmark& current);
    void SkipInvalid(bool forward);
    [[nodiscard]] bool CanGoBack() const noexcept { return !back_.empty(); }
    [[nodiscard]] bool CanGoForward() const noexcept { return !forward_.empty(); }
    void Clear() noexcept { back_.clear(); forward_.clear(); }
private:
    void Append(std::vector<NavigationBookmark>& stack, const NavigationBookmark& position);
    std::size_t limit_;
    std::vector<NavigationBookmark> back_, forward_;
};
} // namespace War3Map::Shell
