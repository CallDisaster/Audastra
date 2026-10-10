#include "FeedbackDialog.h"
#include "FeedbackUi.h"
#include "../feedback/Fixtures.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>
#include <QtTest>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace War3Map::QtFeedback;
using namespace FeedbackFixtures;

namespace {

bool Capture(QWidget& widget, const QString& name) {
    const auto output = qEnvironmentVariable("FORGE_QT_FEEDBACK_CAPTURE_DIR");
    if (output.isEmpty()) return true;
    const QDir directory(output);
    if (!directory.isAbsolute() || !directory.exists()) return false;
    const auto path = directory.filePath(name + QStringLiteral(".png"));
    if (QFileInfo::exists(path)) return false;
    const auto pixmap = widget.grab();
    if (!pixmap.save(path)) return false;
    const auto* screen = widget.screen();
    const QJsonObject metadata{
        {QStringLiteral("schema"), QStringLiteral("audastra-qt-widget-capture-v1")},
        {QStringLiteral("captureMethod"), QStringLiteral("QWidget::grab")},
        {QStringLiteral("image"), name + QStringLiteral(".png")},
        {QStringLiteral("qpaPlatform"), QGuiApplication::platformName()},
        {QStringLiteral("widgetLogicalWidth"), widget.width()},
        {QStringLiteral("widgetLogicalHeight"), widget.height()},
        {QStringLiteral("pixmapPixelWidth"), pixmap.width()},
        {QStringLiteral("pixmapPixelHeight"), pixmap.height()},
        {QStringLiteral("devicePixelRatio"), pixmap.devicePixelRatio()},
        {QStringLiteral("widgetDevicePixelRatio"), widget.devicePixelRatioF()},
        {QStringLiteral("screenDevicePixelRatio"), screen ? QJsonValue(screen->devicePixelRatio()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("fontFamily"), widget.font().family()},
        {QStringLiteral("resolvedFontFamily"), QFontInfo(widget.font()).family()},
        {QStringLiteral("locale"), QLocale::system().name()},
        {QStringLiteral("widgetLocale"), widget.locale().name()},
        {QStringLiteral("qtTranslatorLoaded"), qApp->property("feedbackCaptureQtTranslatorLoaded").toBool()}
    };
    QFile metadataFile(directory.filePath(name + QStringLiteral(".metadata.json")));
    if (!metadataFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    if (metadataFile.write(bytes) != bytes.size() || !metadataFile.flush()) return false;
    qInfo().noquote() << "Feedback Qt original-fixture capture:" << path << "metadata:" << metadataFile.fileName();
    return true;
}

QString WithoutDisplayWhitespace(const QString& display) {
    // 独立匹配完整字符串token，只去掉其外的JSON空白，以验收展示未丢失或改写原正文。
    const QRegularExpression tokens(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|[ \\t\\r\\n]+"));
    auto matches = tokens.globalMatch(display);
    QString compact;
    qsizetype end = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        compact += display.mid(end, match.capturedStart() - end);
        if (match.captured().startsWith(QLatin1Char('"'))) compact += match.captured();
        end = match.capturedEnd();
    }
    compact += display.mid(end);
    return compact;
}

} // namespace

class FeedbackDialogTests final : public QObject {
    Q_OBJECT
private:
    QTranslator qtTranslator_;
private slots:
    void initTestCase();
    void init();
    void SettingsStagesConsentAndCancelDoesNotApplyIt();
    void FormDefaultsDoNotCollectOptionalContentAndCanExportWithoutConsent();
    void ExplicitLogsAndConversationShowTheirWholeCleanedContent();
    void ScreenshotCanCancelAndBindsFinalPngBeforePreviewConfirmation();
    void QueueCanReviewAndDeleteAgentDraftThroughOneService();
    void ClearedSentAndDeletedReportsExplainTheirDifferentStates();
    void ModifiedPreviewAndDestinationDisableSend();
    void ReadablePreviewPreservesEveryOriginalJsonToken();
    void ScreenshotDisplayFitsWithoutUpscalingAndUsesPhysicalPixels();
};

void FeedbackDialogTests::initTestCase() {
    // 镜像主程序的系统Qt翻译加载；成员在整个测试期间保有translator生命周期。
    bool loaded = false;
    for (const QString& directory : {
             QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
             QLibraryInfo::path(QLibraryInfo::TranslationsPath)}) {
        if (qtTranslator_.load(QLocale::system(), QStringLiteral("qt"), QStringLiteral("_"), directory)) {
            loaded = qApp->installTranslator(&qtTranslator_);
            break;
        }
    }
    qApp->setProperty("feedbackCaptureQtTranslatorLoaded", loaded);
}

void FeedbackDialogTests::init() {
    UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    const auto fonts = UiKit::LoadFonts(UiKit::DefaultFontDirectory());
    UiKit::ApplyApplicationFont(fonts);
}

void FeedbackDialogTests::SettingsStagesConsentAndCancelDoesNotApplyIt() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "settings original queue");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    {
        FeedbackSettingsSection section(*client); section.resize(660, 330); section.show();
        auto* consent = section.findChild<QCheckBox*>(QStringLiteral("feedbackConsent"));
        auto* endpoint = section.findChild<QLineEdit*>(QStringLiteral("feedbackEndpointOverride"));
        QVERIFY(consent && endpoint && !consent->isChecked());
        QVERIFY(!client->Preferences().consent);
        QVERIFY(Capture(section, QStringLiteral("feedback-consent-default-graphite")));
        consent->setChecked(true);
        endpoint->setText(QStringLiteral("https://example.invalid/feedback"));
        QVERIFY(section.Validate());
        QVERIFY(section.Preferences().consent);
        QVERIFY(!queue->Consent() && !client->Preferences().consent);
        endpoint->setText(QStringLiteral("https://example.invalid/feedback?token=original-private"));
        QString error; QVERIFY(!section.Validate(&error));
        QVERIFY(!error.contains(QStringLiteral("original-private")));
        QVERIFY(!section.findChild<QLabel*>(QStringLiteral("feedbackSettingsDestination"))->text().contains(QStringLiteral("original-private")));
    }
    QVERIFY(!client->Preferences().consent && !queue->Consent());
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::FormDefaultsDoNotCollectOptionalContentAndCanExportWithoutConsent() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "default form original queue");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    int logReads = 0, conversationReads = 0;
    FeedbackFormContext context;
    context.application = AgentDraft().application;
    context.recentLogs = [&] { ++logReads; return "original-log-not-selected"; };
    context.conversationSnippet = [&] { ++conversationReads; return "original-conversation-not-selected"; };
    {
        FeedbackDialog form(*client, context); form.show();
        auto* description = form.findChild<QPlainTextEdit*>(QStringLiteral("feedbackDescription"));
        auto* logs = form.findChild<QCheckBox*>(QStringLiteral("feedbackIncludeLogs"));
        auto* conversation = form.findChild<QCheckBox*>(QStringLiteral("feedbackIncludeConversation"));
        auto* create = form.findChild<QPushButton*>(QStringLiteral("feedbackCreatePreview"));
        QVERIFY(description && logs && conversation && create);
        QVERIFY(!logs->isChecked() && !conversation->isChecked());
        QVERIFY(!create->isEnabled());
        QCOMPARE(logReads, 0); QCOMPARE(conversationReads, 0);
        description->setPlainText(QStringLiteral("原创建议：希望能够自定义编辑快捷键。"));
        QVERIFY(Capture(form, QStringLiteral("feedback-form-graphite")));
        QSignalSpy drafted(&form, &FeedbackDialog::DraftCreated);
        QTest::mouseClick(create, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(drafted.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(form.findChild<FeedbackPreviewDialog*>() != nullptr, 3000);
        auto* previewWindow = form.findChild<FeedbackPreviewDialog*>();
        QVERIFY(previewWindow->CompletePreviewAvailable());
        QVERIFY(!previewWindow->findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"))->isEnabled());
        QVERIFY(previewWindow->findChild<QPushButton*>(QStringLiteral("feedbackPreviewExport"))->isEnabled());
        QCOMPARE(logReads, 0); QCOMPARE(conversationReads, 0);
        const auto id = drafted.front()[0].toString();
        const auto preview = Take(queue->GetPreview(id.toStdString()), "default form complete payload");
        QVERIFY(!preview.delivery.cleanedLogs && preview.delivery.userReviewedScreenshotPng.empty());
        QVERIFY(preview.delivery.reportJson.find("original-conversation-not-selected") == std::string::npos);
        QVERIFY(Capture(*previewWindow, QStringLiteral("feedback-full-preview-graphite")));
        QSignalSpy completed(client.get(), &FeedbackClient::OperationFinished);
        const auto request = client->ExportReport(id, QString::fromStdWString((temp.Path() / L"report.zip").wstring()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QString::fromStdWString((temp.Path() / L"report.zip").wstring())), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!client->IsBusy(), 3000);
        bool exported = false;
        for (const auto& result : completed) if (result[0].toULongLong() == request) exported = result[1].toBool();
        QVERIFY(exported && !queue->Consent());
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ExplicitLogsAndConversationShowTheirWholeCleanedContent() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "selected optional content queue");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    int logReads = 0, conversationReads = 0;
    FeedbackFormContext context;
    context.application = AgentDraft().application;
    context.recentLogs = [&] { ++logReads; return "original log C:\\Users\\Synthetic User\\private.txt apiKey=\"selected-log-key\""; };
    context.conversationSnippet = [&] { ++conversationReads; return "原创明确选中的对话片段\n完整末行。"; };
    {
        FeedbackDialog form(*client, context); form.show();
        form.findChild<QPlainTextEdit*>(QStringLiteral("feedbackDescription"))->setPlainText(QStringLiteral("原创问题：操作按钮偶尔没有响应。"));
        form.findChild<QCheckBox*>(QStringLiteral("feedbackIncludeLogs"))->setChecked(true);
        form.findChild<QCheckBox*>(QStringLiteral("feedbackIncludeConversation"))->setChecked(true);
        QCOMPARE(logReads, 0); QCOMPARE(conversationReads, 0);
        QSignalSpy drafted(&form, &FeedbackDialog::DraftCreated);
        QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("feedbackCreatePreview")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(drafted.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(form.findChild<FeedbackPreviewDialog*>() != nullptr, 3000);
        QCOMPARE(logReads, 1); QCOMPARE(conversationReads, 1);
        auto* preview = form.findChild<FeedbackPreviewDialog*>();
        auto* logView = preview->findChild<QPlainTextEdit*>(QStringLiteral("feedbackPreviewLogs"));
        auto* conversationView = preview->findChild<QPlainTextEdit*>(QStringLiteral("feedbackPreviewConversation"));
        QVERIFY(logView && conversationView);
        QVERIFY(!logView->toPlainText().contains(QStringLiteral("Synthetic User")));
        QVERIFY(!logView->toPlainText().contains(QStringLiteral("selected-log-key")));
        QCOMPARE(conversationView->toPlainText(), QStringLiteral("原创明确选中的对话片段\n完整末行。"));
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ScreenshotCanCancelAndBindsFinalPngBeforePreviewConfirmation() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "screenshot review original queue");
    FeedbackPreferences preferences; preferences.consent = true;
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{QStringLiteral("https://example.invalid/feedback")}, preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    QWidget canvas; canvas.resize(440, 160);
    auto* canvasLayout = new QVBoxLayout(&canvas);
    canvasLayout->addWidget(new QLabel(QStringLiteral("原创截图窗口 · 快捷键设置")));
    canvas.show();
    FeedbackFormContext context; context.application = AgentDraft().application; context.screenshotTarget = &canvas;
    {
        FeedbackDialog form(*client, context); form.show();
        auto* capture = form.findChild<QPushButton*>(QStringLiteral("feedbackCaptureScreenshot"));
        auto* remove = form.findChild<QPushButton*>(QStringLiteral("feedbackRemoveScreenshot"));
        bool cancelled = false;
        QTimer::singleShot(0, &form, [&] {
            auto* review = form.findChild<QDialog*>(QStringLiteral("feedbackScreenshotReview"));
            if (review) { cancelled = true; review->reject(); }
        });
        QTest::mouseClick(capture, Qt::LeftButton);
        QVERIFY(cancelled && !remove->isEnabled());
        QString reviewedSha;
        bool captured = false;
        bool reviewScaleInvariant = false;
        QTimer::singleShot(0, &form, [&] {
            auto* review = form.findChild<QDialog*>(QStringLiteral("feedbackScreenshotReview"));
            if (!review) return;
            reviewedSha = review->findChild<QLineEdit*>(QStringLiteral("feedbackScreenshotReviewedSha"))->text();
            auto* mode = review->findChild<QComboBox*>(QStringLiteral("feedbackReviewedScreenshotScaleMode"));
            if (mode) {
                mode->setCurrentIndex(1);
                reviewScaleInvariant = review->findChild<QLineEdit*>(QStringLiteral("feedbackScreenshotReviewedSha"))->text() == reviewedSha;
                mode->setCurrentIndex(0);
                reviewScaleInvariant = reviewScaleInvariant && review->findChild<QLineEdit*>(QStringLiteral("feedbackScreenshotReviewedSha"))->text() == reviewedSha;
            }
            captured = Capture(*review, QStringLiteral("feedback-screenshot-review-graphite"));
            review->accept();
        });
        QTest::mouseClick(capture, Qt::LeftButton);
        QVERIFY(captured && reviewedSha.size() == 64 && remove->isEnabled());
        QVERIFY(reviewScaleInvariant);
        form.findChild<QPlainTextEdit*>(QStringLiteral("feedbackDescription"))->setPlainText(QStringLiteral("原创带截图问题，请检查按钮状态。"));
        QSignalSpy drafted(&form, &FeedbackDialog::DraftCreated);
        QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("feedbackCreatePreview")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(drafted.count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(form.findChild<FeedbackPreviewDialog*>() != nullptr, 3000);
        const auto report = Take(queue->GetPreview(drafted.front()[0].toString().toStdString()), "screenshot complete report");
        const auto finalPng = Take(PrepareScreenshot(report.delivery.userReviewedScreenshotPng), "final preview PNG structure");
        QCOMPARE(QString::fromStdString(finalPng.sha256), reviewedSha);
        auto* preview = form.findChild<FeedbackPreviewDialog*>();
        auto* confirmation = preview->findChild<QCheckBox*>(QStringLiteral("feedbackConfirmPreview"));
        auto* send = preview->findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"));
        confirmation->setChecked(true);
        QVERIFY(!send->isEnabled());
        auto* tabs = preview->findChild<QTabWidget*>(QStringLiteral("feedbackPreviewTabs"));
        auto* screenshotPage = preview->findChild<QWidget*>(QStringLiteral("feedbackPreviewScreenshotTab"));
        tabs->setCurrentWidget(screenshotPage);
        QVERIFY(send->isEnabled());
        auto* mode = preview->findChild<QComboBox*>(QStringLiteral("feedbackPreviewScreenshotScaleMode"));
        auto* shownSha = preview->findChild<QLineEdit*>(QStringLiteral("feedbackPreviewScreenshotSha"));
        QVERIFY(mode && shownSha);
        mode->setCurrentIndex(1);
        QCOMPARE(shownSha->text(), reviewedSha);
        QVERIFY(send->isEnabled());
        mode->setCurrentIndex(0);
        QCOMPARE(shownSha->text(), reviewedSha);
        QVERIFY(send->isEnabled());
        const auto afterScale = Take(queue->GetPreview(drafted.front()[0].toString().toStdString()), "display mode does not change the report");
        QCOMPARE(QByteArray::fromStdString(afterScale.delivery.reportJson), QByteArray::fromStdString(report.delivery.reportJson));
        QVERIFY(afterScale.delivery.userReviewedScreenshotPng == report.delivery.userReviewedScreenshotPng);
        QVERIFY(Capture(*preview, QStringLiteral("feedback-preview-screenshot-graphite")));
        // 此用例只审核按钮与SHA，不向example.invalid发送任何请求。
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::QueueCanReviewAndDeleteAgentDraftThroughOneService() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "queue page original shared queue");
    const auto draft = Take(queue->Draft(AgentDraft(), 1000), "Agent local UI draft");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    {
        FeedbackQueuePage page(*client); page.resize(780, 430); page.show();
        auto* table = page.findChild<QTableWidget*>(QStringLiteral("feedbackQueueTable"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 3000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("助手"));
        QVERIFY(Capture(page, QStringLiteral("feedback-queue-graphite")));
        QTest::mouseClick(page.findChild<QPushButton*>(QStringLiteral("feedbackQueuePreview")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(page.findChild<FeedbackPreviewDialog*>() != nullptr, 3000);
        auto* preview = page.findChild<FeedbackPreviewDialog*>();
        QCOMPARE(WithoutDisplayWhitespace(preview->findChild<QPlainTextEdit*>(QStringLiteral("feedbackPreviewJson"))->toPlainText()),
            QString::fromStdString(Take(queue->GetPreview(draft.entry.id), "queue full source payload").delivery.reportJson));
        preview->close();
        QTest::mouseClick(page.findChild<QPushButton*>(QStringLiteral("feedbackQueueDelete")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(Take(queue->GetStatus(draft.entry.id), "queue deleted readback").state == QueueState::Deleted, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 4)->text(), QStringLiteral("用户删除"), 3000);
        QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("feedbackQueuePreview"))->isEnabled());
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ClearedSentAndDeletedReportsExplainTheirDifferentStates() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "cleared sent/deleted original queue");
    const auto sent = Take(queue->Draft(AgentDraft(), 1000), "sent summary fixture");
    const auto preview = Take(queue->GetPreview(sent.entry.id), "review sent fixture");
    QVERIFY(queue->ConfirmPreview(sent.entry.id, preview.delivery.previewSha256).Succeeded());
    QVERIFY(queue->SetConsent(true).Succeeded());
    FakeTransport transport;
    const auto outcome = Take(queue->TrySend(sent.entry.id, preview.delivery.previewSha256, 1001, transport), "fixture delivery");
    QCOMPARE(outcome.state, AttemptState::Sent);
    QVERIFY(queue->SetConsent(false).Succeeded());
    const auto deleted = Take(queue->Draft(UserDraft(), 1002), "deleted summary fixture");
    QVERIFY(queue->Delete(deleted.entry.id).Succeeded());
    (void)Take(queue->Draft(UserDraft(), 1003), "still readable pending fixture");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    {
        FeedbackQueuePage page(*client);
        page.resize(780, 430); page.show();
        auto* table = page.findChild<QTableWidget*>(QStringLiteral("feedbackQueueTable"));
        auto* content = page.findChild<QPushButton*>(QStringLiteral("feedbackQueuePreview"));
        auto* status = page.findChild<QLabel*>(QStringLiteral("feedbackQueueStatus"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 3, 3000);
        table->selectRow(2); // newest first，已发送条目为较早的第一条。
        QCOMPARE(table->item(2, 4)->text(), QStringLiteral("已发送"));
        QCOMPARE(status->text(), QStringLiteral("内容已在发送后清除，仅保留状态摘要。"));
        QVERIFY(!content->isEnabled());
        QCOMPARE(content->toolTip(), status->text());
        table->selectRow(1);
        QCOMPARE(table->item(1, 4)->text(), QStringLiteral("用户删除"));
        QCOMPARE(status->text(), QStringLiteral("内容已按用户删除操作清除，仅保留删除状态。"));
        QVERIFY(!content->isEnabled());
        table->selectRow(0);
        QVERIFY(content->isEnabled());
        QVERIFY(!status->text().contains(QStringLiteral("内容已")));
        QVERIFY(!queue->GetPreview(sent.entry.id).Succeeded() && !queue->GetPreview(deleted.entry.id).Succeeded());
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ModifiedPreviewAndDestinationDisableSend() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "preview binding original queue");
    const auto draft = Take(queue->Draft(UserDraft(), 1000), "preview binding draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "preview binding original content");
    FeedbackPreferences preferences; preferences.consent = true;
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{QStringLiteral("https://example.invalid/feedback")}, preferences);
    QTRY_VERIFY_WITH_TIMEOUT(client->Snapshot().consent, 3000);
    {
        auto altered = preview; altered.delivery.reportJson += " ";
        FeedbackPreviewDialog invalid(*client, altered, client->EffectiveEndpoint());
        QVERIFY(!invalid.CompletePreviewAvailable());
        invalid.findChild<QCheckBox*>(QStringLiteral("feedbackConfirmPreview"))->setChecked(true);
        QVERIFY(!invalid.findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"))->isEnabled());
        QVERIFY(!invalid.findChild<QPushButton*>(QStringLiteral("feedbackPreviewExport"))->isEnabled());
        FeedbackPreviewDialog valid(*client, preview, client->EffectiveEndpoint());
        QVERIFY(valid.CompletePreviewAvailable());
        valid.findChild<QCheckBox*>(QStringLiteral("feedbackConfirmPreview"))->setChecked(true);
        QVERIFY(valid.findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"))->isEnabled());
        auto changed = client->Preferences(); changed.endpointOverride = QStringLiteral("https://other.invalid/feedback");
        QVERIFY(client->ApplyCommittedPreferences(changed));
        QVERIFY(!valid.findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"))->isEnabled());
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ReadablePreviewPreservesEveryOriginalJsonToken() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "readable preview original queue");
    const auto draft = Take(queue->Draft(UserDraft(), 1000), "readable preview original draft");
    auto report = Take(queue->GetPreview(draft.entry.id), "readable preview original complete content");
    auto client = std::make_unique<FeedbackClient>(queue, FeedbackEndpointConfig{});
    {
        FeedbackPreviewDialog original(*client, report, {});
        auto* text = original.findChild<QPlainTextEdit*>(QStringLiteral("feedbackPreviewJson"));
        auto* tabs = original.findChild<QTabWidget*>(QStringLiteral("feedbackPreviewTabs"));
        QVERIFY(text && tabs && original.CompletePreviewAvailable());
        QCOMPARE(tabs->tabText(0), QStringLiteral("全部内容"));
        QVERIFY(text->toPlainText().contains(QStringLiteral("\n    \"")));
        QCOMPARE(WithoutDisplayWhitespace(text->toPlainText()).toUtf8(), QByteArray::fromStdString(report.delivery.reportJson));

        // 原创词法夹具保留指数、小数尾零、负零、UTF-8、转义及字符串内的所有空白和标点。
        report.delivery.reportJson = R"json({"logs":null,"screenshot":null,"decimal":1.2300e+04,"negative":-0,"text":"中文  { [ , : ] } and \"quote\" \\ tail","nested":[true,{"value":2.0}],"empty":{}})json";
        report.delivery.previewSha256 = Take(Sha256Text(report.delivery.reportJson), "original lexical preview SHA");
        FeedbackPreviewDialog lexical(*client, report, {});
        QVERIFY(lexical.CompletePreviewAvailable());
        auto* rendered = lexical.findChild<QPlainTextEdit*>(QStringLiteral("feedbackPreviewJson"));
        QVERIFY(rendered);
        QCOMPARE(WithoutDisplayWhitespace(rendered->toPlainText()).toUtf8(), QByteArray::fromStdString(report.delivery.reportJson));
        QVERIFY(rendered->toPlainText().contains(QStringLiteral("\"decimal\": 1.2300e+04")));
        QVERIFY(rendered->toPlainText().contains(QStringLiteral("\"negative\": -0")));
        QVERIFY(rendered->toPlainText().contains(QStringLiteral("中文  { [ , : ] }")));
        QVERIFY(!lexical.findChild<QPushButton*>(QStringLiteral("feedbackPreviewSend"))->isEnabled());
    }
    client.reset(); QTRY_COMPARE_WITH_TIMEOUT(queue.use_count(), long{1}, 3000);
}

void FeedbackDialogTests::ScreenshotDisplayFitsWithoutUpscalingAndUsesPhysicalPixels() {
    // 独立原创像素oracle：每个原像素都有确定颜色，实际显示不能在DPR2下再放大一次。
    QImage pixels(16, 8, QImage::Format_ARGB32);
    for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
        pixels.setPixelColor(x, y, QColor(x * 15, y * 30, ((x + y) % 2) * 255));
    }
    War3Map::QtFeedback::Detail::ScreenshotView small(pixels, QStringLiteral("originalPixelOracle"));
    small.resize(240, 140); small.show();
    auto* label = small.findChild<QLabel*>(QStringLiteral("originalPixelOracle"));
    auto* mode = small.findChild<QComboBox*>(QStringLiteral("originalPixelOracleScaleMode"));
    QVERIFY(label && mode);
    QTRY_VERIFY(!label->pixmap().isNull());
    QCOMPARE(mode->currentIndex(), 0);
    QCOMPARE(label->pixmap().size(), QSize(16, 8)); // 适配不能放大小图。
    mode->setCurrentIndex(1);
    const qreal dpr = small.devicePixelRatioF();
    QCOMPARE(label->pixmap().size(), QSize(16, 8));
    QCOMPARE(label->pixmap().devicePixelRatio(), dpr);
    if (dpr == 1.0) QCOMPARE(label->size(), QSize(16, 8));
    if (dpr == 2.0) QCOMPARE(label->size(), QSize(8, 4));
    const auto shown = label->grab().toImage();
    QVERIFY(shown.width() >= 16 && shown.height() >= 8);
    for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
        QCOMPARE(shown.pixelColor(x, y), pixels.pixelColor(x, y));
    }
    QImage largePixels(1600, 800, QImage::Format_ARGB32); largePixels.fill(Qt::cyan);
    War3Map::QtFeedback::Detail::ScreenshotView large(largePixels, QStringLiteral("originalFitOracle"));
    auto* scroll = large.findChild<QScrollArea*>(QStringLiteral("originalFitOracleScroll"));
    auto* largeLabel = large.findChild<QLabel*>(QStringLiteral("originalFitOracle"));
    auto* largeMode = large.findChild<QComboBox*>(QStringLiteral("originalFitOracleScaleMode"));
    QVERIFY(scroll && largeLabel && largeMode);
    scroll->setFixedSize(400, 300); large.resize(420, 360); large.show();
    QTRY_COMPARE(scroll->viewport()->size(), QSize(400, 300));
    QTRY_VERIFY(largeLabel->width() <= 400 && largeLabel->height() <= 300);
    QVERIFY(!scroll->horizontalScrollBar()->isVisible() && !scroll->verticalScrollBar()->isVisible());
    if (dpr == 1.0) QCOMPARE(largeLabel->pixmap().size(), QSize(400, 200));
    if (dpr == 2.0) QCOMPARE(largeLabel->pixmap().size(), QSize(800, 400));
    largeMode->setCurrentIndex(1);
    QCOMPARE(largeLabel->pixmap().size(), QSize(1600, 800));
    if (dpr == 1.0) QCOMPARE(largeLabel->size(), QSize(1600, 800));
    if (dpr == 2.0) QCOMPARE(largeLabel->size(), QSize(800, 400));
    largeMode->setCurrentIndex(0);
    QTRY_VERIFY(largeLabel->width() <= 400 && largeLabel->height() <= 300);
    QVERIFY(!scroll->horizontalScrollBar()->isVisible() && !scroll->verticalScrollBar()->isVisible());
}

QTEST_MAIN(FeedbackDialogTests)
#include "FeedbackDialogTests.moc"
