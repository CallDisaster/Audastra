#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <functional>

namespace War3Map::Services {

// 输入只能由宿主的来源解析器提供。缺成员是nullopt，读取失败是Failure，不能降级。
struct MapObjectReferenceSource {
    std::string source; // 实际来源路径/成员，不由客户端赋予类别。
    std::optional<std::vector<std::byte>> bytes;
};

class MapObjectReferenceCapture final {
public:
    using Reader = std::function<Result<MapObjectReferenceSource>(std::string_view)>;
    // nativeReader仅请求war3map.w3u/w3t；standardReader仅请求Units/UnitData.slk/ItemData.slk。
    // 原版表读取器可为空；此时只有真实native类别能给出正向资格，绝不从非item推unit。
    [[nodiscard]] static Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> Capture(
        const MapDocument& map, const Reader& nativeReader, const Reader& standardReader = {});
    // 比较原成员及真实分类输入身份；不把展开目录aggregate SHA与归档SHA混用。
    [[nodiscard]] static Status Verify(const Trigger::MapObjectReferenceSnapshot& expected,
        const MapDocument& map, const Reader& nativeReader, const Reader& standardReader = {});
};

[[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> CaptureDirectoryMapObjectReferences(
    const std::filesystem::path& root, const std::filesystem::path& gameRoot = {});
[[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> CaptureArchiveMapObjectReferences(
    const MapArchiveSession& archive, const std::filesystem::path& gameRoot = {},
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {});
// 展开目录和归档的来源位置不同，逐逻辑角色核对实际字节；游戏分类来源不允许换根或遮蔽。
[[nodiscard]] Status VerifyMapObjectReferenceContents(const Trigger::MapObjectReferenceSnapshot& expected,
    const Trigger::MapObjectReferenceSnapshot& actual);
[[nodiscard]] Status VerifyMapObjectReferenceStandardSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const std::filesystem::path& gameRoot);

} // namespace War3Map::Services
