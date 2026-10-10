#pragma once

#include "AssetOwnedPayload.h"

#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;
class QTreeWidget;
namespace War3Map::UiKit { class CodeView; class CodeProblemsView; }

namespace War3Map::Modules {

// 导入的 Lua 与资源页共用同一份 AssetState；这里只拥有当前源码草稿和视图状态。
class TriggerLuaScripts final : public QWidget {
    Q_OBJECT
public:
    explicit TriggerLuaScripts(QtProject::EditorProject& project, QWidget* parent = nullptr);
    [[nodiscard]] UiKit::CodeView* Editor() const noexcept { return editor_; }
    [[nodiscard]] QTreeWidget* Directory() const noexcept { return directory_; }
    [[nodiscard]] QString SelectedPath() const { return selectedPath_; }
    [[nodiscard]] QString StatusText() const;
    [[nodiscard]] QStringList ScriptPaths() const;
    [[nodiscard]] bool HasPendingDraft() const;
    bool CommitPendingDraft();
    void DiscardPendingDraft();
    bool SelectScript(const QString& path);
    bool CreateScript(const QString& path, const QString& initialText = QStringLiteral("-- Lua 脚本草稿\n"));
    bool RenameScript(const QString& path, const QString& newPath);
    bool DeleteScript(const QString& path);
    void RefreshDirectory();

signals:
    void DraftEdited();
    void RevealAssetRequested(const QString& path);

private:
    friend class TriggerModule;
    // 导航先捕获共同只读来源，再消费同一自持有载荷；不把来源正文放进书签。
    bool SelectAcceptedScript(QtProject::OwnedAssetSource source);
    void LoadAcceptedScript(QtProject::OwnedAssetSource source);
    bool LoadScript(const QString& path);
    void UpdateDraftState();
    void Status(const QString& text);
    QtProject::EditorProject& project_;
    QTreeWidget* directory_ = nullptr;
    UiKit::CodeView* editor_ = nullptr;
    UiKit::CodeProblemsView* problems_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* sourceNote_ = nullptr;
    QPushButton* apply_ = nullptr;
    QPushButton* discard_ = nullptr;
    QString selectedPath_, baselineText_;
    std::optional<QtProject::OwnedAssetSource> source_;
    bool suppress_ = false, publishing_ = false, editableSource_ = false;
};

} // namespace War3Map::Modules
