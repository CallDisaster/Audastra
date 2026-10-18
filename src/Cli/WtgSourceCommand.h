#pragma once
#include <filesystem>
#include <string_view>

int RunWtgSourceInspect(const std::filesystem::path& file, std::string_view sourceSha,
    const std::filesystem::path& uiRoot, std::string_view maxNodes = {});
int RunWtgSourceQuery(const std::filesystem::path& file, std::string_view sourceSha,
    const std::filesystem::path& uiRoot, std::string_view catalogSha, std::string_view table,
    std::string_view start, std::string_view limit, std::string_view maxNodes = {}, std::string_view triggerItem = {});
