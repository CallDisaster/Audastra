#include "FeedbackDialog.h"
#include "FeedbackUi.h"
#include "War3MapCore/Hash.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QPixmap>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

#include <string_view>

namespace War3Map::QtFeedback {
namespace {

bool JsonWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::string ReviewJson(std::string_view original) {
    // 只调整字符串外的JSON空白。键顺序、数字字面量、转义和UTF-8 token逐字保留；发送仍用原正文。
    std::string display;
    display.reserve(original.size());
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    char previousToken = '\0';
    auto newLine = [&] { display.push_back('\n'); display.append(static_cast<std::size_t>(depth) * 4, ' '); };
    for (std::size_t index = 0; index < original.size(); ++index) {
        const char value = original[index];
        if (inString) {
            display.push_back(value);
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') { inString = false; previousToken = '"'; }
            continue;
        }
        if (JsonWhitespace(value)) continue;
        if (value == '"') { inString = true; display.push_back(value); }
        else if (value == '{' || value == '[') {
            // 外部传入异常深度时回退原文显示，仍展示全部字节，不生成二次膨胀的缩进。
            if (++depth > 64) return std::string(original);
            display.push_back(value);
            auto next = index + 1;
            while (next < original.size() && JsonWhitespace(original[next])) ++next;
            if (next == original.size() || original[next] != (value == '{' ? '}' : ']')) newLine();
        } else if (value == '}' || value == ']') {
            if (depth == 0) return std::string(original);
            --depth;
            if (previousToken != (value == '}' ? '{' : '[')) newLine();
            display.push_back(value);
        } else if (value == ',') { display.push_back(value); newLine(); }
        else if (value == ':') { display.push_back(value); display.push_back(' '); }
        else display.push_back(value);
        previousToken = value;
    }
    return display;
}

} // namespace

class FeedbackPreviewDialog::Impl {
public:
    FeedbackPreviewDialog* dialog;
    FeedbackClient& client;
    Feedback::Preview preview;
    QString endpoint;
    QTabWidget* tabs = nullptr;
    QCheckBox* confirmed = nullptr;
    QPushButton* send = nullptr;
    QPushButton* exportReport = nullptr;
    QLabel* status = nullptr;
    QLabel* destination = nullptr;
    bool complete = false;
    bool screenshotSeen = true;
    int screenshotIndex = -1;
    quint64 sendRequest = 0;
    quint64 exportRequest = 0;

    Impl(FeedbackPreviewDialog* q, FeedbackClient& service, Feedback::Preview value, QString target)
        : dialog(q), client(service), preview(std::move(value)), endpoint(std::move(target)) {
        dialog->setObjectName(QStringLiteral("feedbackPreviewDialog"));
        dialog->setWindowTitle(QStringLiteral("反馈完整预览"));
        dialog->resize(840, 640);
        dialog->setMinimumSize(580, 440);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(12);
        layout->addWidget(Detail::Heading(QStringLiteral("查看将发出的全部内容")));
        destination = Detail::PlainLabel(endpoint.isEmpty() ? QStringLiteral("上报通道尚未配置；可导出报告包。")
            : QStringLiteral("上报到：%1").arg(endpoint), true);
        destination->setObjectName(QStringLiteral("feedbackPreviewDestination"));
        layout->addWidget(destination);
        tabs = new QTabWidget;
        tabs->setObjectName(QStringLiteral("feedbackPreviewTabs"));
        tabs->addTab(Detail::PayloadText(QStringLiteral("feedbackPreviewJson"), QString::fromStdString(ReviewJson(preview.delivery.reportJson))),
            QStringLiteral("全部内容"));
        if (preview.delivery.cleanedLogs) tabs->addTab(
            Detail::PayloadText(QStringLiteral("feedbackPreviewLogs"), QString::fromStdString(*preview.delivery.cleanedLogs)),
            QStringLiteral("logs.txt · 已清理"));
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(preview.delivery.reportJson), &error);
        const auto json = document.object();
        if (json.value(QStringLiteral("conversationSnippet")).isString()) tabs->addTab(
            Detail::PayloadText(QStringLiteral("feedbackPreviewConversation"), json.value(QStringLiteral("conversationSnippet")).toString()),
            QStringLiteral("所选对话片段"));
        const auto hash = Sha256Text(preview.delivery.reportJson);
        complete = error.error == QJsonParseError::NoError && document.isObject() && hash.Succeeded()
            && *hash.value == preview.delivery.previewSha256;
        const auto jsonLogs = json.value(QStringLiteral("logs"));
        complete = complete && (preview.delivery.cleanedLogs
            ? jsonLogs.isString() && jsonLogs.toString().toStdString() == *preview.delivery.cleanedLogs : jsonLogs.isNull());
        const bool hasScreenshot = !preview.delivery.userReviewedScreenshotPng.empty();
        complete = complete && hasScreenshot == preview.requiresScreenshotPreview;
        if (hasScreenshot) {
            screenshotSeen = false;
            const auto& bytes = preview.delivery.userReviewedScreenshotPng;
            const auto canonical = Feedback::PrepareScreenshot(bytes);
            const auto screenshotJson = json.value(QStringLiteral("screenshot")).toObject();
            const QImage image = QImage::fromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()), "PNG");
            complete = complete && canonical.Succeeded() && canonical.value->png == bytes && !image.isNull()
                && screenshotJson.value(QStringLiteral("sha256")).toString().toStdString() == canonical.value->sha256
                && screenshotJson.value(QStringLiteral("bytes")).toInteger() == static_cast<qint64>(bytes.size());
            auto* page = new QWidget;
            page->setObjectName(QStringLiteral("feedbackPreviewScreenshotTab"));
            auto* screenshotLayout = new QVBoxLayout(page);
            screenshotLayout->setContentsMargins(8, 8, 8, 8);
            screenshotLayout->addWidget(new Detail::ScreenshotView(image, QStringLiteral("feedbackPreviewScreenshot")), 1);
            screenshotLayout->addWidget(Detail::PlainLabel(QStringLiteral("截图附加信息已移除；像素未自动脱敏，请检查可见的路径、对话和密钥。"), true));
            auto* sha = new QLineEdit(screenshotJson.value(QStringLiteral("sha256")).toString());
            sha->setObjectName(QStringLiteral("feedbackPreviewScreenshotSha"));
            sha->setReadOnly(true);
            screenshotLayout->addWidget(sha);
            screenshotIndex = tabs->addTab(page, QStringLiteral("screenshot.png · 请检查"));
        } else complete = complete && json.value(QStringLiteral("screenshot")).isNull();
        layout->addWidget(tabs, 1);
        confirmed = new QCheckBox(QStringLiteral("我已查看全部文本及所选附件，同意发送这份内容"));
        confirmed->setObjectName(QStringLiteral("feedbackConfirmPreview"));
        layout->addWidget(confirmed);
        status = Detail::PlainLabel(complete ? QStringLiteral("报告保存在本地。勾选确认后可发送，或直接导出报告包。")
            : QStringLiteral("完整内容校验失败，发送和导出已停用。"), true);
        status->setObjectName(QStringLiteral("feedbackPreviewStatus"));
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        send = buttons->addButton(QStringLiteral("确认并发送"), QDialogButtonBox::ActionRole);
        send->setObjectName(QStringLiteral("feedbackPreviewSend"));
        exportReport = buttons->addButton(QStringLiteral("导出报告包…"), QDialogButtonBox::ActionRole);
        exportReport->setObjectName(QStringLiteral("feedbackPreviewExport"));
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(confirmed, &QCheckBox::toggled, dialog, [this] { UpdateActions(); });
        QObject::connect(tabs, &QTabWidget::currentChanged, dialog, [this](int index) {
            if (index == screenshotIndex) screenshotSeen = true;
            UpdateActions();
        });
        QObject::connect(&client, &FeedbackClient::PreferencesChanged, dialog, [this] { UpdateActions(); });
        QObject::connect(&client, &FeedbackClient::SnapshotChanged, dialog, [this] { UpdateActions(); });
        QObject::connect(send, &QPushButton::clicked, dialog, [this] {
            sendRequest = client.ConfirmAndSend(QString::fromStdString(preview.delivery.reportId),
                QString::fromStdString(preview.delivery.previewSha256), endpoint);
            status->setText(QStringLiteral("正在上报…"));
            UpdateActions();
        });
        QObject::connect(exportReport, &QPushButton::clicked, dialog, [this] {
            const auto target = QFileDialog::getSaveFileName(dialog, QStringLiteral("导出反馈报告包"),
                QString::fromStdString(preview.delivery.reportId) + QStringLiteral(".zip"), QStringLiteral("ZIP 报告包 (*.zip)"));
            if (target.isEmpty()) return;
            if (QFileInfo::exists(target)) { status->setText(QStringLiteral("请选择新的文件名，已有文件不会被覆盖。")); return; }
            exportRequest = client.ExportReport(QString::fromStdString(preview.delivery.reportId), target);
            status->setText(QStringLiteral("正在导出报告包…"));
            UpdateActions();
        });
        QObject::connect(&client, &FeedbackClient::OperationFinished, dialog,
            [this](quint64 request, bool success, const QString& message) {
                if (request == 0) return;
                if (request == sendRequest) {
                    sendRequest = 0;
                    status->setText(message);
                    if (success) { complete = false; emit dialog->ReportSent(); }
                } else if (request == exportRequest) { exportRequest = 0; status->setText(message); }
                else return;
                UpdateActions();
            });
        UpdateActions();
    }

    void UpdateActions() {
        const bool sendingAllowed = client.Preferences().consent && client.Snapshot().consent
            && !endpoint.isEmpty() && client.EffectiveEndpoint() == endpoint;
        send->setEnabled(complete && confirmed->isChecked() && screenshotSeen && sendingAllowed && sendRequest == 0 && exportRequest == 0);
        exportReport->setEnabled(complete && sendRequest == 0 && exportRequest == 0);
        if (!sendingAllowed) send->setToolTip(QStringLiteral("需在设置中同意上报并配置当前通道。"));
        else if (!screenshotSeen) send->setToolTip(QStringLiteral("请打开截图页，检查最终 PNG 像素。"));
        else send->setToolTip({});
    }
};

FeedbackPreviewDialog::FeedbackPreviewDialog(FeedbackClient& client, Feedback::Preview preview, QString endpoint, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, client, std::move(preview), std::move(endpoint))) {}
FeedbackPreviewDialog::~FeedbackPreviewDialog() = default;
QString FeedbackPreviewDialog::PreviewSha256() const { return QString::fromStdString(impl_->preview.delivery.previewSha256); }
bool FeedbackPreviewDialog::CompletePreviewAvailable() const { return impl_->complete; }

} // namespace War3Map::QtFeedback
