#pragma once

#include <cmath>

// 经典版界面坐标与屏幕像素的换算。界面坐标宽 0.8、高 0.6，铺满整个游戏画面：宽屏下横向拉伸，
// 所以同样的像素数在横向和纵向对应不同的界面长度（1920×1080 下 64×64 像素是 0.0267 × 0.0356）。
// 界面工作区与助手工具共用。
namespace War3Map::GameUi {

inline constexpr double kUiScreenWidth = 0.8;
inline constexpr double kUiScreenHeight = 0.6;

struct UiResolution {
    int width = 1920;
    int height = 1080;
    [[nodiscard]] bool Valid() const noexcept { return width > 0 && height > 0 && width <= 16384 && height <= 16384; }
};

[[nodiscard]] inline double UiUnitsFromPixelsX(double pixels, const UiResolution& resolution) {
    return pixels * kUiScreenWidth / resolution.width;
}
[[nodiscard]] inline double UiUnitsFromPixelsY(double pixels, const UiResolution& resolution) {
    return pixels * kUiScreenHeight / resolution.height;
}
[[nodiscard]] inline double UiPixelsFromUnitsX(double units, const UiResolution& resolution) {
    return units * resolution.width / kUiScreenWidth;
}
[[nodiscard]] inline double UiPixelsFromUnitsY(double units, const UiResolution& resolution) {
    return units * resolution.height / kUiScreenHeight;
}

// 写进描述的界面长度：保留 6 位小数，像素换算来的值往返后与原像素相差不到 0.01 像素。
[[nodiscard]] inline double RoundUiUnits(double units) { return std::round(units * 1.0e6) / 1.0e6; }

} // namespace War3Map::GameUi
