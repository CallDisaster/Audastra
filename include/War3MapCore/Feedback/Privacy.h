#pragma once

#include "War3MapCore/Types.h"

#include <span>
#include <string_view>

namespace War3Map::Feedback {

enum class PathRole : std::uint8_t { Game, Ydwe, User };

struct PrivatePath {
    PathRole role = PathRole::User;
    std::string pathUtf8;
};

// 仅在当前进程内使用；这些原始路径、地图名和密钥绝不写入队列或报告。
struct PrivacyContext {
    std::vector<PrivatePath> paths;
    std::vector<std::string> mapNames;
    std::vector<std::string> secretValues;
};

struct PrivacyLimits {
    std::size_t maxTextBytes = 256 * 1024;
    std::size_t maxContextEntries = 128;
    std::size_t maxContextTextBytes = 4096;
};

struct CleanText {
    std::string text;
    std::size_t replacements = 0;
};

[[nodiscard]] Status ValidatePrivacyContext(const PrivacyContext& context, PrivacyLimits limits = {});
[[nodiscard]] Result<CleanText> CleanPrivateText(std::string_view text,
    const PrivacyContext& context = {}, PrivacyLimits limits = {});
[[nodiscard]] Result<std::string> MapNameHash8(std::string_view fileNameUtf8);

struct PreparedScreenshot {
    std::vector<std::byte> png;
    std::string sha256;
    [[nodiscard]] bool operator==(const PreparedScreenshot&) const = default;
};

// 只接受有界静态 PNG，移除文本/EXIF 等附加块。不能保证图片像素不含隐私。
// UI 应展示本函数的输出，让用户明确确认输出 SHA 后再交给 Draft。
[[nodiscard]] Result<PreparedScreenshot> PrepareScreenshot(std::span<const std::byte> png,
    std::size_t maxBytes = 4 * 1024 * 1024);

} // namespace War3Map::Feedback
