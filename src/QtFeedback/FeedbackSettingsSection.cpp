#include "FeedbackDialog.h"
#include "FeedbackUi.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace War3Map::QtFeedback {

class FeedbackSettingsSection::Impl {
public:
    FeedbackClient& client;
    QCheckBox* consent = nullptr;
    QLineEdit* endpoint = nullptr;
    QLabel* destination = nullptr;
    QLabel* lastSent = nullptr;

    Impl(FeedbackSettingsSection* q, FeedbackClient& service) : client(service) {
        q->setObjectName(QStringLiteral("feedbackSettingsSection"));
        auto* layout = new QVBoxLayout(q);
        layout->setContentsMargins(0, 8, 0, 8);
        layout->setSpacing(10);
        layout->addWidget(Detail::Heading(QStringLiteral("体验反馈")));
        consent = new QCheckBox(QStringLiteral("同意帮助改善应用体验"));
        consent->setObjectName(QStringLiteral("feedbackConsent"));
        consent->setChecked(client.Preferences().consent);
        layout->addWidget(consent);
        layout->addWidget(Detail::PlainLabel(QStringLiteral("上报应用版本、系统/显卡和清理后的问题诊断；你选择的日志、对话片段和截图也会包含。地图和触发器正文不会自动附带。发送前可查看全部内容。"), true));
        layout->addWidget(Detail::PlainLabel(QStringLiteral("可随时关闭，关闭后清空尚未发送的队列。已经发送的内容无法撤回。"), true));
        auto* form = new QFormLayout;
        endpoint = new QLineEdit(client.Preferences().endpointOverride);
        endpoint->setObjectName(QStringLiteral("feedbackEndpointOverride"));
        endpoint->setMaxLength(2048);
        endpoint->setPlaceholderText(QStringLiteral("留空使用发布配置；默认没有上报地址"));
        form->addRow(QStringLiteral("上报端点"), endpoint);
        layout->addLayout(form);
        destination = Detail::PlainLabel({}, true);
        destination->setObjectName(QStringLiteral("feedbackSettingsDestination"));
        layout->addWidget(destination);
        lastSent = Detail::PlainLabel({}, true);
        lastSent->setObjectName(QStringLiteral("feedbackLastSent"));
        layout->addWidget(lastSent);
        QObject::connect(endpoint, &QLineEdit::textChanged, q, [this] { Update(); });
        QObject::connect(&client, &FeedbackClient::LastSentChanged, q, [this] { Update(); });
        Update();
    }

    void Update() {
        const auto target = endpoint->text().isEmpty() ? client.PublishedEndpoint() : endpoint->text();
        auto proposed = client.Preferences();
        proposed.endpointOverride = endpoint->text();
        if (!FeedbackClient::ValidatePreferences(proposed)) {
            destination->setText(QStringLiteral("上报地址尚未通过校验，请检查输入。"));
            return;
        }
        destination->setText(target.isEmpty() ? QStringLiteral("上报到：尚未配置。报告仅保留本地，可导出。") : QStringLiteral("上报到：%1").arg(target));
        const auto time = client.Preferences().lastSentUnixSeconds;
        lastSent->setText(time == 0 ? QStringLiteral("最近上报：尚未发送") : QStringLiteral("最近上报：%1")
            .arg(QDateTime::fromSecsSinceEpoch(time).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    }
};

FeedbackSettingsSection::FeedbackSettingsSection(FeedbackClient& client, QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>(this, client)) {}
FeedbackSettingsSection::~FeedbackSettingsSection() = default;
FeedbackPreferences FeedbackSettingsSection::Preferences() const {
    auto preferences = impl_->client.Preferences();
    preferences.consent = impl_->consent->isChecked();
    preferences.endpointOverride = impl_->endpoint->text();
    return preferences;
}
bool FeedbackSettingsSection::Validate(QString* error) const { return FeedbackClient::ValidatePreferences(Preferences(), error); }

} // namespace War3Map::QtFeedback
