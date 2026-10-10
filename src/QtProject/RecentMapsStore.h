#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace War3Map::QtProject {

struct RecentMapEntry {
    QString path;
    QDateTime lastOpenedUtc;
    bool pinned = false;
    bool available = false; // 每次读取根据路径状态计算，不写入配置。
};

struct RecentMapsResult {
    bool succeeded = false;
    bool changed = false;
    bool recorded = false;
    QString message;
    QVector<RecentMapEntry> entries;
};

// 仅保存本机用户的打开历史；不读取地图内容，不承担打开/探针来源的判断。
// 宿主只有在真实用户打开成功后调用 RecordOpened，不能登记工程内部基线或探针。
class RecentMapsStore final {
public:
    static constexpr int kMaximumEntries = 10;
    static constexpr qint64 kMaximumJsonBytes = 256 * 1024;
    static constexpr int kMaximumPathCharacters = 4096;

    // 空参数使用 AppLocalDataLocation。测试必须注入独立 QTemporaryDir。
    explicit RecentMapsStore(QString dataDirectory = {}, int lockTimeoutMs = 1000);
    [[nodiscard]] QString FilePath() const;
    [[nodiscard]] QString LockPath() const;
    [[nodiscard]] RecentMapsResult Read() const;
    [[nodiscard]] RecentMapsResult RecordOpened(const QString& absolutePath, QDateTime openedUtc = {}) const;
    [[nodiscard]] RecentMapsResult SetPinned(const QString& absolutePath, bool pinned) const;
    [[nodiscard]] RecentMapsResult Remove(const QString& absolutePath) const;

private:
    enum class Mutation { Opened, Pin, Remove };
    [[nodiscard]] RecentMapsResult Mutate(Mutation mutation, const QString& path,
                                         QDateTime openedUtc, bool pinned) const;
    QString directory_;
    int lockTimeoutMs_ = 1000;
};

} // namespace War3Map::QtProject
