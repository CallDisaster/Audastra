#include "LegacyMapBackupCheck.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include <Windows.h>

#include <chrono>
#include <algorithm>
#include <thread>

namespace War3Map::QtProject {
namespace {
struct ReadHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ReadHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
QByteArray ReadManifest(const QString& path) {
    // 不借用预检后重新打开的 QFile 路径；同句柄拒 reparse / 改写 / 删除并核对实际解析路径。
    ReadHandle file{CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        || GetFileType(file.value) != FILE_TYPE_DISK || info.nFileSizeHigh || info.nFileSizeLow > 2 * 1024 * 1024) return {};
    wchar_t actual[32768]{};
    const auto length = GetFinalPathNameByHandleW(file.value, actual, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= 32768) return {};
    auto resolved = QString::fromWCharArray(actual, static_cast<int>(length));
    if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"))) resolved = QStringLiteral("//") + resolved.mid(8);
    else if (resolved.startsWith(QStringLiteral("\\\\?\\"))) resolved = resolved.mid(4);
    if (QDir::cleanPath(QDir::fromNativeSeparators(resolved)).compare(
        QDir::cleanPath(path), Qt::CaseInsensitive) != 0) return {};
    QByteArray bytes(static_cast<qsizetype>(info.nFileSizeLow), Qt::Uninitialized);
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) return {};
    return bytes;
}
bool PlainDirectory(const QString& path) {
    if (!QDir::isAbsolutePath(path)) return false;
    QString current = QDir::cleanPath(path);
    for (;;) {
        const auto attributes = GetFileAttributesW(reinterpret_cast<const wchar_t*>(current.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}

bool PortableManifest(const QString& install) {
    const auto versionDir = QFileInfo(install).dir().dirName();
    const QRegularExpression layout(QStringLiteral("^[0-9]{8}-([0-9a-f]{7,40})$"));
    const auto match = layout.match(versionDir);
    if (!match.hasMatch() || QFileInfo(install).fileName() != QStringLiteral("Audastra")) return false;
    if (!PlainDirectory(install)) return false;
    const auto manifest = QDir(install).filePath(QStringLiteral("manifest.json"));
    const auto object = QJsonDocument::fromJson(ReadManifest(manifest)).object();
    const auto productName = object.value(QStringLiteral("product")).toString();
    if (object.value(QStringLiteral("format")).toInt() != 1
        || (productName != QStringLiteral("Audastra unsigned author test portable")
            && productName != QStringLiteral("Audastra complete desktop portable candidate"))) return false;
    const auto commit = object.value(QStringLiteral("approved_main_commit")).toString(
        object.value(QStringLiteral("source_commit")).toString());
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(commit).hasMatch()
        || !commit.startsWith(match.captured(1))) return false;
    const auto products = object.contains(QStringLiteral("products"))
        ? object.value(QStringLiteral("products")).toArray() : object.value(QStringLiteral("files")).toArray();
    for (const auto product : products) {
        const auto entry = product.toObject();
        if (entry.value(QStringLiteral("path")).toString() != QStringLiteral("bin/war3-map-editor.exe")) continue;
        if (!PlainDirectory(QDir(install).filePath(QStringLiteral("bin")))) return false;
        const auto exe = QDir(install).filePath(QStringLiteral("bin/war3-map-editor.exe"));
        const auto info = QFileInfo(exe);
        const auto attr = GetFileAttributesW(reinterpret_cast<const wchar_t*>(exe.utf16()));
        return info.isFile()
            && attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)
            && entry.value(QStringLiteral("bytes")).toDouble() == static_cast<double>(info.size())
            && QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                .match(entry.value(QStringLiteral("sha256")).toString()).hasMatch();
    }
    return false;
}
} // namespace

QString LegacyMapBackupReport::Text() const {
    QStringList lines;
    if (!confirmedDirectories.isEmpty()) {
        lines << QObject::tr("旧版备份仍在以下位置（每处已确认至少一份）：");
        for (const auto& directory : confirmedDirectories) lines << QDir::toNativeSeparators(directory);
        lines << QObject::tr("可手工复制到新默认目录，或在上方指定原目录；程序不会自动迁移或删除。");
    } else {
        lines << QObject::tr("已检查的位置未确认旧版备份；这不代表其他安装目录没有备份。");
    }
    if (!unknownDirectories.isEmpty()) {
        lines << QObject::tr("以下位置未能完整核验（占用、失效记录、取消或检查额度等）：");
        for (const auto& directory : unknownDirectories) lines << QDir::toNativeSeparators(directory);
    }
    if (discoveryIncomplete) lines << QObject::tr("旧安装位置发现未完成；可手工选择原 backups 目录检查。");
    lines << QObject::tr("检查范围：当前安装及同一便携版本族，或手工选择的位置；未扫描其他磁盘和安装历史。");
    return lines.join(QLatin1Char('\n'));
}

LegacyMapBackupReport CheckLegacyMapBackups(const QString& installationRoot,
    const QString& explicitBackupDirectory, std::stop_token stop) {
    LegacyMapBackupReport report;
    QStringList roots;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    if (!explicitBackupDirectory.isEmpty()) {
        roots << QDir::cleanPath(explicitBackupDirectory);
    } else if (QDir::isAbsolutePath(installationRoot)) {
        roots << QDir(installationRoot).filePath(QStringLiteral("backups"));
        if (QFileInfo(installationRoot).fileName() == QStringLiteral("Audastra") && PortableManifest(installationRoot)) {
            const QDir family(QFileInfo(installationRoot).dir().filePath(QStringLiteral("..")));
            if (PlainDirectory(family.absolutePath())) {
                QDirIterator entries(family.absolutePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
                int visited = 0;
                while (entries.hasNext() && visited++ < 64 && roots.size() < 8
                    && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    const auto entry = entries.next();
                    const auto sibling = QDir(entry).filePath(QStringLiteral("Audastra"));
                    if (sibling != installationRoot && PortableManifest(sibling))
                        roots << QDir(sibling).filePath(QStringLiteral("backups"));
                }
                report.discoveryIncomplete = entries.hasNext();
            } else report.discoveryIncomplete = true;
        }
    } else report.discoveryIncomplete = true;
    roots.removeDuplicates();
    std::uint64_t remaining = 1024ull * 1024 * 1024;
    for (const auto& directory : roots) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline || remaining == 0) {
            report.unknownDirectories << directory;
            continue;
        }
        MapBackupInspectionOptions options;
        options.stop = stop;
        options.deadline = deadline;
        options.maxReadBytes = remaining;
        const auto result = InspectMapBackupDirectory(std::filesystem::path(directory.toStdWString()), options);
        report.checkedDirectories << directory;
        if (!result.value) {
            report.unknownDirectories << directory;
            continue;
        }
        remaining -= std::min(remaining, result.value->bytesRead);
        if (result.value->state == MapBackupInspectionState::ConfirmedAtLeastOne)
            report.confirmedDirectories << directory;
        else if (result.value->state == MapBackupInspectionState::Unknown)
            report.unknownDirectories << directory;
    }
    return report;
}

LegacyMapBackupCheck::LegacyMapBackupCheck(QString installationRoot, QObject* parent, Scanner scanner)
    : QObject(parent), installationRoot_(std::move(installationRoot)),
      scanner_(scanner ? std::move(scanner) : CheckLegacyMapBackups),
      timer_(new QTimer(this)), text_(tr("旧版备份位置尚未检查")) {
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this]() { Poll(); });
}

LegacyMapBackupCheck::~LegacyMapBackupCheck() { if (task_) task_->stop.request_stop(); }
bool LegacyMapBackupCheck::Busy() const { return static_cast<bool>(task_); }
QString LegacyMapBackupCheck::Text() const { return text_; }
void LegacyMapBackupCheck::SetObserver(std::function<void(QString, bool)> observer) {
    observer_ = std::move(observer);
    Notify();
}
void LegacyMapBackupCheck::Notify() {
    const auto callback = observer_;
    const auto text = text_;
    const auto busy = Busy();
    // callback 可重入替换观察者或销毁服务；调用后不再访问 this。
    if (callback) callback(text, busy);
}
void LegacyMapBackupCheck::Start(const QString& directory) {
    if (task_) {
        pending_ = directory;
        task_->stop.request_stop();
        return;
    }
    Launch(directory);
}
void LegacyMapBackupCheck::Cancel() {
    pending_.reset();
    if (task_) task_->stop.request_stop();
}
void LegacyMapBackupCheck::Launch(QString directory) {
    task_ = std::make_shared<Task>();
    auto task = task_;
    const auto root = installationRoot_;
    const auto scanner = scanner_;
    text_ = tr("正在后台只读检查旧版备份位置…");
    try {
        std::thread([task, root, directory = std::move(directory), scanner]() {
            LegacyMapBackupReport report;
            try { report = scanner(root, directory, task->stop.get_token()); }
            catch (...) { report.discoveryIncomplete = true; }
            const std::lock_guard lock(task->mutex);
            task->result = std::move(report);
        }).detach();
        timer_->start();
    } catch (...) {
        task_.reset();
        text_ = tr("旧版备份位置未能启动检查，可稍后手工重试。");
    }
    Notify();
}
void LegacyMapBackupCheck::Poll() {
    if (!task_) return;
    std::optional<LegacyMapBackupReport> report;
    {
        const std::lock_guard lock(task_->mutex);
        if (task_->result) report = std::move(task_->result);
    }
    if (!report) return;
    task_.reset();
    timer_->stop();
    if (pending_) {
        const auto directory = std::move(*pending_);
        pending_.reset();
        Launch(directory);
        return;
    }
    text_ = report->Text();
    Notify();
}
} // namespace War3Map::QtProject
