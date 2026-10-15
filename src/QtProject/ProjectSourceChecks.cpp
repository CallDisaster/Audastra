#include "EditorProject.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

namespace War3Map::QtProject {

Status EditorProject::CheckProjectSources() {
    Status result;
    const auto reject = [&](const std::string& code, const std::string& message) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "来源", code, message, std::nullopt});
    };
    if (archiveSession_) {
        std::error_code error;
        if (sourceArchivePath_.empty()
            || !std::filesystem::equivalent(sourceArchivePath_, archiveSession_->SourcePath(), error)
            || error) {
            reject("SOURCE_IDENTITY_CHANGED", "源地图路径与当前快照不一致或已不可访问");
        } else {
            const auto check = archiveSession_->VerifySourceUnchanged();
            if (!check.Succeeded()) {
                for (const auto& diagnostic : check.diagnostics)
                    result.diagnostics.push_back(diagnostic);
            }
        }
    } else if (readOnlyArchive_) {
        std::error_code error;
        if (sourceArchivePath_.empty() || !std::filesystem::equivalent(sourceArchivePath_, readOnlyArchive_->SourcePath(), error) || error)
            reject("SOURCE_IDENTITY_CHANGED", "源地图路径与当前只读绑定不一致或已不可访问");
        else {
            const auto check = readOnlyArchive_->VerifyUnchanged();
            result.diagnostics.insert(result.diagnostics.end(), check.diagnostics.begin(), check.diagnostics.end());
        }
    } else if (!sourceArchivePath_.empty()) {
        reject("SOURCE_SNAPSHOT_UNAVAILABLE", "当前地图缺少完整来源快照，不能确认来源未变");
    } else if (directorySnapshot_) {
        std::error_code error;
        if (projectRoot_.empty()
            || !std::filesystem::equivalent(projectRoot_, directorySnapshot_->Snapshot().root, error)
            || error) {
            reject("SOURCE_IDENTITY_CHANGED", "源工作区路径与当前快照不一致或已不可访问");
        } else {
            const auto check = directorySnapshot_->VerifyUnchanged();
            if (!check.Succeeded()) {
                for (const auto& diagnostic : check.diagnostics)
                    result.diagnostics.push_back(diagnostic);
            }
        }
    } else if (!projectRoot_.empty()) {
        reject("SOURCE_SNAPSHOT_UNAVAILABLE", "当前工作区缺少来源快照");
    }
    if (!projectFilePath_.empty()) {
        const auto sha = War3Map::Sha256File(projectFilePath_);
        if (!sha.Succeeded() || projectFileSha256_.empty() || *sha.value != projectFileSha256_)
            reject("PROJECT_FILE_CHANGED", "工程文件已被其他程序修改、移走或无法读取");
    }
    const bool valid = result.Succeeded();
    sourceChangeSuspected_ = !valid;
    sourceChangeConfirmed_ = !valid;
    projectNoticeDismissed_ = false;
    // 文件替换可能撤销原文件 watch；显式检查后重建观察列表。
    RefreshProjectWatch();
    return result;
}

} // namespace War3Map::QtProject
