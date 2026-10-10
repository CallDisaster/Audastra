#pragma once

#include "FeedbackClient.h"

#include <QDialog>
#include <QWidget>

#include <functional>
#include <memory>

namespace War3Map::QtFeedback {

struct FeedbackFormContext {
    Feedback::ApplicationInfo application;
    // 只有用户勾选对应项后才调用，不在表单构造时取内容。
    std::function<std::string()> recentLogs;
    std::function<std::string()> conversationSnippet;
    QWidget* screenshotTarget = nullptr;
};

class FeedbackPreviewDialog final : public QDialog {
    Q_OBJECT
public:
    FeedbackPreviewDialog(FeedbackClient& client, Feedback::Preview preview, QString endpoint,
        QWidget* parent = nullptr);
    ~FeedbackPreviewDialog() override;
    [[nodiscard]] QString PreviewSha256() const;
    [[nodiscard]] bool CompletePreviewAvailable() const;
signals:
    void ReportSent();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FeedbackDialog final : public QDialog {
    Q_OBJECT
public:
    FeedbackDialog(FeedbackClient& client, FeedbackFormContext context = {}, QWidget* parent = nullptr);
    ~FeedbackDialog() override;
signals:
    void DraftCreated(const QString& reportId);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 插入设置页。勾选/端点文本只暂存，宿主accept/commit后应用；取消不修改同意。
class FeedbackSettingsSection final : public QWidget {
    Q_OBJECT
public:
    explicit FeedbackSettingsSection(FeedbackClient& client, QWidget* parent = nullptr);
    ~FeedbackSettingsSection() override;
    [[nodiscard]] FeedbackPreferences Preferences() const;
    [[nodiscard]] bool Validate(QString* error = nullptr) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FeedbackQueuePage final : public QWidget {
    Q_OBJECT
public:
    explicit FeedbackQueuePage(FeedbackClient& client, QWidget* parent = nullptr);
    ~FeedbackQueuePage() override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::QtFeedback
