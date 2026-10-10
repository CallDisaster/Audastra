#pragma once

#include <filesystem>
#include <string_view>

int RunObjectLayerInspect(const std::filesystem::path& map);
int RunObjectLayerQuery(const std::filesystem::path& map, std::string_view expectedSourceSha256,
    std::string_view type, std::string_view offset, std::string_view limit);
