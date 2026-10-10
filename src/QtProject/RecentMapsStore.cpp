#include "RecentMapsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace War3Map::QtProject {
namespace {

RecentMapsResult Failure(const QString& message) {
    RecentMapsResult result;
    result.message = message;
    return result;
}

QString NormalizePath(const QString& path) {
    if (path.isEmpty() || path.size() > RecentMapsStore::kMaximumPathCharacters || !QDir::isAbsolutePath(path)) return {};
    for (const QChar character : path) if (character.unicode() < 0x20) return {};
    const auto cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const auto canonical = QFileInfo(cleaned).canonicalFilePath();
    return canonical.isEmpty() ? cleaned : QDir::cleanPath(canonical);
}

QString Identity(const QString& path) {
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

void RefreshAndSort(QVector<RecentMapEntry>& entries) {
    for (auto& entry : entries) {
        const QFileInfo file(entry.path);
        entry.available = file.exists() && (file.isFile() || file.isDir());
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.pinned != right.pinned) return left.pinned;
        if (left.lastOpenedUtc != right.lastOpenedUtc) return left.lastOpenedUtc > right.lastOpenedUtc;
        return Identity(left.path) < Identity(right.path);
    });
}

bool SafeStoreFile(const QString& path) {
    const QFileInfo file(path);
    return !file.isSymLink() && (!file.exists() || file.isFile());
}

RecentMapsResult ReadFile(const QString& filePath) {
    if (!SafeStoreFile(filePath)) return Failure(QStringLiteral("最近地图配置不是普通文件，未读取或覆盖。"));
    if (!QFileInfo::exists(filePath)) {
        RecentMapsResult result;
        result.succeeded = true;
        return result;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return Failure(QStringLiteral("无法读取最近地图配置：") + file.errorString());
    if (file.size() > RecentMapsStore::kMaximumJsonBytes) return Failure(QStringLiteral("最近地图配置超过容量限制，未覆盖。"));
    const auto bytes = file.read(RecentMapsStore::kMaximumJsonBytes + 1);
    if (bytes.size() > RecentMapsStore::kMaximumJsonBytes || file.error() != QFile::NoError)
        return Failure(QStringLiteral("最近地图配置读取失败或超过容量限制，未覆盖。"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return Failure(QStringLiteral("最近地图配置格式无效，原文件已保留。"));
    const auto object = document.object();
    if (object.size() != 2 || object.value(QStringLiteral("schema")).toDouble(-1) != 1
        || !object.value(QStringLiteral("maps")).isArray())
        return Failure(QStringLiteral("最近地图配置版本或字段无效，原文件已保留。"));
    const auto maps = object.value(QStringLiteral("maps")).toArray();
    if (maps.size() > RecentMapsStore::kMaximumEntries)
        return Failure(QStringLiteral("最近地图配置条目超过 10 条，原文件已保留。"));
    RecentMapsResult result;
    QSet<QString> identities;
    for (const auto& value : maps) {
        if (!value.isObject()) return Failure(QStringLiteral("最近地图配置条目无效，原文件已保留。"));
        const auto entryObject = value.toObject();
        if (entryObject.size() != 3 || !entryObject.value(QStringLiteral("path")).isString()
            || !entryObject.value(QStringLiteral("lastOpenedUtc")).isString()
            || !entryObject.value(QStringLiteral("pinned")).isBool())
            return Failure(QStringLiteral("最近地图配置条目字段无效，原文件已保留。"));
        RecentMapEntry entry;
        entry.path = NormalizePath(entryObject.value(QStringLiteral("path")).toString());
        entry.lastOpenedUtc = QDateTime::fromString(entryObject.value(QStringLiteral("lastOpenedUtc")).toString(), Qt::ISODateWithMs).toUTC();
        entry.pinned = entryObject.value(QStringLiteral("pinned")).toBool();
        if (entry.path.isEmpty() || !entry.lastOpenedUtc.isValid() || identities.contains(Identity(entry.path)))
            return Failure(QStringLiteral("最近地图配置路径、时间或重复身份无效，原文件已保留。"));
        identities.insert(Identity(entry.path));
        result.entries.push_back(std::move(entry));
    }
    RefreshAndSort(result.entries);
    result.succeeded = true;
    return result;
}

bool WriteFile(const QString& path, const QVector<RecentMapEntry>& entries, QString& error) {
    if (!SafeStoreFile(path)) { error = QStringLiteral("最近地图配置不是普通文件，未覆盖。"); return false; }
    QJsonArray maps;
    for (const auto& entry : entries) maps.push_back(QJsonObject{
        {QStringLiteral("path"), entry.path},
        {QStringLiteral("lastOpenedUtc"), entry.lastOpenedUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("pinned"), entry.pinned}});
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("maps"), maps}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > RecentMapsStore::kMaximumJsonBytes) { error = QStringLiteral("最近地图配置超过容量限制，未写入。"); return false; }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("无法原子保存最近地图配置：") + file.errorString();
        return false;
    }
    return true;
}

} // namespace

RecentMapsStore::RecentMapsStore(QString dataDirectory, int lockTimeoutMs)
    : lockTimeoutMs_(std::clamp(lockTimeoutMs, 0, 5000)) {
    if (dataDirectory.isEmpty()) dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!dataDirectory.isEmpty() && QDir::isAbsolutePath(dataDirectory)) directory_ = QDir::cleanPath(dataDirectory);
}

QString RecentMapsStore::FilePath() const {
    return directory_.isEmpty() ? QString{} : QDir(directory_).filePath(QStringLiteral("recent-maps.json"));
}

QString RecentMapsStore::LockPath() const {
    return directory_.isEmpty() ? QString{} : FilePath() + QStringLiteral(".lock");
}

RecentMapsResult RecentMapsStore::Read() const {
    if (directory_.isEmpty()) return Failure(QStringLiteral("最近地图配置目录不是绝对路径。"));
    return ReadFile(FilePath());
}

RecentMapsResult RecentMapsStore::RecordOpened(const QString& absolutePath, QDateTime openedUtc) const {
    if (!openedUtc.isValid()) openedUtc = QDateTime::currentDateTimeUtc();
    return Mutate(Mutation::Opened, absolutePath, openedUtc.toUTC(), false);
}

RecentMapsResult RecentMapsStore::SetPinned(const QString& absolutePath, bool pinned) const {
    return Mutate(Mutation::Pin, absolutePath, {}, pinned);
}

RecentMapsResult RecentMapsStore::Remove(const QString& absolutePath) const {
    return Mutate(Mutation::Remove, absolutePath, {}, false);
}

RecentMapsResult RecentMapsStore::Mutate(Mutation mutation, const QString& path, QDateTime openedUtc, bool pinned) const {
    const auto normalized = NormalizePath(path);
    if (directory_.isEmpty() || normalized.isEmpty()) return Failure(QStringLiteral("最近地图目录和地图路径必须是有效的绝对路径。"));
    if (!QDir().mkpath(directory_)) return Failure(QStringLiteral("无法创建最近地图配置目录。"));
    if (!SafeStoreFile(LockPath())) return Failure(QStringLiteral("最近地图锁文件无效，未写入。"));
    QLockFile lock(LockPath());
    lock.setStaleLockTime(0); // 不因墙钟时长驱逐仍在运行的另一个实例。
    if (!lock.tryLock(lockTimeoutMs_)) return Failure(QStringLiteral("最近地图正在由另一个实例更新，请稍后重试。"));
    auto result = ReadFile(FilePath()); // 取得锁之后重读，不能用调用者先前的列表覆盖其他实例。
    if (!result.succeeded) return result;
    const auto key = Identity(normalized);
    auto existing = std::find_if(result.entries.begin(), result.entries.end(), [&](const auto& entry) { return Identity(entry.path) == key; });
    if (mutation != Mutation::Opened && existing == result.entries.end())
        return Failure(QStringLiteral("这条最近地图记录已不存在，请刷新列表。"));
    if (mutation == Mutation::Opened) {
        if (existing == result.entries.end()) {
            if (result.entries.size() == kMaximumEntries) {
                auto victim = result.entries.end();
                for (auto entry = result.entries.begin(); entry != result.entries.end(); ++entry)
                    if (!entry->pinned && (victim == result.entries.end() || entry->lastOpenedUtc < victim->lastOpenedUtc)) victim = entry;
                if (victim == result.entries.end()) {
                    result.message = QStringLiteral("10 条最近地图均已固定；本次打开成功，但未加入最近地图。请取消固定或移除一条记录。");
                    return result;
                }
                result.entries.erase(victim);
            }
            result.entries.push_back({normalized, openedUtc, false, false});
            result.changed = true;
        } else {
            // ReadFile 已将现有条目按当前文件规范化；仅大小写不同的别名不改其展示路径。
            result.changed = existing->lastOpenedUtc != openedUtc;
            existing->lastOpenedUtc = openedUtc;
        }
        result.recorded = true;
    } else if (mutation == Mutation::Pin) {
        result.changed = existing->pinned != pinned;
        existing->pinned = pinned;
    } else {
        result.entries.erase(existing);
        result.changed = true;
    }
    RefreshAndSort(result.entries);
    if (result.changed && !WriteFile(FilePath(), result.entries, result.message)) {
        result.succeeded = false;
        result.changed = false;
        result.recorded = false;
        result.entries.clear();
    }
    return result;
}

} // namespace War3Map::QtProject
