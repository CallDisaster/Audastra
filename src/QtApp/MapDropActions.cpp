#include "MainWindow.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <functional>

namespace {
bool IsMapUrl(const QUrl& url) {
    const auto suffix = QFileInfo(url.path()).suffix();
    return suffix.compare(QStringLiteral("w3x"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("w3m"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("w3n"), Qt::CaseInsensitive) == 0;
}

// 原生世界窗口、目录及停靠页各自接收拖放。仅截获本主窗口里的外部地图URL，
// 其他资源和模块内拖放仍交原控件；模态对话框和其他主窗口不属于此路由。
class MapFileDropRouter final : public QObject {
public:
    MapFileDropRouter(QWidget& owner, std::function<bool()> available,
        std::function<void(const QString&)> open)
        : QObject(&owner), owner_(owner), available_(std::move(available)), open_(std::move(open)) {
        owner_.setAcceptDrops(true);
        qApp->installEventFilter(this);
    }
private:
    bool BelongsToWindow(QObject* watched) const {
        if (const auto* widget = qobject_cast<QWidget*>(watched)) return widget->window() == &owner_;
        const auto* native = qobject_cast<QWindow*>(watched);
        for (auto* window = native; window; window = window->parent())
            if (window == owner_.windowHandle()) return true;
        return false;
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove
            && event->type() != QEvent::Drop) return false;
        if (!BelongsToWindow(watched)) return false;
        auto* drop = static_cast<QDropEvent*>(event);
        const auto urls = drop->mimeData()->urls();
        if (urls.empty() || !std::any_of(urls.begin(), urls.end(), IsMapUrl)) return false;
        // 一个手势只打开一张本地地图；不下载远程URL，不猜多文件顺序。
        const bool local = urls.size() == 1 && urls.front().isLocalFile()
            && (urls.front().host().isEmpty() || urls.front().host() == QStringLiteral("localhost"));
        const auto path = local ? urls.front().toLocalFile() : QString{};
        if (!local || !QFileInfo(path).isFile() || !available_()
            || !(drop->possibleActions() & Qt::CopyAction)) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction); // 打开不移动源文件。
        drop->accept();
        if (event->type() == QEvent::Drop) open_(path);
        return true;
    }
    QWidget& owner_;
    std::function<bool()> available_;
    std::function<void(const QString&)> open_;
};
}

void MainWindow::SetupMapFileDrop() {
    new MapFileDropRouter(*this,
        [this] { return !project_->savingMap_ && !changesChecking_ && !mapDropQueued_; },
        [this](const QString& path) {
            mapDropQueued_ = true;
            // 退出Windows拖放回调后再打开/询问未保存修改，避免在原生拖放循环中重入。
            QTimer::singleShot(0, this, [this, path] {
                const QPointer<MainWindow> liveWindow(this);
                const auto queued = qScopeGuard([liveWindow] {
                    if (liveWindow) liveWindow->mapDropQueued_ = false;
                });
                if (!project_->savingMap_ && !changesChecking_)
                    (void)OpenUserMapArchivePath(std::filesystem::path(path.toStdWString()));
            });
        });
}
