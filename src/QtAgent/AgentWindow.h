#pragma once

#include "AgentTypes.h"

#include <QJsonObject>
#include <QWidget>

#include <memory>

namespace War3Map::Agent {

// 原生独立助手窗口。这里只呈现实际运行事件，不执行地图编辑或保存。
class AgentWindow final : public QWidget {
    Q_OBJECT

public:
    enum class PresentationMode { Page, Sidebar, Detached };
    Q_ENUM(PresentationMode)
    explicit AgentWindow(QWidget* parent = nullptr);
    ~AgentWindow() override;

    void SetSettings(const AgentSettings& settings);
    void SetActiveMap(QString title, QString identity);
    void SetMapSummary(QString summary);
    void AppendEvent(const QJsonObject& event);
    void SetBusy(bool busy);
    // 仅清除呈现；运行时会话与地图状态由 Controller 管理。
    void ClearConversation();
    void SetPresentationMode(PresentationMode mode);
    [[nodiscard]] PresentationMode Mode() const noexcept;

signals:
    void PromptSubmitted(QString text, QString providerId, QString modelId);
    void StopRequested();
    void SettingsRequested();
    void NewConversationRequested();
    void ConversationSwitchRequested(QString sessionId);
    void ConversationRenameRequested(QString sessionId, QString title);
    void ConversationArchiveRequested(QString sessionId);
    void VisibilityChanged(bool visible);
    void NavigateRequested(QString kind, QString objectId);
    // 开着“跟随修改”时，每批改动里最后一个能定位的对象；宿主只在编辑区可见时跳过去，不激活窗口。
    void FollowRequested(QString kind, QString objectId);
    // 头部“跟随修改”开关；由宿主保存设置后经 SetSettings 回写。
    void FollowChangesChangeRequested(bool follow);
    void PresentationModeRequested(War3Map::Agent::AgentWindow::PresentationMode mode);
    void BackRequested();
    // 输入框权限菜单选择的新档位；由宿主保存设置并重新应用工具策略。
    void PermissionsChangeRequested(bool allowEdits, bool allowSave, bool allowCommands);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace War3Map::Agent
