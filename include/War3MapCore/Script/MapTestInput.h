#pragma once
#include "War3MapCore/Types.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace War3Map::Script {
enum class MapTestInputKind { Click, Key, Close };
struct MapTestInput {
    MapTestInputKind kind = MapTestInputKind::Click;
    double x = 0, y = 0;
    std::vector<std::uint32_t> keys;
};
struct MapTestInputReceipt {
    std::uint32_t processId = 0, sequence = 0;
    std::uint64_t window = 0;
    int width = 0, height = 0, pixelX = 0, pixelY = 0;
    bool cursorObserved = false, keyObserved = false;
    std::string method;
};
struct MapTestProcessExit {
    std::uint32_t processId = 0, exitCode = 0;
    bool launcher = false, game = false, known = false;
};
struct MapTestNormalExitStep {
    double delaySeconds = 0;
    std::string stage;
    MapTestInput input;
};
// 仅已核验的经典测试环境使用；通过游戏菜单正常退出，动作送达不等于进程已退出。
[[nodiscard]] std::vector<MapTestNormalExitStep> MapTestNormalExit127();
[[nodiscard]] Result<std::vector<std::uint32_t>> ParseMapTestKeys(std::string_view text);
[[nodiscard]] Status ValidateMapTestInput(const MapTestInput& input);
// 经典界面0..0.8/0..0.6按整个客户区拉伸；不猜测渲染器另设的letterbox视口。
[[nodiscard]] Result<std::pair<int, int>> MapTestInputPixel(double x, double y, int width, int height);
}
