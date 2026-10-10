#include "Widgets/WarcraftTextPreview.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "PopupCapture.h"

#include <QApplication>
#include <QColorDialog>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <stdexcept>

using War3Map::UiKit::WarcraftTextPreview;

namespace {
QString captureDirectory;
QString PrepareCaptureDirectory() {
    const QString requested = qEnvironmentVariable("AUD_COLOUR_PREVIEW_CAPTURE_DIR");
    if (requested.isEmpty()) return {};
    if (!captureDirectory.isEmpty()) return requested == captureDirectory ? QString{} : QStringLiteral("捕获目录不能在测试内切换");
    const QFileInfo info(requested);
    if (!info.isAbsolute() || !info.isDir() || info.isSymLink()) return QStringLiteral("捕获需显式绝对、已有、非链接目录");
    QDirIterator entries(requested, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    if (entries.hasNext()) return QStringLiteral("保留旧捕获：输出目录必须为空");
    captureDirectory = requested;
    return {};
}
class PreviewCaptureHost {
public:
    PreviewCaptureHost(QPlainTextEdit& draft, WarcraftTextPreview& preview)
        : draft_(&draft), preview_(&preview), draftParent_(draft.parentWidget()), previewParent_(preview.parentWidget()),
          draftFlags_(draft.windowFlags()), previewFlags_(preview.windowFlags()), draftMaximumHeight_(draft.maximumHeight()) {
        host_.setObjectName(QStringLiteral("warcraftTextOriginalFixtureCapture"));
        host_.setWindowTitle(QStringLiteral("颜色码预览 · 原创组件夹具"));
        auto* layout = new QVBoxLayout(&host_);
        auto* label = new QLabel(QStringLiteral("当前草稿（原控件，未应用到地图）"), &host_);
        label->setTextFormat(Qt::PlainText); layout->addWidget(label);
        draft.setMaximumHeight(160); layout->addWidget(&draft); layout->addWidget(&preview, 1);
        host_.resize(1080, 680); host_.show(); preview.Refresh(); QApplication::processEvents();
    }
    ~PreviewCaptureHost() {
        if (draft_) { draft_->setParent(draftParent_, draftFlags_); draft_->setMaximumHeight(draftMaximumHeight_); }
        if (preview_) preview_->setParent(previewParent_, previewFlags_);
    }
    QString SaveDraft(const QString& stem) {
        const QString imagePath = QDir(captureDirectory).filePath(stem + QStringLiteral(".png"));
        QFile image(imagePath);
        if (!image.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return QStringLiteral("保留旧PNG或不能创建：%1").arg(imagePath);
        const auto pixels = host_.grab();
        if (pixels.isNull() || !pixels.save(&image, "PNG")) return QStringLiteral("实际组件PNG捕获失败");
        image.close();
        QJsonObject metadata{{QStringLiteral("fixture_only"), true}, {QStringLiteral("mainwindow_verified"), false},
            {QStringLiteral("capture_method"), QStringLiteral("actual_qwidget_grab_of_original_draft_and_component")},
            {QStringLiteral("platform"), QGuiApplication::platformName()},
            {QStringLiteral("logical_width"), host_.width()}, {QStringLiteral("logical_height"), host_.height()},
            {QStringLiteral("pixel_width"), pixels.width()}, {QStringLiteral("pixel_height"), pixels.height()},
            {QStringLiteral("dpr"), pixels.devicePixelRatio()}, {QStringLiteral("draft"), draft_->toPlainText()},
            {QStringLiteral("preview"), preview_->Preview()->toPlainText()}, {QStringLiteral("status"), preview_->StatusText()},
            {QStringLiteral("theme_from_unchanged_test_main"), QStringLiteral("graphite")},
            {QStringLiteral("font_family"), host_.font().family()}, {QStringLiteral("font_point_size"), host_.font().pointSizeF()}};
        QFile json(QDir(captureDirectory).filePath(stem + QStringLiteral(".json")));
        if (!json.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return QStringLiteral("保留旧JSON或不能创建");
        const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        return json.write(bytes) == bytes.size() ? QString{} : QStringLiteral("捕获回执写入失败");
    }
    QString SavePicker(QColorDialog& picker, QWidget* anchor) {
        return PopupCapture::Save(host_, picker, anchor, captureDirectory, QStringLiteral("colour-picker"));
    }
private:
    QWidget host_;
    QPointer<QPlainTextEdit> draft_;
    QPointer<WarcraftTextPreview> preview_;
    QWidget* draftParent_ = nullptr;
    QWidget* previewParent_ = nullptr;
    Qt::WindowFlags draftFlags_, previewFlags_;
    int draftMaximumHeight_ = QWIDGETSIZE_MAX;
};
} // namespace

class WarcraftTextPreviewTests final : public QObject {
    Q_OBJECT
private slots:
    void PlainTextStylesNeverInterpretHtmlOrLoadResources();
    void InsertsOnlyDraftWithOriginalUndoAndPickerCancel();
    void UnknownReferencesBudgetAndBadUnicodePreserveDraft();
    void BindingGenerationRejectsLatePickerAndOwnerDeletion();
    void ResolverDisplaysPreparedTextAndRejectsUnknownOrStale();
    void ResolverReentryAndDeletionCannotPublishOldResult();
};

void WarcraftTextPreviewTests::PlainTextStylesNeverInterpretHtmlOrLoadResources() {
    QPlainTextEdit source;
    source.setPlainText(QStringLiteral("|cff00ff00绿|r默认|n||cffffffff <img src='file:///private'>中文"));
    WarcraftTextPreview widget; widget.Bind(&source, QStringLiteral("原字段草稿 <b>literal</b>"));
    const QString expected = QStringLiteral("绿默认\n|cffffffff <img src='file:///private'>中文");
    QCOMPARE(widget.Preview()->toPlainText(), expected);
    QTextCursor first(widget.Preview()->document()); first.setPosition(0); first.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(first.charFormat().foreground().color(), QColor(0, 255, 0));
    QTextCursor reset(widget.Preview()->document()); reset.setPosition(1); reset.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QVERIFY(!reset.charFormat().hasProperty(QTextFormat::ForegroundBrush));
    QVERIFY(!widget.Preview()->document()->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("file:///never-read"))).isValid());
    QVERIFY(!widget.Preview()->isUndoRedoEnabled());
    QVERIFY(widget.StatusText().contains(QStringLiteral("alpha")));
    QCOMPARE(source.toPlainText(), QStringLiteral("|cff00ff00绿|r默认|n||cffffffff <img src='file:///private'>中文"));
    if (qEnvironmentVariableIsSet("AUD_COLOUR_PREVIEW_CAPTURE_DIR")) {
        const auto error = PrepareCaptureDirectory(); QVERIFY2(error.isEmpty(), qPrintable(error));
        if (!captureDirectory.isEmpty()) {
            PreviewCaptureHost capture(source, widget);
            const auto failure = capture.SaveDraft(QStringLiteral("colour-format-draft-preview"));
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
        }
    }
}

void WarcraftTextPreviewTests::InsertsOnlyDraftWithOriginalUndoAndPickerCancel() {
    QPlainTextEdit source; source.setPlainText(QStringLiteral("原文"));
    WarcraftTextPreview widget; widget.Bind(&source);
    auto cursor = source.textCursor(); cursor.movePosition(QTextCursor::End); source.setTextCursor(cursor);
    QVERIFY(widget.InsertColour(QColor(18, 52, 86, 10)));
    QCOMPARE(source.toPlainText(), QStringLiteral("原文|cff123456"));
    source.undo(); QCOMPARE(source.toPlainText(), QStringLiteral("原文"));
    source.redo(); QCOMPARE(source.toPlainText(), QStringLiteral("原文|cff123456"));
    QVERIFY(widget.InsertReset()); QVERIFY(widget.InsertNewline());
    QCOMPARE(source.toPlainText(), QStringLiteral("原文|cff123456|r|n"));
    source.undo(); QCOMPARE(source.toPlainText(), QStringLiteral("原文|cff123456|r"));
    source.undo(); QCOMPARE(source.toPlainText(), QStringLiteral("原文|cff123456"));
    const QString before = source.toPlainText();
    QVERIFY(!widget.InsertColour(QColor())); QCOMPARE(source.toPlainText(), before);
    auto* button = widget.findChild<QPushButton*>(QStringLiteral("warcraftTextColour")); QVERIFY(button);
    std::unique_ptr<PreviewCaptureHost> capture;
    QString captureFailure;
    if (qEnvironmentVariableIsSet("AUD_COLOUR_PREVIEW_CAPTURE_DIR")) {
        const auto error = PrepareCaptureDirectory(); QVERIFY2(error.isEmpty(), qPrintable(error));
        if (!captureDirectory.isEmpty()) capture = std::make_unique<PreviewCaptureHost>(source, widget);
    }
    QTimer::singleShot(0, &widget, [&] {
        if (auto* picker = qobject_cast<QColorDialog*>(QApplication::activeModalWidget())) {
            if (capture) captureFailure = capture->SavePicker(*picker, button);
            picker->reject();
        } else if (capture) captureFailure = QStringLiteral("现有取色路径未显示实际Qt取色器");
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY2(captureFailure.isEmpty(), qPrintable(captureFailure)); capture.reset();
    QCOMPARE(source.toPlainText(), before);
    source.undo(); QCOMPARE(source.toPlainText(), QStringLiteral("原文"));

    QLineEdit line(QStringLiteral("A")); line.setMaxLength(3); widget.Bind(&line);
    line.setCursorPosition(1);
    QVERIFY(!widget.InsertColour(Qt::red)); QCOMPARE(line.text(), QStringLiteral("A"));
    QVERIFY(widget.InsertReset()); QCOMPARE(line.text(), QStringLiteral("A|r"));
    line.undo(); QCOMPARE(line.text(), QStringLiteral("A"));
}

void WarcraftTextPreviewTests::UnknownReferencesBudgetAndBadUnicodePreserveDraft() {
    QPlainTextEdit source; source.setPlainText(QStringLiteral("|cff00ff00当前正文|r"));
    WarcraftTextPreview widget; widget.Bind(&source); const auto last = widget.Preview()->toPlainText();
    source.setPlainText(QStringLiteral("TRIGSTR_42")); widget.Refresh();
    QCOMPARE(source.toPlainText(), QStringLiteral("TRIGSTR_42"));
    QVERIFY(widget.StatusText().contains(QStringLiteral("未解析"))); QVERIFY(!widget.InsertReset());
    QCOMPARE(widget.Preview()->toPlainText(), last);
    const QString large(256 * 1024 + 1, QLatin1Char('x'));
    source.setPlainText(large); widget.Refresh();
    QCOMPARE(source.toPlainText(), large); QCOMPARE(widget.Preview()->toPlainText(), last);
    QVERIFY(widget.StatusText().contains(QStringLiteral("预算")) && widget.StatusText().contains(QStringLiteral("上次")));
    QString malformed; malformed.append(QChar(0xd800)); source.setPlainText(malformed); widget.Refresh();
    QCOMPARE(source.toPlainText(), malformed); QVERIFY(widget.StatusText().contains(QStringLiteral("Unicode")));
    widget.Bind(&source, QStringLiteral("来源未确认"), false); QVERIFY(!widget.InsertNewline());
    QCOMPARE(source.toPlainText(), malformed); QVERIFY(widget.StatusText().contains(QStringLiteral("未确认")));
}

void WarcraftTextPreviewTests::BindingGenerationRejectsLatePickerAndOwnerDeletion() {
    QPlainTextEdit first; first.setPlainText(QStringLiteral("旧草稿"));
    QPlainTextEdit second; second.setPlainText(QStringLiteral("新草稿"));
    WarcraftTextPreview widget; widget.Bind(&first);
    auto* button = widget.findChild<QPushButton*>(QStringLiteral("warcraftTextColour")); QVERIFY(button);
    QTimer::singleShot(0, &widget, [&] {
        widget.Bind(&second);
        if (auto* picker = qobject_cast<QColorDialog*>(QApplication::activeModalWidget())) {
            picker->setCurrentColor(Qt::red); picker->accept();
        }
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(first.toPlainText(), QStringLiteral("旧草稿")); QCOMPARE(second.toPlainText(), QStringLiteral("新草稿"));
    QVERIFY(widget.InsertReset()); QCOMPARE(second.toPlainText(), QStringLiteral("|r新草稿"));
    first.setPlainText(QStringLiteral("不会重新绑定")); widget.Refresh();
    QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("新草稿"));

    QPlainTextEdit independent;
    QPointer<WarcraftTextPreview> deleted = new WarcraftTextPreview;
    deleted->Bind(&independent);
    QObject::connect(&independent, &QPlainTextEdit::textChanged, &independent, [&] { delete deleted.data(); });
    (void)deleted->InsertReset(); QVERIFY(deleted.isNull());
    QCOMPARE(independent.toPlainText(), QStringLiteral("|r"));
}

void WarcraftTextPreviewTests::ResolverDisplaysPreparedTextAndRejectsUnknownOrStale() {
    using namespace War3Map::UiKit;
    QPlainTextEdit draft; draft.setPlainText(QStringLiteral("TRIGSTR_7"));
    WarcraftTextPreview widget; int calls = 0; QString received; WarcraftTextSourceState state = WarcraftTextSourceState::Available;
    const auto resolver = [&](const QString& raw) {
        ++calls; received = raw;
        return WarcraftTextSource{QStringLiteral("|cff00ff00真实正文|r|n第二行"), QStringLiteral("当前 owned 来源"), state};
    };
    widget.Bind(&draft, {}, true, resolver);
    QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("真实正文\n第二行"));
    QCOMPARE(calls, 1); QCOMPARE(received, QStringLiteral("TRIGSTR_7")); QCOMPARE(draft.toPlainText(), QStringLiteral("TRIGSTR_7"));
    state = WarcraftTextSourceState::Unknown; widget.Refresh();
    QVERIFY(widget.StatusText().contains(QStringLiteral("没有可确认")));
    state = WarcraftTextSourceState::Stale; widget.Refresh();
    QVERIFY(widget.StatusText().contains(QStringLiteral("已失效")) && widget.StatusText().contains(QStringLiteral("未代表当前")));
    QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("真实正文\n第二行"));
    QCOMPARE(calls, 3); QCOMPARE(draft.toPlainText(), QStringLiteral("TRIGSTR_7"));
}

void WarcraftTextPreviewTests::ResolverReentryAndDeletionCannotPublishOldResult() {
    using namespace War3Map::UiKit;
    QPlainTextEdit first; first.setPlainText(QStringLiteral("TRIGSTR_1"));
    QPlainTextEdit second; second.setPlainText(QStringLiteral("当前草稿"));
    WarcraftTextPreview widget; bool rebound = false;
    widget.Bind(&first, {}, true, [&](const QString&) {
        rebound = true; widget.Bind(&second);
        return WarcraftTextSource{QStringLiteral("旧正文不能覆盖"), {}, WarcraftTextSourceState::Available};
    });
    QVERIFY(rebound); QCOMPARE(widget.Preview()->toPlainText(), QStringLiteral("当前草稿"));
    QCOMPARE(first.toPlainText(), QStringLiteral("TRIGSTR_1"));
    QPointer<WarcraftTextPreview> deleted = new WarcraftTextPreview;
    deleted->Bind(&first, {}, true, [&](const QString&) {
        delete deleted.data();
        return WarcraftTextSource{QStringLiteral("销毁后的旧正文"), {}, WarcraftTextSourceState::Available};
    });
    QVERIFY(deleted.isNull());
    QPointer<WarcraftTextPreview> throwing = new WarcraftTextPreview;
    throwing->Bind(&first, {}, true, [&](const QString&) -> WarcraftTextSource {
        delete throwing.data(); throw std::runtime_error("owned callback failure");
    });
    QVERIFY(throwing.isNull());
}

void InitializeWarcraftTextStyle() { Q_INIT_RESOURCE(war3map_uikit_style); }

int main(int argc, char** argv) {
    QTemporaryDir settings; if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WarcraftTextPreview"));
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    InitializeWarcraftTextStyle();
    War3Map::UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
    WarcraftTextPreviewTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "WarcraftTextPreviewTests.moc"
