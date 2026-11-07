#pragma once
#include "Framework.h"

namespace War3Map::HLua {
struct ProjectConfiguration {
    bool recognized = false;
    std::string sha256;
    std::filesystem::path frameworkOverride;
};
inline constexpr std::string_view kBuildStateMember="audastra/editor/hlua-build.json";
struct ProjectBuildState {std::string projectSha256,frameworkSha256,configurationSha256;};
[[nodiscard]] Result<std::vector<std::byte>> EncodeProjectBuildState(const ProjectBuildState& state);
[[nodiscard]] Result<std::optional<ProjectBuildState>> ReadProjectBuildState(const MapArchiveSession& map);
[[nodiscard]] Result<bool> RecognizeProject(const std::filesystem::path& project);
[[nodiscard]] Result<ProjectConfiguration> ReadProjectConfiguration(const std::filesystem::path& project);
[[nodiscard]] Status ConfigureProjectFramework(const std::filesystem::path& project,
    const std::filesystem::path& framework, std::string_view expectedConfigurationSha256);
[[nodiscard]] Result<FrameworkLocation> ResolveProjectFramework(const std::filesystem::path& project,
    const FrameworkLocation& explicitLocation = {}, const FrameworkLocation& applicationDefault = {});
[[nodiscard]] Status VerifyProjectConfiguration(const std::filesystem::path& project, const ProjectConfiguration& expected);
} // namespace War3Map::HLua
