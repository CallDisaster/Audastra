#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

class QTimer;

namespace War3Map::QtProject {

struct LegacyMapBackupReport {
    QStringList confirmedDirectories;
    QStringList unknownDirectories;
    QStringList checkedDirectories;
    bool discoveryIncomplete = false;
    QString Text() const;
};

// 只检查当前安装的 backups、同一便携版本族及手工选定的精确目录。
// unsigned manifest 仅限定发现范围；备份真伪由 Core 的 owner / 原生身份 / SHA 确认。
[[nodiscard]] LegacyMapBackupReport CheckLegacyMapBackups(
    const QString& installationRoot, const QString& explicitBackupDirectory,
    std::stop_token stop = {});

// 后台仅持有值与共享结果。取消 / 析构不在界面线程等待 Win32 I/O，
// 新请求等旧只读任务完成后再开始，同一服务至多一个任务。
class LegacyMapBackupCheck final : public QObject {
public:
    using Scanner = std::function<LegacyMapBackupReport(const QString&, const QString&, std::stop_token)>;
    explicit LegacyMapBackupCheck(QString installationRoot, QObject* parent = nullptr, Scanner scanner = {});
    ~LegacyMapBackupCheck() override;
    void Start(const QString& explicitBackupDirectory = {});
    void Cancel();
    bool Busy() const;
    QString Text() const;
    void SetObserver(std::function<void(QString, bool)> observer);
private:
    struct Task {
        std::mutex mutex;
        std::stop_source stop;
        std::optional<LegacyMapBackupReport> result;
    };
    void Launch(QString directory);
    void Poll();
    void Notify();
    QString installationRoot_;
    Scanner scanner_;
    QTimer* timer_ = nullptr;
    std::shared_ptr<Task> task_;
    std::optional<QString> pending_;
    QString text_;
    std::function<void(QString, bool)> observer_;
};

} // namespace War3Map::QtProject
