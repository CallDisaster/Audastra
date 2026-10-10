#include "RecentMapsList.h"

#include "Theme/DesignTokens.generated.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace War3Map::Shell {
namespace {

QString SafeTooltip(const QString& plainText) {
    return QStringLiteral("<qt>") + plainText.toHtmlEscaped() + QStringLiteral("</qt>");
}

class ElidedPathLabel final : public QLabel {
public:
    ElidedPathLabel(QString path, QWidget* parent) : QLabel(parent), path_(std::move(path)) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setAccessibleName(path_);
        setToolTip(SafeTooltip(path_));
        setText(path_);
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(path_, Qt::ElideMiddle, width()));
    }
private:
    QString path_;
};

} // namespace

RecentMapsList::RecentMapsList(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("recentMapsList"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(8);
    rows_ = new QVBoxLayout;
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(8);
    outer->addLayout(rows_);
    empty_ = new QLabel(tr("暂无最近打开的地图"), this);
    empty_->setObjectName(QStringLiteral("recentMapsEmpty"));
    empty_->setTextFormat(Qt::PlainText);
    empty_->setWordWrap(true);
    outer->addWidget(empty_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("recentMapsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->hide();
    outer->addWidget(status_);
}

bool RecentMapsList::SetEntries(const QVector<RecentMapListEntry>& entries) {
    while (auto* item = rows_->takeAt(0)) {
        if (auto* retired = item->widget()) {
            // 刷新可由当前按钮的信号同步触发；延迟销毁，避免释放仍在处理点击的发送者。
            retired->hide();
            retired->setParent(nullptr);
            retired->deleteLater();
        }
        delete item;
    }
    if (entries.size() > 10 || std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.path.isEmpty() || entry.path.size() > 4096;
    })) {
        empty_->show();
        SetStatus(tr("最近地图列表条目或路径超过限制，未显示。"));
        return false;
    }
    SetStatus({});
    empty_->setVisible(entries.isEmpty());
    for (const auto& entry : entries) {
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("recentMapRow"));
        row->setProperty("recentMapPath", entry.path);
        auto* vertical = new QVBoxLayout(row);
        vertical->setContentsMargins(0, 0, 0, 0);
        vertical->setSpacing(2);
        auto* top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(6);
        const auto fileName = QFileInfo(entry.path).fileName();
        auto buttonText = fileName.isEmpty() ? entry.path : fileName;
        auto* open = new QToolButton(row);
        open->setObjectName(QStringLiteral("recentMapOpen"));
        open->setText(buttonText.replace(QLatin1Char('&'), QStringLiteral("&&")));
        open->setAccessibleName(fileName.isEmpty() ? entry.path : fileName);
        open->setToolTip(SafeTooltip(entry.path));
        open->setToolButtonStyle(Qt::ToolButtonTextOnly);
        open->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        open->setMinimumWidth(0);
        open->setMinimumHeight(UiKit::Tokens::Size::kMenuItemHeight);
        open->setEnabled(entry.available);
        top->addWidget(open, 1);
        auto* pin = new QToolButton(row);
        pin->setObjectName(QStringLiteral("recentMapPin"));
        pin->setText(entry.pinned ? tr("取消固定") : tr("固定"));
        pin->setAccessibleName(entry.pinned ? tr("取消固定地图") : tr("固定地图"));
        top->addWidget(pin);
        auto* remove = new QToolButton(row);
        remove->setObjectName(QStringLiteral("recentMapRemove"));
        remove->setText(tr("移除"));
        remove->setAccessibleName(tr("从最近地图移除"));
        top->addWidget(remove);
        vertical->addLayout(top);
        auto* path = new ElidedPathLabel(QDir::toNativeSeparators(entry.path), row);
        path->setObjectName(QStringLiteral("recentMapPath"));
        vertical->addWidget(path);
        const auto time = entry.lastOpenedUtc.isValid() ? entry.lastOpenedUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : tr("时间未知");
        auto* detail = new QLabel(entry.available ? tr("上次打开 %1").arg(time) : tr("文件不存在 · 上次打开 %1").arg(time), row);
        detail->setObjectName(QStringLiteral("recentMapDetails"));
        detail->setTextFormat(Qt::PlainText);
        detail->setWordWrap(true);
        if (!entry.available) {
            auto palette = path->palette();
            palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
            path->setPalette(palette);
        }
        vertical->addWidget(detail);
        rows_->addWidget(row);
        const auto ownedPath = entry.path;
        connect(open, &QToolButton::clicked, this, [this, ownedPath] { emit OpenRequested(ownedPath); });
        connect(pin, &QToolButton::clicked, this, [this, ownedPath, pinned = entry.pinned] { emit PinRequested(ownedPath, !pinned); });
        connect(remove, &QToolButton::clicked, this, [this, ownedPath] { emit RemoveRequested(ownedPath); });
    }
    return true;
}

void RecentMapsList::SetStatus(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

} // namespace War3Map::Shell
