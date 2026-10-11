#pragma once

#include "Modules/EditorModule.h"
#include "War3MapCore/Services/CampaignService.h"

#include <QObject>
#include <QPointer>
#include <array>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QCheckBox;
class QTabWidget;
class QTableWidget;
class QTemporaryDir;
class QUndoStack;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {
class ObjectModule;
class AssetsModule;
class CampaignHistoryCommand;
class CampaignScopeHost;

class CampaignModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
    friend class CampaignHistoryCommand;
    friend class CampaignScopeHost;
public:
    explicit CampaignModule(QtProject::EditorProject& mainProject);
    ~CampaignModule() override;
    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    QUndoStack* UndoStack() override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    bool PrepareNavigationLeave() override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    [[nodiscard]] bool Open(const std::filesystem::path& path);
    [[nodiscard]] bool New();
    [[nodiscard]] bool Save(bool saveAs = false);
    [[nodiscard]] bool SaveTo(const std::filesystem::path& target, bool inPlace = false);
    [[nodiscard]] bool ConfirmClose(const QString& action, bool allowDiscard = true);
    [[nodiscard]] bool Dirty() const;
    [[nodiscard]] bool HasDraft() const;
    [[nodiscard]] bool FlushDraft();
    [[nodiscard]] std::shared_ptr<Services::CampaignSession> Session() const { return session_; }
    // MCP 同服务发布后刷新；有本地草稿时宿主必须先调用 FlushDraft，或拒绝外部写入。
    void RefreshFromService();
    void SetSession(std::shared_ptr<Services::CampaignSession> session);
    [[nodiscard]] QtProject::EditorProject& SharedProject();
    [[nodiscard]] bool ApplyCommand(const Services::CampaignCommand& command);
    [[nodiscard]] bool OpenChapter(StableId id);
    [[nodiscard]] bool UpdateOpenedChapter();
    // 地图保存宿主在确实写出章节副本后通知；保存战役前须显式更新章节。
    void NoteChapterSaved(const std::filesystem::path& path);
    [[nodiscard]] bool HasUncommittedChapter() const;
    [[nodiscard]] QString LastError() const { return lastError_; }
signals:
    void ChapterOpenRequested(const QString& path);
    void SessionChanged();
    void Changed();
private:
    void BuildPage();
    QWidget* BuildSettings();
    void RefreshTables();
    void RefreshSettings();
    void ReloadShared();
    void RebuildHistory();
    void Replay(bool redo);
    bool CommitShared();
    bool CommitSettings();
    void EditButton(bool create);
    void AddMap(bool replace);
    void RemoveSelection(bool menu);
    void MoveSelection(bool menu, int delta);
    void ExportSelection();
    void Notice(const QString& text);
    bool Failed(const std::vector<Diagnostic>& diagnostics);
    StableId SelectedId(bool menu) const;
    QtProject::EditorProject& mainProject_;
    std::unique_ptr<QtProject::EditorProject> shared_;
    std::unique_ptr<CampaignScopeHost> scopeHost_;
    std::unique_ptr<ObjectModule> objects_;
    std::unique_ptr<AssetsModule> assets_;
    std::shared_ptr<Services::CampaignSession> session_;
    Services::CampaignRevision displayedRevision_;
    std::unique_ptr<QTemporaryDir> workspace_;
    std::unique_ptr<QTemporaryDir> chapterFiles_;
    std::filesystem::path openedChapterFile_;
    StableId openedChapterId_ = 0;
    bool chapterSavedPending_ = false;
    std::string openedChapterSourceSha_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QPointer<QTabWidget> tabs_;
    QPointer<QTableWidget> maps_, buttons_;
    QPointer<QLabel> summary_;
    std::array<QPointer<QLineEdit>, 7> texts_{}; // 名称、难度、作者、背景、缩略图、环境声、雾色原字节。
    QPointer<QPlainTextEdit> description_;
    std::array<QPointer<QSpinBox>, 5> numbers_{}; // 背景、环境声、雾样式、光标、编辑器版本。
    std::array<QPointer<QLineEdit>, 3> fog_{};
    QPointer<QCheckBox> variableDifficulty_, expansion_;
    QUndoStack* history_ = nullptr;
    bool rebuildingHistory_ = false, refreshing_ = false, settingsDirty_ = false;
    QString lastError_;
};
} // namespace War3Map::Modules
