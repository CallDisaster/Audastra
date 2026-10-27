#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

class QTimer;
namespace War3Map::Agent {

struct MapBackupMigrationRequest {
    QString from, to;
    std::size_t keepBackups = 10;
    QString unavailableReason; // 旧偏好无效时只报告，不推测迁移源。
};
struct MapBackupMigrationDispatch {
    std::optional<QString> genericDataRoot; // 缺省使用本机GenericDataLocation；测试可显式隔离。
    std::function<void(MapBackupMigrationRequest)> submit;
};
struct MapBackupMigrationReport {
    std::size_t moved = 0, skipped = 0, pruned = 0;
    bool hasResult = true, complete = true;
    QStringList diagnostics;
};

// 应用级串行worker，生命周期不依赖设置页。后台只持有输入和共享结果，不访问QObject。
class MapBackupMigrationService final : public QObject {
    Q_OBJECT
public:
    using Worker = std::function<MapBackupMigrationReport(const MapBackupMigrationRequest&, std::stop_token)>;
    explicit MapBackupMigrationService(QObject* parent = nullptr, Worker worker = {});
    ~MapBackupMigrationService() override;
    static MapBackupMigrationService* Application(); // 仅应用线程；不启动迁移。
    static bool Post(MapBackupMigrationRequest request); // 任何线程，异步投递。
    void Submit(MapBackupMigrationRequest request);
    bool Busy() const;
    QString Text() const { return text_; }
signals:
    void StatusChanged(const QString& text, bool busy);
private:
    struct Task {
        std::mutex mutex;
        std::stop_source stop;
        std::optional<MapBackupMigrationReport> result;
    };
    void Launch(MapBackupMigrationRequest request);
    void Poll();
    Worker worker_;
    QTimer* timer_;
    std::deque<MapBackupMigrationRequest> pending_;
    std::shared_ptr<Task> task_;
    QString text_;
};
} // namespace War3Map::Agent
