#include "FeedbackDialog.h"
#include "FeedbackUi.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace War3Map::QtFeedback {

class FeedbackDialog::Impl {
public:
    FeedbackDialog* dialog;
    FeedbackClient& client;
    FeedbackFormContext context;
    QPointer<QWidget> screenshotTarget;
    QComboBox* kind = nullptr;
    QPlainTextEdit* description = nullptr;
    QCheckBox* includeLogs = nullptr;
    QCheckBox* includeConversation = nullptr;
    QPushButton* create = nullptr;
    QPushButton* removeScreenshot = nullptr;
    QLabel* screenshotStatus = nullptr;
    QLabel* status = nullptr;
    std::optional<Feedback::PreparedScreenshot> screenshot;
    quint64 draftRequest = 0;
    quint64 previewRequest = 0;

    Impl(FeedbackDialog* q, FeedbackClient& service, FeedbackFormContext input)
        : dialog(q), client(service), context(std::move(input)), screenshotTarget(context.screenshotTarget) {
        dialog->setObjectName(QStringLiteral("feedbackDialog"));
        dialog->setWindowTitle(QStringLiteral("问题与建议"));
        dialog->resize(660, 520);
        dialog->setMinimumSize(520, 420);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(24, 20, 24, 16);
        layout->setSpacing(12);
        layout->addWidget(Detail::Heading(QStringLiteral("问题与建议")));
        layout->addWidget(Detail::PlainLabel(QStringLiteral("先生成清理后的完整预览，再选择发送或导出。"), true));
        auto* form = new QFormLayout;
        kind = new QComboBox;
        kind->setObjectName(QStringLiteral("feedbackKind"));
        kind->addItem(QStringLiteral("问题"), static_cast<int>(Feedback::ReportKind::Problem));
        kind->addItem(QStringLiteral("建议"), static_cast<int>(Feedback::ReportKind::Suggestion));
        kind->setMaximumWidth(220);
        form->addRow(QStringLiteral("类型"), kind);
        layout->addLayout(form);
        description = new QPlainTextEdit;
        description->setObjectName(QStringLiteral("feedbackDescription"));
        description->setPlaceholderText(QStringLiteral("描述发生了什么，或希望改进的操作…"));
        layout->addWidget(description, 1);
        includeLogs = new QCheckBox(QStringLiteral("附带最近日志"));
        includeLogs->setObjectName(QStringLiteral("feedbackIncludeLogs"));
        includeLogs->setEnabled(static_cast<bool>(context.recentLogs));
        if (!context.recentLogs) includeLogs->setToolTip(QStringLiteral("当前窗口未提供可选日志。"));
        includeConversation = new QCheckBox(QStringLiteral("附带当前对话片段"));
        includeConversation->setObjectName(QStringLiteral("feedbackIncludeConversation"));
        includeConversation->setEnabled(static_cast<bool>(context.conversationSnippet));
        if (!context.conversationSnippet) includeConversation->setToolTip(QStringLiteral("当前窗口未提供可选对话片段。"));
        auto* options = new QHBoxLayout;
        options->addWidget(includeLogs);
        options->addWidget(includeConversation);
        options->addStretch();
        layout->addLayout(options);
        auto* screenshotRow = new QHBoxLayout;
        auto* capture = new QPushButton(QStringLiteral("截取当前窗口…"));
        capture->setObjectName(QStringLiteral("feedbackCaptureScreenshot"));
        capture->setEnabled(!screenshotTarget.isNull());
        removeScreenshot = new QPushButton(QStringLiteral("移除截图"));
        removeScreenshot->setObjectName(QStringLiteral("feedbackRemoveScreenshot"));
        removeScreenshot->setEnabled(false);
        screenshotRow->addWidget(capture);
        screenshotRow->addWidget(removeScreenshot);
        screenshotRow->addStretch();
        layout->addLayout(screenshotRow);
        screenshotStatus = Detail::PlainLabel(QStringLiteral("未附带截图。截图像素由你检查，不会自动脱敏。"), true);
        screenshotStatus->setObjectName(QStringLiteral("feedbackScreenshotStatus"));
        layout->addWidget(screenshotStatus);
        status = Detail::PlainLabel(client.Preferences().consent ? QStringLiteral("地图和触发器正文不会自动附带。发送前需确认完整预览。")
            : QStringLiteral("未同意上报；报告仅存本地，仍可查看并导出。"), true);
        status->setObjectName(QStringLiteral("feedbackFormStatus"));
        layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        create = buttons->addButton(QStringLiteral("生成预览"), QDialogButtonBox::ActionRole);
        create->setObjectName(QStringLiteral("feedbackCreatePreview"));
        create->setDefault(true);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(description, &QPlainTextEdit::textChanged, dialog, [this] { UpdateCreate(); });
        QObject::connect(capture, &QPushButton::clicked, dialog, [this] { Capture(); });
        QObject::connect(removeScreenshot, &QPushButton::clicked, dialog, [this] {
            screenshot.reset();
            removeScreenshot->setEnabled(false);
            screenshotStatus->setText(QStringLiteral("未附带截图。截图像素由你检查，不会自动脱敏。"));
        });
        QObject::connect(create, &QPushButton::clicked, dialog, [this] { Draft(); });
        QObject::connect(&client, &FeedbackClient::DraftReady, dialog,
            [this](quint64 request, const Feedback::DraftResult& draft) {
                if (request != draftRequest) return;
                emit dialog->DraftCreated(QString::fromStdString(draft.entry.id));
                previewRequest = client.RequestPreview(QString::fromStdString(draft.entry.id));
            });
        QObject::connect(&client, &FeedbackClient::PreviewReady, dialog,
            [this](quint64 request, const Feedback::Preview& preview, const QString& endpoint) {
                if (request != previewRequest) return;
                previewRequest = 0;
                auto* window = new FeedbackPreviewDialog(client, preview, endpoint, dialog);
                window->setAttribute(Qt::WA_DeleteOnClose);
                window->setWindowModality(Qt::WindowModal);
                window->show();
                UpdateCreate();
            });
        QObject::connect(&client, &FeedbackClient::OperationFinished, dialog,
            [this](quint64 request, bool, const QString& message) {
                if (request == 0) return;
                if (request == draftRequest) { draftRequest = 0; status->setText(message); }
                else if (request == previewRequest) { previewRequest = 0; status->setText(message); }
                else return;
                UpdateCreate();
            });
        UpdateCreate();
    }

    void UpdateCreate() {
        const auto text = description->toPlainText();
        const bool bounded = text.toUtf8().size() <= 16 * 1024;
        create->setEnabled(!text.trimmed().isEmpty() && bounded && draftRequest == 0 && previewRequest == 0);
        if (!bounded) status->setText(QStringLiteral("描述超过 16 KiB，请缩短后再生成预览。"));
    }

    void Draft() {
        Feedback::DraftInput draft;
        draft.source = Feedback::ReportSource::User;
        draft.kind = static_cast<Feedback::ReportKind>(kind->currentData().toInt());
        draft.application = context.application;
        draft.userDescription = description->toPlainText().toStdString();
        try {
            if (includeLogs->isChecked() && context.recentLogs) {
                draft.includeLogs = true;
                draft.recentLogs = context.recentLogs();
            }
            if (includeConversation->isChecked() && context.conversationSnippet) {
                draft.includeConversation = true;
                draft.conversationSnippet = context.conversationSnippet();
            }
        } catch (const std::exception&) {
            status->setText(QStringLiteral("无法取得所选附加内容，未保存或发送报告。"));
            return;
        }
        if (screenshot) {
            draft.includeScreenshot = true;
            draft.screenshot = {screenshot->png, screenshot->sha256};
        }
        draftRequest = client.SubmitDraft(std::move(draft));
        status->setText(QStringLiteral("正在生成本地预览…"));
        UpdateCreate();
    }

    void Capture() {
        if (screenshotTarget.isNull()) { status->setText(QStringLiteral("当前窗口已关闭，无法截图。")); return; }
        QByteArray bytes;
        QBuffer output(&bytes);
        output.open(QIODevice::WriteOnly);
        if (!screenshotTarget->grab().save(&output, "PNG")) { status->setText(QStringLiteral("当前窗口截图失败。")); return; }
        const auto canonical = Feedback::PrepareScreenshot(std::as_bytes(std::span(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
        if (!canonical.Succeeded()) { status->setText(QStringLiteral("截图未通过容量或 PNG 检查，未附带到反馈。")); return; }
        const auto& png = canonical.value->png;
        const QImage image = QImage::fromData(reinterpret_cast<const uchar*>(png.data()), static_cast<int>(png.size()), "PNG");
        if (image.isNull()) { status->setText(QStringLiteral("最终 PNG 无法完整预览，未附带到反馈。")); return; }
        QDialog review(dialog);
        review.setObjectName(QStringLiteral("feedbackScreenshotReview"));
        review.setWindowTitle(QStringLiteral("检查截图"));
        review.resize(760, 520);
        auto* layout = new QVBoxLayout(&review);
        layout->addWidget(Detail::PlainLabel(QStringLiteral("检查最终 PNG。附加信息已移除，像素中的路径、对话和密钥仍需你确认。")));
        layout->addWidget(new Detail::ScreenshotView(image, QStringLiteral("feedbackReviewedScreenshot")), 1);
        auto* hash = new QLineEdit(QString::fromStdString(canonical.value->sha256));
        hash->setObjectName(QStringLiteral("feedbackScreenshotReviewedSha"));
        hash->setReadOnly(true);
        layout->addWidget(hash);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("使用这张截图"));
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &review, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &review, &QDialog::reject);
        if (review.exec() != QDialog::Accepted) return;
        screenshot = *canonical.value;
        screenshotStatus->setText(QStringLiteral("已选择 %1 × %2 截图；发送前还可检查最终附件。").arg(image.width()).arg(image.height()));
        removeScreenshot->setEnabled(true);
    }
};

FeedbackDialog::FeedbackDialog(FeedbackClient& client, FeedbackFormContext context, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, client, std::move(context))) {}
FeedbackDialog::~FeedbackDialog() = default;

} // namespace War3Map::QtFeedback
