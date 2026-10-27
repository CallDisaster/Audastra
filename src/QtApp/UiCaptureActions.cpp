#include "MainWindow.h"
#include "Shell/StartupPage.h"
#include "Widgets/PopoverSurface.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageReader>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>

#include <cmath>
#include <optional>

// 实际 Qt 控件逐图捕获；原生 GPU 世界画面仍须使用完成帧探针。
bool MainWindow::CaptureUi(const std::filesystem::path& directory, QStringList& log) {
    const auto arguments = QCoreApplication::arguments();
    QSize requested(1600, 1000);
    std::optional<double> expectedDpr;
    const auto value = [&](const QString& option) -> std::optional<QString> {
        if (arguments.count(option) != 1) return std::nullopt;
        const auto at = arguments.indexOf(option);
        if (at + 1 >= arguments.size() || arguments[at + 1].startsWith(QStringLiteral("--"))) return std::nullopt;
        return arguments[at + 1];
    };
    if (arguments.contains(QStringLiteral("--capture-ui-size"))) {
        const auto text = value(QStringLiteral("--capture-ui-size"));
        const auto match = QRegularExpression(QStringLiteral("^([1-9][0-9]{0,3})x([1-9][0-9]{0,3})$")).match(text.value_or(QString{}));
        if (!match.hasMatch()) { log << QStringLiteral("capture-ui=invalid-size"); return false; }
        requested = QSize(match.captured(1).toInt(), match.captured(2).toInt());
    }
    if (arguments.contains(QStringLiteral("--capture-ui-expected-dpr"))) {
        const auto text = value(QStringLiteral("--capture-ui-expected-dpr"));
        bool valid = false;
        const double ratio = text ? text->toDouble(&valid) : 0;
        if (!valid || !std::isfinite(ratio) || ratio <= 0 || ratio > 4) {
            log << QStringLiteral("capture-ui=invalid-expected-dpr"); return false;
        }
        expectedDpr = ratio;
    }
    if (requested.width() > 4096 || requested.height() > 4096) {
        log << QStringLiteral("capture-ui=logical-size-budget"); return false;
    }
    // 原生 Popup 会按显示器范围定位并取得焦点；此入口只在隔离 Qt offscreen 平台捕获。
    if (startupMode_ == StartupMode::Normal || QApplication::platformName() != QStringLiteral("offscreen")) {
        log << QStringLiteral("capture-ui=requires-nonpersistent-offscreen-probe"); return false;
    }
    const double requestedPixels = std::ceil(requested.width() * devicePixelRatioF())
        * std::ceil(requested.height() * devicePixelRatioF());
    if (!std::isfinite(requestedPixels) || requestedPixels > 16 * 1024 * 1024) {
        log << QStringLiteral("capture-ui=requested-pixel-budget"); return false;
    }
    const QDir out(QString::fromStdWString(directory.wstring()));
    if (!out.exists() && !QDir().mkpath(out.absolutePath())) { log << QStringLiteral("capture-ui=output-unavailable"); return false; }
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    resize(requested);
    QRect screens;
    for (const auto* screen : QApplication::screens()) screens = screens.united(screen->geometry());
    if (!screens.isEmpty()) move(screens.left() - width() - 256, screens.top() - height() - 256);
    show();
    const auto settle = [](int milliseconds) {
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < milliseconds) QApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    settle(500);
    const auto dimensions = [](QSize size) { return QJsonObject{{"width", size.width()}, {"height", size.height()}}; };
    const auto geometry = [](QRect rect) {
        return QJsonObject{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
    };
    const auto writeJson = [](const QString& path, const QJsonObject& json) {
        QSaveFile file(path);
        const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    bool ok = true;
    QJsonArray images;
    const auto capture = [&](QWidget& subject, const QString& name, const QString& kind) {
        const double ratio = subject.devicePixelRatioF();
        const auto physicalWidth = std::ceil(subject.width() * ratio), physicalHeight = std::ceil(subject.height() * ratio);
        if (!std::isfinite(ratio) || ratio <= 0 || physicalWidth > 8192 || physicalHeight > 8192
            || physicalWidth * physicalHeight > 16 * 1024 * 1024) {
            log << QStringLiteral("capture-ui=%1 pixel-budget-exceeded").arg(name); ok = false; return;
        }
        const auto pixmap = subject.grab(); // 本次真实控件像素，禁止重采样。
        const bool windowMatches = size() == requested;
        const bool dprMatches = !expectedDpr || std::abs(ratio - *expectedDpr) < 1e-6;
        const QString fileName = name + QStringLiteral(".png");
        const bool saved = !pixmap.isNull() && pixmap.save(out.filePath(fileName), "PNG");
        const QSize pngSize = saved ? QImageReader(out.filePath(fileName)).size() : QSize{};
        const bool pixelsMatch = pixmap.size() == QSize(qRound(subject.width() * ratio), qRound(subject.height() * ratio))
            && std::abs(pixmap.devicePixelRatioF() - ratio) < 1e-6 && pngSize == pixmap.size();
        QJsonObject receipt{{"file", fileName}, {"kind", kind}, {"capture_method", "QWidget::grab"},
            {"captured_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"qt_platform", QApplication::platformName()}, {"requested_window_logical", dimensions(requested)},
            {"actual_window_logical", dimensions(size())}, {"window_geometry_logical", geometry(this->geometry())},
            {"window_frame_geometry_logical", geometry(frameGeometry())}, {"subject_logical", dimensions(subject.size())},
            {"subject_geometry_logical", geometry(subject.geometry())}, {"png_physical", dimensions(pngSize)},
            {"device_pixel_ratio", ratio}, {"pixmap_device_pixel_ratio", pixmap.devicePixelRatioF()},
            {"screen_available_logical", subject.screen() ? geometry(subject.screen()->availableGeometry()) : QJsonObject{}},
            {"requested_device_pixel_ratio", expectedDpr ? QJsonValue(*expectedDpr) : QJsonValue(QJsonValue::Null)},
            {"window_request_matches", windowMatches}, {"dpr_request_matches", dprMatches},
            {"pixels_match_subject", pixelsMatch}, {"saved", saved}, {"resampled", false}};
        const bool receiptSaved = writeJson(out.filePath(name + QStringLiteral(".json")), receipt);
        images.append(receipt);
        ok &= saved && receiptSaved && windowMatches && dprMatches && pixelsMatch;
        log << QStringLiteral("capture-ui %1 window=%2x%3 png=%4x%5 dpr=%6 window-match=%7 dpr-match=%8 saved=%9")
            .arg(name).arg(width()).arg(height()).arg(pixmap.width()).arg(pixmap.height()).arg(ratio)
            .arg(windowMatches).arg(dprMatches).arg(saved);
    };
    const bool startup = workspacePages_ && startupPage_ && workspacePages_->currentWidget() == startupPage_;
    if (startup) capture(*this, QStringLiteral("startup"), QStringLiteral("startup_window"));
    else {
        for (const auto& id : modules_.AvailableIds()) {
            ActivateModule(id);
            settle(400);
            capture(*this, QString::fromStdString(id), QStringLiteral("module_window"));
        }
        // 变量属性表单是模态窗口：有可编辑变量时打开、抓取本次真实像素再取消，不提交任何修改。
        ActivateModule("variables");
        settle(200);
        QPushButton* properties = nullptr;
        for (auto* widget : QApplication::allWidgets()) {
            if (widget->objectName() == QStringLiteral("variablePropertiesButton") && widget->isVisible())
                properties = qobject_cast<QPushButton*>(widget);
        }
        if (properties && properties->isEnabled()) {
            bool formCaptured = false;
            QTimer::singleShot(400, this, [&] {
                auto* form = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!form) return;
                capture(*form, QStringLiteral("variables-properties"), QStringLiteral("module_form"));
                formCaptured = true;
                form->reject();
            });
            properties->click();
            settle(500);
            if (!formCaptured) { log << QStringLiteral("capture-ui=variables-properties-unavailable"); ok = false; }
        }
    }
    ShowAbout();
    settle(200);
    auto* about = findChild<War3Map::UiKit::PopoverSurface*>(QStringLiteral("applicationAbout"));
    if (about && about->isVisible()) {
        // Popup 是独立窗口，单独捕获原始像素；不合成到宿主图上伪装全窗截图。
        capture(*about, QStringLiteral("about"), QStringLiteral("about_popup"));
        about->Dismiss();
    } else { log << QStringLiteral("capture-ui=about-unavailable"); ok = false; }
    const bool manifestSaved = writeJson(out.filePath(QStringLiteral("capture-manifest.json")),
        {{"schema_version", 1}, {"requested_window_logical", dimensions(requested)}, {"images", images}, {"passed", ok}});
    ok &= manifestSaved;
    return ok;
}
