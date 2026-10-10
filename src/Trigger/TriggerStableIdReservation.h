#pragma once

// 私有分配优化；此文件不安装为公共 API，也不提供来源/编辑资格。
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::Trigger::Detail {

inline constexpr std::size_t kStableIdReserveMinimum = 4096;
inline constexpr std::size_t kStableIdReserveMaximum = 1024 * 1024;

// 只读五个实际 vector 的记录数。溢出/窗口外仅不预留，完整验证继续。
[[nodiscard]] constexpr std::size_t StableIdReservationCount(
    const std::array<std::size_t, 5>& counts) noexcept {
    std::size_t total = 0;
    for (const auto count : counts) {
        if (count > (std::numeric_limits<std::size_t>::max)() - total) return 0;
        total += count;
    }
    return total >= kStableIdReserveMinimum && total <= kStableIdReserveMaximum ? total : 0;
}

template<class Set>
void ReserveStableIdBuckets(Set& ids, const std::array<std::size_t, 5>& counts) {
    const auto count = StableIdReservationCount(counts);
    if (!count) return;
    try {
        ids.reserve(count);
    } catch (const std::bad_alloc&) {
        // 预留不是验证必要条件；空集合维持原 growth/异常路径。
    } catch (const std::length_error&) {
        // 同样只失去优化；其余验证/图分配的异常不由此 helper 吞掉。
    }
}

} // namespace War3Map::Trigger::Detail
