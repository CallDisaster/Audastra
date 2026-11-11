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
        {20, 20, 22, 255}, // bg
        {25, 26, 29, 255}, // panel
        {34, 36, 40, 255}, // raised
        {43, 46, 52, 255}, // hover
        {45, 48, 55, 255}, // line
        {237, 237, 239, 255}, // text
        {213, 216, 222, 255}, // textBody
        {159, 227, 196, 255}, // inlineCodeAccent
        {143, 148, 158, 255}, // muted
        {108, 113, 123, 255}, // dim
        {85, 89, 99, 255}, // disabled
        {110, 231, 183, 255}, // accent
        {110, 231, 183, 26}, // accentSoft
        {61, 133, 104, 255}, // accentLine
        {31, 55, 46, 255}, // selection
        {60, 64, 72, 255}, // scrollbar
        {234, 179, 100, 255}, // amber
        {192, 132, 252, 255}, // violet
        {96, 165, 250, 255}, // blue
        {242, 132, 124, 255}, // red
    }},
    {"midnight", "深海", true, {
        {13, 19, 28, 255}, // bg
        {19, 28, 39, 255}, // panel
        {27, 39, 54, 255}, // raised
        {35, 51, 70, 255}, // hover
        {39, 57, 77, 255}, // line
        {235, 241, 247, 255}, // text
        {200, 213, 227, 255}, // textBody
        {159, 213, 235, 255}, // inlineCodeAccent
        {139, 160, 181, 255}, // muted
        {104, 125, 148, 255}, // dim
        {82, 101, 122, 255}, // disabled
        {125, 211, 252, 255}, // accent
        {125, 211, 252, 26}, // accentSoft
        {59, 120, 158, 255}, // accentLine
        {25, 50, 74, 255}, // selection
        {57, 80, 104, 255}, // scrollbar
        {234, 179, 100, 255}, // amber
        {194, 168, 245, 255}, // violet
        {125, 211, 252, 255}, // blue
        {242, 132, 124, 255}, // red
    }},
    {"paper", "浅色", false, {
        {239, 243, 241, 255}, // bg
        {248, 250, 249, 255}, // panel
        {255, 255, 255, 255}, // raised
        {227, 236, 231, 255}, // hover
        {210, 221, 215, 255}, // line
        {27, 40, 34, 255}, // text
        {51, 68, 60, 255}, // textBody
        {36, 107, 76, 255}, // inlineCodeAccent
        {88, 109, 99, 255}, // muted
        {110, 130, 120, 255}, // dim
        {148, 164, 156, 255}, // disabled
        {24, 107, 75, 255}, // accent
        {24, 107, 75, 20}, // accentSoft
        {122, 179, 153, 255}, // accentLine
        {216, 239, 227, 255}, // selection
        {178, 194, 186, 255}, // scrollbar
        {154, 103, 24, 255}, // amber
        {108, 73, 168, 255}, // violet
        {34, 107, 143, 255}, // blue
        {196, 56, 48, 255}, // red
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
