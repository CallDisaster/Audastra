#pragma once

#include "War3MapCore/Types.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Services {

inline constexpr wchar_t kExternalDependenciesDirectory[] = L"外部依赖";
inline constexpr std::size_t kExternalDependencyModuleLimit = 8;
inline constexpr std::size_t kExternalDependencyNativeLimit = 256;

struct ExternalDependencyFile {
    std::filesystem::path path;
    std::string sha256;
    std::string entry;
    std::vector<std::string> imports;
    std::map<std::string, std::string, std::less<>> natives;
    bool peValid = false;
};

struct ExternalDependencyProvider {
    std::string id;
    std::filesystem::path manifest;
    std::vector<ExternalDependencyFile> files;
    std::vector<Diagnostic> diagnostics;
    bool usable = false;
};

struct ExternalDependenciesStatus {
    std::filesystem::path directory;
    bool exists = false;
    std::vector<ExternalDependencyProvider> providers;
    std::vector<std::filesystem::path> undescribedDlls;
};

using ExternalNativeRequirements = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] std::filesystem::path ExternalDependenciesDirectory(
    const std::filesystem::path& installation, const std::filesystem::path& configured);
[[nodiscard]] Status EnsureExternalDependenciesDirectory(const std::filesystem::path& directory);
[[nodiscard]] Result<ExternalDependenciesStatus> QueryExternalDependencies(const std::filesystem::path& directory);
// 空签名表示只能从脚本取得名字。每个完整提供者按声明顺序载入。
[[nodiscard]] Result<std::vector<ExternalDependencyFile>> SelectExternalDependencyFiles(
    const ExternalDependenciesStatus& status, const ExternalNativeRequirements& requirements);
[[nodiscard]] std::string ExternalDependenciesJson(const ExternalDependenciesStatus& status);

} // namespace War3Map::Services
