#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <functional>

namespace War3Map::Services {
struct ObjectCatalogSources;

// 输入只能由宿主的来源解析器提供。缺成员是nullopt，读取失败是Failure，不能降级。
struct MapObjectReferenceSource {
    std::string source; // 实际来源路径/成员，不由客户端赋予类别。
    std::optional<std::vector<std::byte>> bytes;
    Trigger::MapObjectReferenceSourceScope scope = Trigger::MapObjectReferenceSourceScope::External;
};

class MapObjectReferenceCapture final {
public:
    using Reader = std::function<Result<MapObjectReferenceSource>(std::string_view)>;
    // nativeReader仅请求war3map.w3u/w3t；standardReader请求实际生效的UnitData/ItemData完整表。
    // 分类读取器可为空；此时只有真实native类别能给出正向资格，绝不从非item推unit。
    [[nodiscard]] static Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> Capture(
        const MapDocument& map, const Reader& nativeReader, const Reader& standardReader = {});
    // 比较原成员及真实分类输入身份；不把展开目录aggregate SHA与归档SHA混用。
    [[nodiscard]] static Status Verify(const Trigger::MapObjectReferenceSnapshot& expected,
        const MapDocument& map, const Reader& nativeReader, const Reader& standardReader = {});
};

[[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> CaptureDirectoryMapObjectReferences(
    const std::filesystem::path& root, const std::filesystem::path& gameRoot = {},
    const ObjectCatalogSources* classificationSources = nullptr);
[[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> CaptureArchiveMapObjectReferences(
    const MapArchiveSession& archive, const std::filesystem::path& gameRoot = {},
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {},
    const ObjectCatalogSources* classificationSources = nullptr);
// 展开目录与归档可显式迁移相同地图成员；游戏/战役分类来源不允许换层或换身份。
[[nodiscard]] Status VerifyMapObjectReferenceContents(const Trigger::MapObjectReferenceSnapshot& expected,
    const Trigger::MapObjectReferenceSnapshot& actual);
[[nodiscard]] Status VerifyMapObjectReferenceStandardSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const std::filesystem::path& gameRoot);
// 当前有效来源复核；包括地图内SLK、会话覆盖与明确配置的下层。
[[nodiscard]] Status VerifyMapObjectReferenceClassificationSources(const Trigger::MapObjectReferenceSnapshot& expected,
    const ObjectCatalogSources& sources);
[[nodiscard]] Status VerifyDirectoryMapObjectReferenceClassificationSources(
    const Trigger::MapObjectReferenceSnapshot& expected, const std::filesystem::path& root,
    const std::filesystem::path& gameRoot = {});

} // namespace War3Map::Services
