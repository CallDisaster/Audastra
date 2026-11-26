#pragma once
#include <algorithm>
#include <cstdint>

namespace War3Map::Modules {
// 播放时钟（Qt-free，调用方提供 steady 纳秒时刻并负责同步）。每次消耗只扣整毫秒，不足 1 ms 的余数留给下一次，
// 长时间播放不单向漂移；落后超过 1 秒只给 1 秒，多出的部分记为丢弃，不偷偷补成巨步。
// 恒等式：clockNs − startNs = 累计扣除 + 丢弃；实际经过与二者之和的差就是余数，消耗后恒在 [0, 1 ms)。
struct WorldPlaybackClock {
    std::int64_t startNs = 0, clockNs = 0; // clockNs：已消耗到的时刻；0 = 尚未启动
    std::uint64_t consumedMs = 0, droppedNs = 0;
    void Start(std::int64_t now) noexcept { startNs = clockNs = now; consumedMs = 0; droppedNs = 0; }
    [[nodiscard]] bool Valid() const noexcept { return clockNs != 0; }
    [[nodiscard]] std::int64_t ElapsedNs(std::int64_t now) const noexcept { return now - startNs; }
    [[nodiscard]] std::uint64_t ConsumedNs() const noexcept { return consumedMs * 1'000'000 + droppedNs; }
    std::uint64_t Consume(std::int64_t now) noexcept {
        if (!clockNs) return 0;
        const auto elapsed = std::max<std::int64_t>(now - clockNs, 0);
        const auto ms = elapsed / 1'000'000;
        if (ms >= 1000) {
            droppedNs += static_cast<std::uint64_t>(elapsed - 1'000'000'000);
            consumedMs += 1000; clockNs += elapsed;
            return 1000;
        }
        consumedMs += static_cast<std::uint64_t>(ms); clockNs += ms * 1'000'000;
        return static_cast<std::uint64_t>(ms);
    }
};
// 接续帧固定网格（Qt-free，调用方持锁）：每帧应开算时刻 = 上一帧网格时刻 + 间隔。晚不到一个间隔就沿网格追回
// （下一帧应开算时刻不随这次晚开顺延），晚一个间隔以上才从这次实际开算时刻重排，不连续补算。
// slotNs = 0 表示未开网格（界面线程开算或时钟重启后），这时按调用方给的回退时刻（时钟 + 间隔）起算。
struct WorldChainGrid {
    std::int64_t slotNs = 0;
    [[nodiscard]] std::int64_t DueNs(std::int64_t fallbackDueNs, std::int64_t intervalNs) const noexcept {
        return slotNs ? slotNs + intervalNs : fallbackDueNs;
    }
    // 本帧在 nowNs 开算、应开算时刻为 dueNs：记下本帧网格时刻，返回晚于网格的纳秒数（不早于 0）。
    std::int64_t Start(std::int64_t dueNs, std::int64_t nowNs, std::int64_t intervalNs) noexcept {
        const auto late = std::max<std::int64_t>(nowNs - dueNs, 0);
        slotNs = late >= intervalNs ? nowNs : dueNs;
        return late;
    }
    void Reset() noexcept { slotNs = 0; }
};
} // namespace War3Map::Modules
