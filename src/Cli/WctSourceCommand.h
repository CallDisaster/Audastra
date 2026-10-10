#pragma once
#include <filesystem>
#include <string_view>

int RunWctSourceInspect(const std::filesystem::path& path, std::string_view sha,
    std::string_view start, std::string_view limit);
int RunWctSourceChunk(const std::filesystem::path& path, std::string_view sha,
    std::string_view block, std::string_view offset, std::string_view limit);
