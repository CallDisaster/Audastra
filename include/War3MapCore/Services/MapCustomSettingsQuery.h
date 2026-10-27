#pragma once
#include "War3MapCore/Services/MapCustomSettingsEditing.h"

namespace War3Map::Services {
[[nodiscard]] Result<std::string> QueryArchiveMapCustomSettings(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, std::string_view queryJson);
} // namespace War3Map::Services
