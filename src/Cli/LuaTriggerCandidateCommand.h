#pragma once
#include <filesystem>
#include <string_view>
#include <span>
#include <string>

int RunLuaTriggerCandidate(const std::filesystem::path& wtg, const std::filesystem::path& wct,
    const std::filesystem::path& uiRoot, const std::filesystem::path& profile, std::string_view revisionOrInspect,
    std::span<const std::string> options = {});
