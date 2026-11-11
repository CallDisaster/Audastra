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
    std::string providerId;
    std::vector<std::string> candidateNatives;
};

enum class ExternalDependencyKind { Declared, Ydwe, KkweDz };

struct ExternalDependencyProvider {
    std::string id;
    std::filesystem::path manifest;
    std::vector<ExternalDependencyFile> files;
    std::vector<Diagnostic> diagnostics;
    bool usable = false;
    ExternalDependencyKind kind = ExternalDependencyKind::Declared;
    std::filesystem::path root;
    bool ydweTree = false;
};

struct ExternalDependenciesStatus {
    std::filesystem::path directory;
    bool exists = false;
    std::vector<ExternalDependencyProvider> providers;
    std::vector<std::filesystem::path> undescribedDlls;
    std::vector<std::filesystem::path> unrecognizedDirectories;
    std::size_t undescribedDllCount = 0;
};

using ExternalNativeRequirements = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] std::filesystem::path ExternalDependenciesDirectory(
    const std::filesystem::path& installation, const std::filesystem::path& configured);
// 命令行地图测试和 h-lua 共用原生设置；显式目录优先。
[[nodiscard]] std::filesystem::path ResolveCliExternalDependenciesDirectory(
    const std::filesystem::path& explicitDirectory = {});
[[nodiscard]] Status ValidateExternalDependenciesDirectory(const std::filesystem::path& directory,
    const std::filesystem::path& testWorkspaceRoot = {});
[[nodiscard]] Status EnsureExternalDependenciesDirectory(const std::filesystem::path& directory);
[[nodiscard]] Result<ExternalDependenciesStatus> QueryExternalDependencies(const std::filesystem::path& directory);
// 空签名表示只能从脚本取得名字。每个完整提供者按声明顺序载入。
[[nodiscard]] Result<std::vector<ExternalDependencyFile>> SelectExternalDependencyFiles(
    const ExternalDependenciesStatus& status, const ExternalNativeRequirements& requirements);
[[nodiscard]] std::string ExternalDependenciesJson(const ExternalDependenciesStatus& status);

} // namespace War3Map::Services
