#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <compare>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Update {

class SemanticVersion {
public:
    [[nodiscard]] const std::string& Text() const noexcept { return text_; }
    [[nodiscard]] bool HasPrerelease() const noexcept { return !prerelease_.empty(); }

private:
    std::string text_;
    std::array<std::string, 3> core_;
    std::vector<std::string> prerelease_;

    friend Result<SemanticVersion> ParseSemanticVersion(std::string_view);
    friend std::strong_ordering CompareSemanticVersions(const SemanticVersion&, const SemanticVersion&) noexcept;
};

// 严格 SemVer 2.0，最多128个ASCII字节。v前缀只允许由发布来源适配器处理。
[[nodiscard]] Result<SemanticVersion> ParseSemanticVersion(std::string_view text);
// build metadata不参与优先级，任意长度的合法数字不转换成固定宽度整数。
[[nodiscard]] std::strong_ordering CompareSemanticVersions(
    const SemanticVersion& left, const SemanticVersion& right) noexcept;

} // namespace War3Map::Update
