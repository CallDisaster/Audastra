#pragma once

#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"

#include <array>
#include <optional>

namespace War3Map::MapData {

// 本编辑器明确采用的fresh-start经典固定profile规则；持有真实来源owner。
// 不读取地图覆盖、不读取DNC，也不冒充任意原版进程的旧terrain方向缓存。
// 原版方向在地形构造时复制；宿主须把本owner与自己的地形/session修订绑定。
class TerrainShadowResources {
public:
    [[nodiscard]] static Result<TerrainShadowResources> Capture(
        const std::filesystem::path& gameRoot, Game::NativeEditorStartupRules rules, std::size_t maximumBytes);
    [[nodiscard]] const std::array<float, 3>& LightDirection() const noexcept { return lightDirection_; }
    // 与Light相同实际startup追加链中的Terrain.MaxHeight/MinHeight首value。
    // 缺失或未知数字语法不影响无caster方向；有caster必须拒绝缺少此输入。
    [[nodiscard]] const std::optional<std::array<float, 2>>& TerrainHeightBounds() const noexcept { return terrainHeightBounds_; }
    [[nodiscard]] std::string_view DependencyFingerprint() const noexcept { return fingerprint_; }
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::size_t CaptureBudgetBytes() const noexcept { return captureBudgetBytes_; }
    // 重放同一fresh-start来源链，检查完整56项winner/absence/原字节及根目录身份。
    [[nodiscard]] Status VerifySources(std::size_t maximumBytes) const;

private:
    TerrainShadowResources() = default;
    std::optional<Game::NativeDoodadCategoryDirectory> profileOwner_;
    std::array<float, 3> lightDirection_{};
    std::optional<std::array<float, 2>> terrainHeightBounds_;
    std::string fingerprint_;
    std::size_t sourceBytes_ = 0, captureBudgetBytes_ = 0;
};

} // namespace War3Map::MapData
