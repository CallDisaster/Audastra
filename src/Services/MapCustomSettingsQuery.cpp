#include "War3MapCore/Services/MapCustomSettingsQuery.h"

namespace War3Map::Services {
Result<std::string> QueryArchiveMapCustomSettings(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, std::string_view queryJson) {
    auto opened = MpqWorkspace::OpenReadOnly(source);
    if (!opened.Succeeded()) return {{}, opened.diagnostics};
    if (opened.value->SourceSha256() != expectedSourceSha256)
        return Result<std::string>::Failure("Map.CustomSettings", "SETTINGS_ARCHIVE_STALE", "源地图 SHA-256 已变化。");
    auto document = MapDocument::FromReadOnlyArchive(*opened.value);
    if (!document.Succeeded()) return {{}, document.diagnostics};
    const MapDataQueryBinding binding{"archive", 1, 0, opened.value->SourceSha256()};
    auto result = QueryMapCustomSettings(*document.value, binding, queryJson);
    auto verified = opened.value->VerifyUnchanged();
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    return result;
}
} // namespace War3Map::Services
