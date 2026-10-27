#include "MapBackupMigrationService.h"
#include "War3MapCore/MapBackupMigration.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QStringList>
#include <thread>

namespace War3Map::Agent {
namespace {
MapBackupMigrationReport Run(const MapBackupMigrationRequest& request, std::stop_token stop) {
    MapBackupMigrationOptions options; options.keepBackups = request.keepBackups; options.stop = stop;
    const auto result = MigrateMapBackups(std::filesystem::path(request.from.toStdWString()),
        std::filesystem::path(request.to.toStdWString()), options);
    MapBackupMigrationReport report; report.hasResult = result.value.has_value();
    if (result.value) {
        report.moved = result.value->moved; report.skipped = result.value->skipped;
        report.pruned = result.value->pruned; report.complete = result.value->complete;
    }
    for (const auto& diagnostic : result.diagnostics) report.diagnostics << QString::fromStdString(diagnostic.message);
    return report;
}
QString Report(const MapBackupMigrationReport& result) {
    QString text;
    if (result.hasResult) text = QObject::tr("已迁移地图备份 %1 份，跳过 %2 份，清理 %3 份")
        .arg(result.moved).arg(result.skipped).arg(result.pruned);
    else text = QObject::tr("地图备份迁移未完成，旧备份保留");
    // 设置页沿用现有状态位置；逐份完整诊断由Core返回，不把失败折叠成成功。
    for (qsizetype i = 0; i < result.diagnostics.size() && i < 8; ++i)
        text += QLatin1Char('\n') + result.diagnostics[i];
    if (result.diagnostics.size() > 8) text += QObject::tr("\n另有 %1 项未迁移诊断").arg(result.diagnostics.size() - 8);
    return text;
}
}
MapBackupMigrationService::MapBackupMigrationService(QObject* parent, Worker worker)
    : QObject(parent), worker_(worker ? std::move(worker) : Run), timer_(new QTimer(this)) {
    setObjectName(QStringLiteral("mapBackupMigrationService"));
    timer_->setInterval(50);
    connect(timer_, &QTimer::timeout, this, &MapBackupMigrationService::Poll);
}
MapBackupMigrationService::~MapBackupMigrationService() { if (task_) task_->stop.request_stop(); }
MapBackupMigrationService* MapBackupMigrationService::Application() {
    auto* app = QCoreApplication::instance();
    if (!app || app->thread() != QThread::currentThread()) return nullptr;
    auto* service = app->findChild<MapBackupMigrationService*>(QStringLiteral("mapBackupMigrationService"), Qt::FindDirectChildrenOnly);
    return service ? service : new MapBackupMigrationService(app);
}
bool MapBackupMigrationService::Post(MapBackupMigrationRequest request) {
    auto* app = QCoreApplication::instance();
    return app && QMetaObject::invokeMethod(app, [request = std::move(request)]() mutable {
        if (auto* service = Application()) service->Submit(std::move(request));
    }, Qt::QueuedConnection);
}
bool MapBackupMigrationService::Busy() const { return static_cast<bool>(task_) || !pending_.empty(); }
void MapBackupMigrationService::Submit(MapBackupMigrationRequest request) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (!request.unavailableReason.isEmpty()) {
        text_ = tr("设置已保存，旧备份未迁移：%1").arg(request.unavailableReason);
        emit StatusChanged(text_, Busy()); return;
    }
    if (request.from == request.to) return;
    if (task_ || !pending_.empty()) {
        if (pending_.size() >= 16) {
            text_ = tr("设置已保存，备份迁移队列已满；此次旧目录保持不变");
            emit StatusChanged(text_, true); return;
        }
        pending_.push_back(std::move(request)); return;
    }
    Launch(std::move(request));
}
void MapBackupMigrationService::Launch(MapBackupMigrationRequest request) {
    task_ = std::make_shared<Task>();
    auto task = task_; const auto worker = worker_;
    text_ = tr("正在后台迁移地图备份…");
    try {
        std::thread([task, request = std::move(request), worker] {
            MapBackupMigrationReport result;
            try { result = worker(request, task->stop.get_token()); }
            catch (...) { result.hasResult = false; result.complete = false; result.diagnostics << QStringLiteral("备份迁移异常中止，未提交的旧备份保留。"); }
            const std::lock_guard lock(task->mutex); task->result = std::move(result);
        }).detach();
        timer_->start();
    } catch (...) {
        task_.reset(); text_ = tr("地图备份迁移未能启动，旧备份保留");
    }
    emit StatusChanged(text_, Busy());
}
void MapBackupMigrationService::Poll() {
    if (!task_) return;
    std::optional<MapBackupMigrationReport> result;
    { const std::lock_guard lock(task_->mutex); if (task_->result) result = std::move(task_->result); }
    if (!result) return;
    task_.reset(); timer_->stop(); text_ = Report(*result);
    if (!pending_.empty()) {
        // 先发布本次结果；观察者可能销毁服务，下一任务通过QObject队列启动。
        QMetaObject::invokeMethod(this, [this] {
            if (task_ || pending_.empty()) return;
            auto request = std::move(pending_.front()); pending_.pop_front(); Launch(std::move(request));
        }, Qt::QueuedConnection);
    }
    emit StatusChanged(text_, Busy());
}
} // namespace War3Map::Agent
