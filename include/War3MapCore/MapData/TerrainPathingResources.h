#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/Workspace.h"

#include <functional>
#include <map>

namespace War3Map::Services { class TerrainPathingSave; }

namespace War3Map::MapData {

// 实际读取的地形语义输入；只能由真实地图/游戏来源建立。
// 旧Capture证明等价修改；CaptureNative另捕获完整生成输入，仍由保存计划
// 比较旧WPM六位并保留0x11与显式工程意图，不从合成位猜作者来源。
// CaptureWaterBrushRules仅捕获编辑规则，不建立保存资格。
class TerrainPathingResources {
public:
    [[nodiscard]] static bool Needed(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after);
    [[nodiscard]] static Result<TerrainPathingResources> Capture(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    // maximumBytes约束累计实际来源读取，并限制每次W3E身份序列化缓冲；
    // 不是整个保存事务的峰值工作内存预算，调用者另计快照、G0/G1和输出容量。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNative(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes);
    // 编辑器采用的经典固定初始化规则链；完整捕获其map/coreMPQ winner或缺失。
    // sourceTerrain必须属于实际来源W3E；currentTerrain为本笔冻结的Session草稿，
    // 其palette可包含此前的贴图编辑。enforceWaterLimits是本笔选项，不读取注册表。
    // 仅提供编辑算法输入，不授予原版运行时配置或保存资格；保存仍须CaptureNative。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    // 未编辑来源的便利重载，转发sourceTerrain=currentTerrain。
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] const TerrainWaterLoadedRules* WaterBrushRules() const noexcept {
        return waterBrushRules_ ? &*waterBrushRules_ : nullptr;
    }
    // 调用方须将冻结current的canonical W3E身份与落笔前Session核对；不是after身份。
    [[nodiscard]] std::string_view WaterBrushInputSha256() const noexcept { return waterBrushInputSha_; }
    [[nodiscard]] std::size_t WaterBrushInputBytes() const noexcept { return waterBrushInputBytes_; }
    // 仅真实水刷规则owner可验证捕获时的source/current；分别比较canonical W3E身份。
    // 不读盘、不授予保存资格；宿主另复核实际资源来源与Session修订。
    [[nodiscard]] Status VerifyWaterBrushInput(const TerrainGridSnapshot& sourceTerrain,
        const TerrainGridSnapshot& currentTerrain) const;
    // 只允许本次捕获的旧/新W3E内容；输出不含头、源WPM及工程意图。
    // 身份缓冲在生成输出前释放；maximumBytes分别约束该缓冲和生成输出。
    [[nodiscard]] Result<std::vector<std::byte>> GenerateNative(const TerrainGridSnapshot& terrain,
        std::size_t maximumBytes) const;
    [[nodiscard]] bool EquivalentGround(std::string_view before, std::string_view after) const;
    // 无坡道顶点的原版 D=0/1/2 和严格覆水状态都必须相等；不使用显示网格高度。
    [[nodiscard]] bool EquivalentWater(const TerrainVertex& before, const TerrainVertex& after) const;
    [[nodiscard]] Status VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals) const;
    // 水刷规则owner在此只复核原始来源W3E/规则；不能用编辑后的输出冒充原始来源。
    [[nodiscard]] Status VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifyReadback(const WorkspaceSnapshot& saved, std::size_t maximumBytes) const;
    [[nodiscard]] std::size_t SourceBytes() const noexcept { return sourceBytes_; }
    // 原字节读取和严格物编解析的累计工作预留；保存计划还需计入输出缓冲。
    [[nodiscard]] std::size_t CaptureBudgetBytes() const noexcept { return sourceBytes_ + objectWorkBytes_; }
    [[nodiscard]] std::size_t OwnedCapacityBytes() const noexcept;

private:
    friend class Services::TerrainPathingSave;
    TerrainPathingResources() = default;
    // 只有Services可信适配器可提供解析器；输入只能是同一次实际捕获的字节。
    // 成功值为已经在maximumBytes内预留的解析工作量，不是外部资格布尔。
    using NativeObjectValidator = std::function<Result<std::size_t>(
        std::span<const std::string_view>, std::span<const ArchiveExactMember>, std::size_t)>;
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativeWithObjects(const MapArchiveSession& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureNativeWithObjects(const WorkspaceSnapshot& source,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator);
    using MemberReader = std::function<Result<std::vector<ArchiveExactMember>>(
        std::span<const std::string>, const ArchiveExactMemberLimits&)>;
    [[nodiscard]] static Result<TerrainPathingResources> CaptureInputs(const MemberReader& reader,
        const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, bool native,
        const NativeObjectValidator* validator = nullptr);
    [[nodiscard]] static Result<TerrainPathingResources> CaptureWaterBrushInputs(const MemberReader& reader,
        const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
        const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes);
    // requiredCount之前的成员必须有实际winner；余项允许缺失并固定缺失身份。
    using ResourceVisitor = std::function<void(std::size_t, bool, std::vector<std::byte>&&)>;
    [[nodiscard]] Status CaptureResourceMembers(const MemberReader& reader,
        std::span<const std::string> names, std::size_t requiredCount, const ResourceVisitor& visitor);
    [[nodiscard]] Status VerifyReadbackMembers(const MemberReader& reader, std::size_t maximumBytes) const;
    [[nodiscard]] Status VerifyGameDependencies(std::size_t remainingBytes) const;
    struct Dependency {
        std::string name, sha256, gameArchive;
        std::size_t bytes = 0;
        bool mapPresent = false;
        bool mapOnly = false;
        bool gameAbsent = false;
    };
    std::vector<Dependency> dependencies_;
    std::map<std::string, std::array<bool, 3>, std::less<>> ground_;
    std::filesystem::path gameRoot_;
    std::uint8_t tileset_ = 0;
    std::size_t sourceBytes_ = 0;
    std::size_t objectWorkBytes_ = 0;
    std::size_t capturedMaximumBytes_ = 0;
    std::vector<TerrainNativeGroundRule> nativeGround_;
    std::optional<TerrainNativeWaterRules> waterRules_;
    std::optional<TerrainWaterLoadedRules> waterBrushRules_;
    std::string waterBrushInputSha_;
    std::size_t waterBrushInputBytes_ = 0;
    std::optional<TerrainNativePlayableBounds> nativeBounds_;
    std::vector<std::byte> nativeDoodads_;
    std::array<std::string, 2> nativeTerrainSha_;
    std::array<std::size_t, 2> nativeTerrainBytes_{};
};

} // namespace War3Map::MapData
