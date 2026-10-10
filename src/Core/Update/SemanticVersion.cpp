#include "War3MapCore/Update/SemanticVersion.h"

#include <algorithm>

namespace War3Map::Update {
namespace {

bool Digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool Numeric(std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), Digit);
}
bool Identifier(char c) noexcept {
    return Digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
}
std::strong_ordering NumberCompare(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return left.size() < right.size()
        ? std::strong_ordering::less : std::strong_ordering::greater;
    return left <=> right;
}
Result<SemanticVersion> Invalid() {
    return Result<SemanticVersion>::Failure("Update", "UPDATE_VERSION_INVALID",
        "版本必须是严格的major.minor.patch语义化版本，可带合法预发布与构建后缀，最多128字节。");
}
bool Identifiers(std::string_view text, bool prerelease, std::vector<std::string>* result) {
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto end = text.find('.', offset);
        const auto part = text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
        if (part.empty() || !std::all_of(part.begin(), part.end(), Identifier)
            || (prerelease && Numeric(part) && part.size() > 1 && part.front() == '0')) return false;
        if (result) result->emplace_back(part);
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return true;
}

} // namespace

Result<SemanticVersion> ParseSemanticVersion(std::string_view text) {
    if (text.empty() || text.size() > 128) return Invalid();
    SemanticVersion version;
    version.text_ = text;
    const auto plus = text.find('+');
    if (plus != std::string_view::npos) {
        if (!Identifiers(text.substr(plus + 1), false, nullptr)) return Invalid();
        text = text.substr(0, plus);
    }
    const auto dash = text.find('-');
    if (dash != std::string_view::npos) {
        if (!Identifiers(text.substr(dash + 1), true, &version.prerelease_)) return Invalid();
        text = text.substr(0, dash);
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < version.core_.size(); ++i) {
        const auto end = text.find('.', offset);
        if ((i < 2) != (end != std::string_view::npos)) return Invalid();
        const auto part = text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
        if (!Numeric(part) || (part.size() > 1 && part.front() == '0')) return Invalid();
        version.core_[i] = part;
        offset = end == std::string_view::npos ? text.size() : end + 1;
    }
    return Result<SemanticVersion>::Success(std::move(version));
}

std::strong_ordering CompareSemanticVersions(const SemanticVersion& left, const SemanticVersion& right) noexcept {
    for (std::size_t i = 0; i < left.core_.size(); ++i) {
        const auto order = NumberCompare(left.core_[i], right.core_[i]);
        if (order != 0) return order;
    }
    if (left.prerelease_.empty() != right.prerelease_.empty()) return left.prerelease_.empty()
        ? std::strong_ordering::greater : std::strong_ordering::less;
    const auto count = (std::min)(left.prerelease_.size(), right.prerelease_.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = left.prerelease_[i];
        const auto& b = right.prerelease_[i];
        const bool aNumeric = Numeric(a), bNumeric = Numeric(b);
        if (aNumeric != bNumeric) return aNumeric ? std::strong_ordering::less : std::strong_ordering::greater;
        const auto order = aNumeric ? NumberCompare(a, b) : (a <=> b);
        if (order != 0) return order;
    }
    return left.prerelease_.size() <=> right.prerelease_.size();
}

} // namespace War3Map::Update
