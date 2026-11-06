#pragma once
#include "War3MapCore/Services/LuaFolder.h"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QJsonObject>

namespace War3Map::QtProject {
class EditorProject;
[[nodiscard]] QJsonObject LuaFolderSourceInfo(const EditorProject& project);

struct LuaFolderReview {
    Services::LuaFolderSnapshot folder;
    std::vector<Services::LuaFolderFile> map;
    Services::LuaFolderDifference difference;
    std::uint64_t generation = 0, revision = 0;
    bool emptyDirectory = false;
};

class ProjectLuaFolder final : public QObject {
public:
    explicit ProjectLuaFolder(EditorProject& project);
    void Refresh(bool force = true);
    [[nodiscard]] bool Linked() const { return !metadataSha_.empty() || !root_.empty(); }
    [[nodiscard]] bool Active() const { return snapshot_.has_value() && !snapshot_->missing; }
    [[nodiscard]] bool Missing() const { return snapshot_.has_value() && snapshot_->missing; }
    [[nodiscard]] const std::filesystem::path& Root() const { return root_; }
    [[nodiscard]] const std::optional<Services::LuaFolderSnapshot>& Snapshot() const { return snapshot_; }
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const { return diagnostics_; }
    [[nodiscard]] Result<LuaFolderReview> Review(const std::filesystem::path& root) const;
    [[nodiscard]] Status Associate(const LuaFolderReview& review, Services::LuaFolderChoice choice);
    [[nodiscard]] Status Unlink();
    [[nodiscard]] Result<std::pair<std::vector<std::string>, std::string>> Deletions() const;
private:
    EditorProject& project_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    std::filesystem::path root_, mapPath_;
    std::string metadataSha_;
    std::optional<Services::LuaFolderSnapshot> snapshot_;
    std::vector<Diagnostic> diagnostics_;
    bool refreshing_ = false;
};
} // namespace War3Map::QtProject
