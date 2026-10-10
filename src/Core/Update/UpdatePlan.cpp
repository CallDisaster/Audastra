#include "War3MapCore/Update/UpdatePlan.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "UpdatePath.h"
#include "UpdateInstallerInternal.h"

#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Update {
namespace {

using PlanResult = Result<UpdateInstallPlan>;
PlanResult Failure(std::string code, std::string message, const std::filesystem::path& path) {
    auto result = PlanResult::Failure("Update", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}
std::wstring SignedPath(std::string_view text) { return {text.begin(), text.end()}; }

} // namespace

PlanResult BuildUpdateInstallPlan(const VerifiedUpdateManifest& target, const VerifiedUpdateManifest* baseline,
    const std::filesystem::path& installDirectory, std::string_view currentVersion) {
    try {
        auto check = CheckUpdate(target, currentVersion, UpdateChannel::Beta);
        if (!check.Succeeded()) return {std::nullopt, std::move(check.diagnostics)};
        if (!check.value->updateAvailable) return Failure("UPDATE_NOT_NEWER", "离线更新不允许安装相同或更低优先级的版本。", installDirectory);
        if (!check.value->directlyUpgradeable) return Failure("UPDATE_MINIMUM_VERSION", "当前版本低于签名清单的最低直接升级版本。", installDirectory);
        if (baseline && (baseline->TrustedKeySha256() != target.TrustedKeySha256()
            || baseline->Manifest().version != currentVersion)) return Failure("UPDATE_BASELINE_IDENTITY_MISMATCH",
                "旧程序清单必须对应当前精确版本并由相同可信发布公钥验证。", installDirectory);
        auto root = Detail::LockSafeDirectory(installDirectory);
        if (!root.Succeeded()) return {std::nullopt, std::move(root.diagnostics)};
        if (root.value->path == root.value->path.root_path()) return Failure("UPDATE_INSTALL_ROOT_REJECTED",
            "不能把整个磁盘根目录作为程序安装目录。", installDirectory);
        auto inspected = Detail::InspectUpdateTree(root.value->path);
        if (!inspected.Succeeded()) return {std::nullopt, std::move(inspected.diagnostics)};
        const auto& inventory = *inspected.value;
        std::map<std::wstring, const UpdateFileEntry*, Detail::WindowsPathLess> oldFiles, newFiles;
        if (baseline) for (const auto& file : baseline->Manifest().files) oldFiles.emplace(SignedPath(file.path), &file);
        for (const auto& file : target.Manifest().files) newFiles.emplace(SignedPath(file.path), &file);
        UpdateInstallPlan plan;
        plan.installDirectory = root.value->path;
        plan.currentVersion = currentVersion;
        plan.targetVersion = target.Manifest().version;
        plan.manifestSha256 = target.ManifestSha256();
        if (baseline) plan.baselineManifestSha256 = baseline->ManifestSha256();
        for (const auto& directory : inventory.directories) plan.directories.push_back(
            IO::GenericPathUtf8(std::filesystem::path(directory)));
        for (const auto& [path, file] : newFiles) {
            if (inventory.directories.contains(path)) return Failure("UPDATE_USER_PATH_CONFLICT",
                "新的程序文件路径已被现有目录占用，拒绝覆盖用户内容。", plan.installDirectory / path);
            for (auto end = path.find(L'/'); end != std::wstring::npos; end = path.find(L'/', end + 1)) {
                if (inventory.files.contains(path.substr(0, end))) return Failure("UPDATE_USER_PATH_CONFLICT",
                    "新的程序文件父目录被现有文件占用，拒绝覆盖用户内容。", plan.installDirectory / path);
            }
            const auto current = inventory.files.find(path);
            if (current != inventory.files.end()) {
                const auto owned = oldFiles.find(path);
                if (!baseline) return Failure("UPDATE_BASELINE_UNTRUSTED",
                    "缺少可信旧程序文件清单，不能授权覆盖已有同路径文件。", plan.installDirectory / path);
                if (owned == oldFiles.end()) return Failure("UPDATE_USER_FILE_CONFLICT",
                    "新程序文件与清单外的用户文件发生碰撞，拒绝覆盖。", plan.installDirectory / path);
                if (owned->second->bytes != current->second.bytes || owned->second->sha256 != current->second.sha256) return Failure(
                    "UPDATE_PROGRAM_FILE_MODIFIED", "既有程序文件已被修改，拒绝静默覆盖。", plan.installDirectory / path);
            }
            if (file->bytes > kMaxUpdateExpandedBytes - plan.programBytes) return Failure("UPDATE_PLAN_BUDGET",
                "计划中的程序容量超限。", installDirectory);
            plan.programBytes += file->bytes;
            plan.files.push_back(UpdatePlanEntry{file->path, current == inventory.files.end()
                ? UpdateFileAction::CreateProgramFile : UpdateFileAction::ReplaceProgramFile,
                current == inventory.files.end() ? 0 : current->second.bytes,
                current == inventory.files.end() ? std::string{} : current->second.sha256, file->bytes, file->sha256});
        }
        for (const auto& [path, file] : inventory.files) {
            if (newFiles.contains(path)) continue;
            const auto old = oldFiles.find(path);
            const bool obsoleteProgram = old != oldFiles.end() && old->second->bytes == file.bytes && old->second->sha256 == file.sha256;
            plan.preservedBytes += file.bytes;
            plan.files.push_back(UpdatePlanEntry{file.path, obsoleteProgram ? UpdateFileAction::PreserveObsoleteProgramFile
                : UpdateFileAction::PreserveUserFile, file.bytes, file.sha256, file.bytes, file.sha256});
        }
        std::sort(plan.files.begin(), plan.files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        plan.inventorySha256 = inventory.sha256;
        return PlanResult::Success(std::move(plan));
    } catch (const std::exception&) {
        return Failure("UPDATE_PLAN_FAILED", "无法完整生成只读安装计划，未授权任何文件替换。", installDirectory);
    }
}

} // namespace War3Map::Update
