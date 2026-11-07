#pragma once
#include "Framework.h"

namespace War3Map::HLua {
struct ProjectConfiguration {
    bool recognized = false;
    std::string sha256;
    std::filesystem::path frameworkOverride;
};
[[nodiscard]] Result<ProjectConfiguration> ReadProjectConfiguration(const std::filesystem::path& project);
[[nodiscard]] Status ConfigureProjectFramework(const std::filesystem::path& project,
    const std::filesystem::path& framework, std::string_view expectedConfigurationSha256);
[[nodiscard]] Result<FrameworkLocation> ResolveProjectFramework(const std::filesystem::path& project,
    const FrameworkLocation& explicitLocation = {}, const FrameworkLocation& applicationDefault = {});
[[nodiscard]] Status VerifyProjectConfiguration(const std::filesystem::path& project, const ProjectConfiguration& expected);
} // namespace War3Map::HLua
