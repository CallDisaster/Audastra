// 由 tools/design_tokens.py 从 assets/design/tokens.json 生成，请勿手改。
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace War3Map::UiKit::Tokens {

struct Color {
    std::uint8_t r, g, b, a;
};

struct ThemeColors {
    Color bg;
    Color panel;
    Color raised;
    Color hover;
    Color line;
    Color text;
    Color textBody;
    Color inlineCodeAccent;
    Color muted;
    Color dim;
    Color disabled;
    Color accent;
    Color accentSoft;
    Color accentLine;
    Color selection;
    Color scrollbar;
    Color amber;
    Color violet;
    Color blue;
    Color red;
};

struct ThemeTokens {
    std::string_view id;
    std::string_view label;
    bool dark;
    ThemeColors colors;
};

inline constexpr std::array<ThemeTokens, 3> kThemes{{
    {"graphite", "石墨", true, {
        {21, 23, 25, 255}, // bg
        {27, 30, 32, 255}, // panel
        {35, 39, 41, 255}, // raised
        {42, 48, 49, 255}, // hover
        {48, 53, 56, 255}, // line
        {229, 233, 231, 255}, // text
        {201, 207, 204, 255}, // textBody
        {174, 203, 185, 255}, // inlineCodeAccent
        {155, 165, 160, 255}, // muted
        {120, 131, 125, 255}, // dim
        {105, 115, 110, 255}, // disabled
        {138, 214, 176, 255}, // accent
        {138, 214, 176, 18}, // accentSoft
        {82, 123, 101, 255}, // accentLine
        {40, 61, 51, 255}, // selection
        {73, 84, 78, 255}, // scrollbar
        {207, 182, 124, 255}, // amber
        {179, 160, 211, 255}, // violet
        {136, 183, 206, 255}, // blue
        {226, 142, 134, 255}, // red
    }},
    {"midnight", "深海", true, {
        {17, 25, 34, 255}, // bg
        {22, 33, 44, 255}, // panel
        {29, 43, 56, 255}, // raised
        {37, 58, 73, 255}, // hover
        {48, 67, 82, 255}, // line
        {229, 233, 231, 255}, // text
        {201, 207, 204, 255}, // textBody
        {176, 203, 195, 255}, // inlineCodeAccent
        {155, 165, 160, 255}, // muted
        {120, 131, 125, 255}, // dim
        {102, 116, 127, 255}, // disabled
        {151, 201, 232, 255}, // accent
        {151, 201, 232, 18}, // accentSoft
        {84, 113, 133, 255}, // accentLine
        {41, 57, 70, 255}, // selection
        {67, 86, 106, 255}, // scrollbar
        {207, 182, 124, 255}, // amber
        {179, 160, 211, 255}, // violet
        {136, 183, 206, 255}, // blue
        {226, 142, 134, 255}, // red
    }},
    {"paper", "浅色", false, {
        {233, 237, 235, 255}, // bg
        {247, 249, 248, 255}, // panel
        {255, 255, 255, 255}, // raised
        {223, 232, 226, 255}, // hover
        {206, 216, 209, 255}, // line
        {37, 51, 45, 255}, // text
        {60, 77, 67, 255}, // textBody
        {66, 110, 86, 255}, // inlineCodeAccent
        {85, 105, 94, 255}, // muted
        {98, 116, 106, 255}, // dim
        {130, 144, 135, 255}, // disabled
        {39, 111, 80, 255}, // accent
        {39, 111, 80, 16}, // accentSoft
        {145, 179, 157, 255}, // accentLine
        {222, 238, 227, 255}, // selection
        {180, 194, 185, 255}, // scrollbar
        {129, 101, 42, 255}, // amber
        {114, 85, 160, 255}, // violet
        {54, 117, 145, 255}, // blue
        {179, 61, 54, 255}, // red
    }},
}};

inline constexpr std::string_view kDefaultTheme = "graphite";

inline constexpr std::array<std::string_view, 7> kUiFontFamilies{"Microsoft YaHei UI", "Microsoft YaHei", "Source Han Sans SC VF", "Source Han Sans SC", "Segoe UI Variable", "Segoe UI", "sans-serif"};
inline constexpr std::array<std::string_view, 4> kCodeFontFamilies{"JetBrains Mono", "Cascadia Mono", "Consolas", "monospace"};

namespace Size {
inline constexpr int kFontTiny = 11;
inline constexpr int kFontSmall = 12;
inline constexpr int kFontBody = 13;
inline constexpr int kFontTitle = 16;
inline constexpr int kFontHeading = 20;
inline constexpr int kRadiusNone = 0;
inline constexpr int kRadiusXs = 4;
inline constexpr int kRadiusSm = 6;
inline constexpr int kRadiusMd = 8;
inline constexpr int kRadiusLg = 12;
inline constexpr int kRadiusXl = 16;
inline constexpr int kRadiusPill = 999;
inline constexpr int kRadiusSmall = 6;
inline constexpr int kRadius = 8;
inline constexpr int kRadiusLarge = 12;
inline constexpr int kShadowPopover = 8;
inline constexpr int kShadowDialog = 12;
inline constexpr int kPopoverPadding = 6;
inline constexpr int kMenuItemHeight = 32;
inline constexpr int kRowHeight = 28;
inline constexpr int kHeaderHeight = 48;
} // namespace Size

namespace Weight {
inline constexpr int kWeightRegular = 400;
inline constexpr int kWeightMedium = 500;
inline constexpr int kWeightBold = 700;
} // namespace Weight

} // namespace War3Map::UiKit::Tokens
