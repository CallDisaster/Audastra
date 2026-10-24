#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData { class TerrainShadowResources; }

namespace War3Map::Game {

// 程序实现的原版规则版本；不是调用方提供的类别资格或任意原版进程状态。
enum class NativeEditorStartupRules : std::uint8_t { Classic127aExpansion };

// 以配置gameRoot作为fresh editor exeDir，重建第一次profile reload以前复制的目录。
// 独立于map-first、tileset及Melee/Custom前缀来源；只由真实文件/MPQ读取建立。
class NativeDoodadCategoryDirectory {
public:
    [[nodiscard]] static Result<NativeDoodadCategoryDirectory> Capture(
        const std::filesystem::path& gameRoot, NativeEditorStartupRules rules, std::size_t maximumBytes);
    [[nodiscard]] std::uint8_t CategoryCSnapCells() const noexcept;
    // 重新执行完整固定启动链，核对winner、原字节及较高来源absence。
    [[nodiscard]] Status VerifySources(std::size_t maximumBytes) const;
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    [[nodiscard]] std::size_t WorkBytes() const noexcept { return workBytes_; }
    [[nodiscard]] std::size_t CaptureBudgetBytes() const noexcept { return sourceBytes_ + workBytes_; }
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;

private:
    friend class MapData::TerrainShadowResources;
    NativeDoodadCategoryDirectory() = default;
    // SHD仅观察本owner同次实际读取的完整profile字节；不另造来源或类别资格。
    using ProfileVisitor = std::function<Status(std::string_view, std::span<const std::byte>)>;
    [[nodiscard]] static Result<NativeDoodadCategoryDirectory> CaptureWithProfiles(
        const std::filesystem::path& gameRoot, NativeEditorStartupRules rules, std::size_t maximumBytes,
        const ProfileVisitor& visitor);
    struct ProfileSource {
        std::string logicalPath, winner, sha256;
        std::size_t bytes = 0;
        bool operator==(const ProfileSource&) const = default;
    };
    NativeEditorStartupRules rules_ = NativeEditorStartupRules::Classic127aExpansion;
    std::filesystem::path configuredRoot_, canonicalRoot_;
    // 每个五包入口均记录present/absent；成员winner按固定表顺序记录。
    std::vector<std::string> archivePaths_;
    std::vector<ProfileSource> profiles_;
    std::vector<std::string> categoryKeys_; // 首次有效定义的原始大小写。
    std::size_t sourceBytes_ = 0, workBytes_ = 0;
};

} // namespace War3Map::Game
