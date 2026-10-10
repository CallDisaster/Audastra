#pragma once

#include <QComboBox>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace War3Map::QtFeedback::Detail {

inline QLabel* PlainLabel(const QString& text, bool secondary = false, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (secondary) {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
        label->setPalette(palette);
    }
    return label;
}

inline QLabel* Heading(const QString& text) {
    auto* label = PlainLabel(text);
    auto font = label->font();
    font.setPointSize(font.pointSize() + 3);
    font.setWeight(QFont::Medium);
    label->setFont(font);
    return label;
}

inline QPlainTextEdit* PayloadText(const QString& name, const QString& text) {
    auto* editor = new QPlainTextEdit;
    editor->setObjectName(name);
    editor->setReadOnly(true);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setPlainText(text);
    return editor;
}

// PNG保留的只是原像素；显示比例由当前绘制设备决定，不信任或恢复已清理的DPI元信息。
// 两处截图审核共用该控件，切换模式只改变显示pixmap，不重新编码或修改报告附件。
class ScreenshotView final : public QWidget {
public:
    ScreenshotView(const QImage& image, const QString& imageObjectName, QWidget* parent = nullptr)
        : QWidget(parent), image_(image) {
        image_.setDevicePixelRatio(1.0);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* controls = new QHBoxLayout;
        controls->addStretch(1);
        controls->addWidget(PlainLabel(QStringLiteral("显示：")));
        mode_ = new QComboBox;
        mode_->setObjectName(imageObjectName + QStringLiteral("ScaleMode"));
        mode_->addItem(QStringLiteral("适应窗口"));
        mode_->addItem(QStringLiteral("实际像素"));
        controls->addWidget(mode_);
        layout->addLayout(controls);
        scroll_ = new QScrollArea;
        scroll_->setObjectName(imageObjectName + QStringLiteral("Scroll"));
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setAlignment(Qt::AlignCenter);
        imageLabel_ = new QLabel;
        imageLabel_->setObjectName(imageObjectName);
        imageLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        imageLabel_->setMargin(0);
        scroll_->setWidget(imageLabel_);
        scroll_->viewport()->installEventFilter(this);
        layout->addWidget(scroll_, 1);
        QObject::connect(mode_, &QComboBox::currentIndexChanged, this, [this](int) { UpdateImage(); });
        UpdateImage();
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal) UpdateImage();
        return handled;
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (scroll_ && watched == scroll_->viewport() &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::DevicePixelRatioChange)) {
            UpdateImage();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void UpdateImage() {
        if (updating_ || !mode_ || !scroll_ || !imageLabel_ || image_.isNull()) return;
        const qreal dpr = imageLabel_->devicePixelRatioF();
        if (!std::isfinite(dpr) || dpr <= 0.0) return;
        updating_ = true;
        const bool actualPixels = mode_->currentIndex() == 1;
        scroll_->setHorizontalScrollBarPolicy(actualPixels ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        scroll_->setVerticalScrollBarPolicy(actualPixels ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        QImage display = image_;
        if (!actualPixels) {
            const QSize available(std::max(1, qFloor(scroll_->viewport()->width() * dpr)),
                std::max(1, qFloor(scroll_->viewport()->height() * dpr)));
            if (display.width() > available.width() || display.height() > available.height()) {
                display = image_.scaled(available, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        auto pixmap = QPixmap::fromImage(display);
        pixmap.setDevicePixelRatio(dpr);
        imageLabel_->setPixmap(pixmap);
        imageLabel_->setFixedSize(qCeil(pixmap.width() / dpr), qCeil(pixmap.height() / dpr));
        updating_ = false;
    }

    QImage image_;
    QComboBox* mode_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    bool updating_ = false;
};

} // namespace War3Map::QtFeedback::Detail
