#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Types.h"
#include <memory>
#include <span>

namespace War3Map::Services {

inline constexpr std::string_view kMapRuntimeSettingsMember = "audastra/runtime/settings.json";
inline constexpr std::string_view kMapRuntimeInstallationMember = "audastra/runtime/package.json";

enum class MapRuntimeMode { Automatic, Enabled, Disabled };
struct MapRuntimeSettings {
    MapRuntimeMode mode = MapRuntimeMode::Automatic;
    // 地图中只保存身份，不保存可供宿主读取的文件系统路径；空值表示尚未批准。
    std::string packageSha256;
    bool operator==(const MapRuntimeSettings&) const = default;
};

[[nodiscard]] Result<MapRuntimeSettings> ParseMapRuntimeSettings(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapRuntimeSettings(const MapRuntimeSettings& settings);

enum class MapRuntimeSavePurpose { Normal, InternalValidation, TestRun };

// 只由可信宿主配置。地图内 JSON 不能构造本地包路径或提高分发许可。
struct MapRuntimePackageSource {
    std::filesystem::path root;
    std::string manifestSha256;
    bool publicDistributionApproved = false;
};

// 拥有完整不透明字节快照，不暴露引导器文本。捕获与复核均只按清单身份处理文件。
class MapRuntimePackage final {
public:
    [[nodiscard]] static Result<std::shared_ptr<const MapRuntimePackage>> Capture(
        const MapRuntimePackageSource& source, MapRuntimeSavePurpose purpose);
    [[nodiscard]] Status VerifySource() const;
    [[nodiscard]] Status VerifyScriptInputs(std::span<const std::string> injectedSourceSha256) const;
    [[nodiscard]] const std::string& Identity() const noexcept { return source_.manifestSha256; }
    [[nodiscard]] const std::string& Id() const noexcept { return id_; }
    [[nodiscard]] const std::string& Version() const noexcept { return version_; }
    [[nodiscard]] const std::string& PayloadSha256() const noexcept { return payloadSha256_; }
    [[nodiscard]] bool InternalOnly() const noexcept { return internalOnly_; }
    [[nodiscard]] const std::vector<MemberReplacement>& Members() const noexcept { return members_; }
private:
    struct File { std::filesystem::path path; std::uint64_t size; std::string sha256; };
    MapRuntimePackageSource source_;
    std::string id_, version_, payloadSha256_, clientSha256_;
    bool internalOnly_ = true;
    std::vector<File> files_;
    std::vector<MemberReplacement> members_;
};

} // namespace War3Map::Services
