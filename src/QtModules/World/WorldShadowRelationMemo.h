#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace War3Map::Modules {

// 仅由UI线程同步使用：同一修订复用已核净变化，水帧/预览不重复比较整份W3E/WPM。
// 不持会话或地形指针；首个新修订仍由唯一比较器计算，不能拿指针差异代替净变化。
class WorldShadowRelationMemo {
public:
    template <class Compare>
    [[nodiscard]] std::optional<bool> Query(const MapData::MapDataEditSession& edits,
        const MapData::TerrainGridSnapshot* source, Compare&& compare) {
        // 先验本次真实source，禁止旧request借同修订memo取得当前会话关系。
        if (!source || edits.Terrain().get() != source) return std::nullopt;
        const Key current{edits.SessionIdentity(), edits.Revision(), edits.TerrainSourceRevision()};
        if (!key_ || *key_ != current) {
            const bool changed = std::forward<Compare>(compare)(edits);
            key_ = current;
            changed_ = changed;
        }
        return changed_;
    }

private:
    struct Key {
        std::uint64_t sessionIdentity = 0;
        std::uint64_t revision = 0;
        std::uint64_t terrainSourceRevision = 0;
        [[nodiscard]] bool operator==(const Key&) const noexcept = default;
    };
    std::optional<Key> key_;
    bool changed_ = false;
};

} // namespace War3Map::Modules
