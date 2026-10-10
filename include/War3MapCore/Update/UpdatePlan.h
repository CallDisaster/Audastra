#pragma once

#include "War3MapCore/Update/UpdateManifest.h"

namespace War3Map::Update {

enum class UpdateFileAction { CreateProgramFile, ReplaceProgramFile, PreserveUserFile, PreserveObsoleteProgramFile };

struct UpdatePlanEntry {
    std::string path;
    UpdateFileAction action = UpdateFileAction::PreserveUserFile;
    std::uint64_t currentBytes = 0;
    std::string currentSha256;
    std::uint64_t targetBytes = 0;
    std::string targetSha256;
};

struct UpdateInstallPlan {
    std::filesystem::path installDirectory;
    std::string currentVersion;
    std::string targetVersion;
    std::string manifestSha256;
    std::string baselineManifestSha256;
    std::string inventorySha256;
    std::uint64_t preservedBytes = 0;
    std::uint64_t programBytes = 0;
    // 包含空目录，后续镜像构建必须保留。计划只返回相对路径。
    std::vector<std::string> directories;
    std::vector<UpdatePlanEntry> files;
};

// 仅盘点和规划，绝不创建目录或替换文件。baseline须是同一可信发布钥验证的旧清单。
// 没有可信baseline时拒绝覆盖既有同路径文件。用户文件冲突、reparse/ADS等均fail-closed。
// installer仍须重新检查inventory、包和所有目标路径；两次目录rename不是崩溃原子事务。
[[nodiscard]] Result<UpdateInstallPlan> BuildUpdateInstallPlan(
    const VerifiedUpdateManifest& target, const VerifiedUpdateManifest* baseline,
    const std::filesystem::path& installDirectory, std::string_view currentVersion);

} // namespace War3Map::Update
