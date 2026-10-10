#pragma once
#include <filesystem>
#include <optional>
#include <string_view>

int RunScriptMigrationCheck(const std::filesystem::path& map, std::string_view sourceSha256,
    std::string_view target, const std::optional<std::filesystem::path>& toolRoot);
