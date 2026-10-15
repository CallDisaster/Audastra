#include "UpdateClient.h"
#include "UpdateFixtures.h"
#include "War3MapCore/Update/UpdateInstaller.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace War3Map::QtUpdate;
using namespace UpdateFixtures;

namespace {
constexpr char kTestKey[] = "5e423033044f56a13a686799487bcbfa63dd1e39204cec27dd39a26daa615628";
constexpr char kTestCommit[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
struct Executables { QString first, second, third, updater; };
Executables executables;

QString ErrorText(const std::vector<War3Map::Diagnostic>& diagnostics) {
    QStringList lines;
    for (const auto& diagnostic : diagnostics)
        lines.push_back(QString::fromStdString(diagnostic.code + ": " + diagnostic.message));
    return lines.join(QLatin1Char('\n'));
}
bool Write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 30000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    return condition();
}
QString ActualDirectory(const QString& path) {
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::array<wchar_t, 32768> bytes{};
    const auto count = GetFinalPathNameByHandleW(handle, bytes.data(), static_cast<DWORD>(bytes.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    std::wstring result(bytes.data(), count < bytes.size() ? count : 0);
    if (!count || count >= bytes.size() || !result.starts_with(L"\\\\?\\") || result.starts_with(L"\\\\?\\UNC\\")) return {};
    return QDir::fromNativeSeparators(QString::fromStdWString(result.substr(4)));
}
std::optional<BY_HANDLE_FILE_INFORMATION> DirectoryIdentity(const QString& path) {
    const auto handle = CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool good = GetFileInformationByHandle(handle, &identity)
        && (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    return good ? std::optional{identity} : std::nullopt;
}
bool SameDirectory(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

// 默认保留失败现场。只有自然进程都结束、目录身份/内容回读成功后才允许Qt清理自己创建的树。
class OwnedTemp final {
public:
    OwnedTemp() {
        const auto parent = ActualDirectory(QDir::tempPath());
        if (parent.isEmpty()) return;
        directory_ = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral("AudastraUpdateE2E-XXXXXX")));
        directory_->setAutoRemove(false);
        if (!directory_->isValid() || ActualDirectory(directory_->path()) != directory_->path()) return;
        identity_ = DirectoryIdentity(directory_->path());
    }
    ~OwnedTemp() {
        if (!directory_) return;
        if (directory_->autoRemove() && !IsOwned()) directory_->setAutoRemove(false);
        if (!directory_->autoRemove()) qWarning().noquote() << "Original update E2E scene retained:" << directory_->path();
    }
    bool IsOwned() const {
        if (!directory_ || !directory_->isValid() || !identity_) return false;
        const auto now = DirectoryIdentity(directory_->path());
        return now && SameDirectory(*identity_, *now) && ActualDirectory(directory_->path()) == directory_->path();
    }
    QString Path(const QString& relative = {}) const { return directory_ ? QDir(directory_->path()).filePath(relative) : QString{}; }
    bool AllowCleanup() {
        if (!IsOwned()) return false;
        QDirIterator iterator(Path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const auto flags = GetFileAttributesW(QDir::toNativeSeparators(iterator.next()).toStdWString().c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
        directory_->setAutoRemove(true); return true;
    }
private:
    std::unique_ptr<QTemporaryDir> directory_;
    std::optional<BY_HANDLE_FILE_INFORMATION> identity_;
};

std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result.push_back(L'\\');
        result.push_back(c); slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'"'); return result;
}

// 使用原生HANDLE而非QProcess所有权：超时只关测试句柄，绝不由析构结束仍活的进程。
class NaturalProcess final {
public:
    ~NaturalProcess() { if (process_) CloseHandle(process_); }
    NaturalProcess() = default;
    NaturalProcess(const NaturalProcess&) = delete;
    NaturalProcess& operator=(const NaturalProcess&) = delete;
    bool Start(const QString& executable, const QStringList& arguments, const QString& directory, const QString& prefix) {
        if (process_) return false;
        output_ = prefix + QStringLiteral(".stdout.json"); errors_ = prefix + QStringLiteral(".stderr.log");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto output = CreateFileW(QDir::toNativeSeparators(output_).toStdWString().c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto error = CreateFileW(QDir::toNativeSeparators(errors_).toStdWString().c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        const auto closeStreams = [&] {
            for (const auto handle : {output, error, input}) if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        };
        if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) { closeStreams(); return false; }
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<std::byte> bytes(size); auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) { closeStreams(); return false; }
        const std::array<HANDLE, 3> inherited{input, output, error};
        const bool limited = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr);
        auto command = Quote(QDir::toNativeSeparators(executable).toStdWString());
        for (const auto& argument : arguments) { command += L' '; command += Quote(argument.toStdWString()); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = output; startup.StartupInfo.hStdError = error;
        PROCESS_INFORMATION child{};
        const bool started = limited && CreateProcessW(QDir::toNativeSeparators(executable).toStdWString().c_str(), command.data(),
            nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
            QDir::toNativeSeparators(directory).toStdWString().c_str(), &startup.StartupInfo, &child);
        DeleteProcThreadAttributeList(attributes); closeStreams();
        if (!started) return false;
        CloseHandle(child.hThread); process_ = child.hProcess; identity_.processId = child.dwProcessId;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process_, &creation, &exit, &kernel, &user)) return false;
        identity_.creationTime100ns = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        return true;
    }
    bool Finished() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0; }
    bool Wait(int timeoutMs = 30000) const { return WaitFor([this] { return Finished(); }, timeoutMs); }
    DWORD ExitCode() const { DWORD code = STILL_ACTIVE; if (process_) GetExitCodeProcess(process_, &code); return code; }
    QJsonObject Output() const {
        if (!Finished() || QFileInfo(output_).size() > 1024 * 1024) return {};
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(Read(output_), &parse);
        return parse.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
    }
    QString Debug() const {
        return QStringLiteral("PID=%1 created=%2 exit=%3 stdout=%4 stderr=%5")
            .arg(identity_.processId).arg(identity_.creationTime100ns).arg(ExitCode()).arg(output_, errors_);
    }
    const War3Map::Update::UpdateProcessIdentity& Identity() const { return identity_; }
private:
    HANDLE process_ = nullptr;
    War3Map::Update::UpdateProcessIdentity identity_;
    QString output_, errors_;
};

class ExitEvent final {
public:
    ExitEvent() {
        name_ = QStringLiteral("AudastraOriginalUpdateE2E-%1-%2").arg(GetCurrentProcessId()).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        event_ = CreateEventW(nullptr, TRUE, FALSE, name_.toStdWString().c_str());
        if (event_ && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(event_); event_ = nullptr; }
    }
    ~ExitEvent() { if (event_) CloseHandle(event_); }
    bool IsValid() const { return event_ != nullptr; }
    QString Name() const { return name_; }
    bool Signal() const { return event_ && SetEvent(event_); }
private:
    QString name_;
    HANDLE event_ = nullptr;
};

QMap<QString, QByteArray> ProgramFiles(const QString& executable, const QString& version) {
    QMap<QString, QByteArray> files{{QStringLiteral("bin/war3-map-editor.exe"), Read(executable)},
        {QStringLiteral("program/version.txt"), QByteArray("original E2E program ") + version.toUtf8()}};
    const QDir original(QFileInfo(executable).absolutePath());
    for (const auto* name : {"msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
            "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll"}) {
        const auto path = original.filePath(QString::fromLatin1(name));
        if (QFileInfo::exists(path)) files.insert(QStringLiteral("bin/") + QString::fromLatin1(name), Read(path));
    }
    return files;
}
Bundle SignedBundle(const QMap<QString, QByteArray>& files, const QString& version, const QString& minimum) {
    Bundle bundle; bundle.package = Zip(files); QJsonArray rows;
    for (auto i = files.begin(); i != files.end(); ++i)
        rows.append(QJsonObject{{"path", i.key()}, {"bytes", qint64(i.value().size())}, {"sha256", QString::fromLatin1(Hash(i.value()))}});
    const QJsonObject manifest{{"format", "audastra-update"}, {"schema", 1}, {"product", "Audastra"}, {"platform", "windows-x64"},
        {"version", version}, {"minimum_direct_upgrade", minimum}, {"source_commit", QString::fromLatin1(kTestCommit)},
        {"zip", QJsonObject{{"filename", "fixture.zip"}, {"bytes", qint64(bundle.package.size())}, {"sha256", QString::fromLatin1(Hash(bundle.package))}}},
        {"release_notes", "原创回环端到端更新；不访问作者安装目录"}, {"files", rows}};
    bundle.manifest = QJsonDocument(manifest).toJson(QJsonDocument::Indented) + "\r\n";
    bundle.signature = bundle.signer.Sign(bundle.manifest); return bundle;
}
bool WriteProgram(const QString& install, const QMap<QString, QByteArray>& files) {
    for (auto i = files.begin(); i != files.end(); ++i) if (i.value().isEmpty() || !Write(QDir(install).filePath(i.key()), i.value())) return false;
    return true;
}
bool ExactProgram(const QString& install, const QMap<QString, QByteArray>& files) {
    for (auto i = files.begin(); i != files.end(); ++i) {
        const auto actual = Read(QDir(install).filePath(i.key()));
        if (actual.size() != i.value().size() || Hash(actual) != Hash(i.value())) return false;
    }
    return true;
}
QJsonObject RecordFor(const QString& receipts, const QString& kind, const QString& version) {
    for (const auto& file : QDir(receipts).entryList({QStringLiteral("fixture-*.json")}, QDir::Files)) {
        const auto record = QJsonDocument::fromJson(Read(QDir(receipts).filePath(file))).object();
        if (record.value("kind") == kind && record.value("version") == version) return record;
    }
    return {};
}
bool RecordExited(const QJsonObject& record) {
    const auto pid = record.value("pid").toInteger(); const auto created = record.value("creation_time_100ns").toInteger();
    if (pid <= 0 || pid > MAXDWORD || created <= 0) return false;
    const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool known = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    const auto actual = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    const bool gone = known && (actual != static_cast<std::uint64_t>(created) || WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
    CloseHandle(process); return gone;
}
bool UserFilesIntact(const QString& install, const QMap<QString, QByteArray>& userFiles) {
    return ExactProgram(install, userFiles) && QFileInfo(QDir(install).filePath(QStringLiteral("user/empty-folder"))).isDir();
}
void CaptureReceipt(const QString& name, const QJsonObject& receipt) {
    const auto output = qEnvironmentVariable("FORGE_QT_UPDATE_E2E_CAPTURE_DIR");
    if (!output.isEmpty() && Write(QDir(output).filePath(name + QStringLiteral(".json")), QJsonDocument(receipt).toJson(QJsonDocument::Indented)))
        qInfo().noquote() << "Original update E2E receipt:" << QDir(output).filePath(name + QStringLiteral(".json"));
}

struct UpgradeResult { QJsonObject receipt; QString error; PreparedUpdate prepared; };
UpgradeResult Upgrade(OwnedTemp& owner, const QString& current, const QString& targetVersion, const Bundle& target,
    const Bundle& expectedBaseline, const QString& tag, bool failHealth = false) {
    UpgradeResult result;
    const auto fail = [&](const QString& error) { result.error = error; return result; };
    if (!owner.IsOwned()) return fail(QStringLiteral("独占TEMP目录身份已变化。"));
    const auto install = owner.Path("install"); const auto receipts = owner.Path("receipts");
    Server server; if (!server.Start()) return fail(QStringLiteral("原创回环server无法启动。")); server.Publish(target, targetVersion);
    QSettings settings(owner.Path(tag + ".ini"), QSettings::IniFormat);
    auto client = std::make_unique<UpdateClient>(server.Source(target, owner.Path("downloads"), install, current), settings);
    QSignalSpy ready(client.get(), &UpdateClient::PackagePrepared); QSignalSpy progress(client.get(), &UpdateClient::DownloadProgress);
    if (!client->Check() || !WaitFor([&] { return !client->IsBusy(); }) || client->Status().state != UpdateState::Available)
        return fail(client->Status().errorCode + QStringLiteral(": ") + client->Status().message);
    if (server.requests.size() != 1 || !client->Status().canDownload || client->Status().availableVersion != targetVersion)
        return fail(QStringLiteral("检查阶段未停在实际Available/单JSON请求。"));
    if (!client->Download() || !WaitFor([&] { return !client->IsBusy(); }) || client->Status().state != UpdateState::Ready || !client->Prepared())
        return fail(client->Status().errorCode + QStringLiteral(": ") + client->Status().message);
    result.prepared = *client->Prepared();
    if (ready.count() != 1 || progress.count() < 2 || server.requests.size() != 4
        || Read(result.prepared.manifestPath) != target.manifest || Read(result.prepared.signaturePath) != target.signature
        || Read(result.prepared.packagePath) != target.package || result.prepared.manifestSha256.toLatin1() != Hash(target.manifest))
        return fail(QStringLiteral("Qt准备回执、真实下载请求/进度或原字节身份不符。"));
    for (const auto& request : server.requests) if (request.toLower().contains("authorization:") || request.toLower().contains("cookie:"))
        return fail(QStringLiteral("原创检查请求意外带认证或Cookie。"));

    const auto key = War3Map::Update::ParseTrustedUpdateKeyHex(kTestKey);
    const auto baseline = key.Succeeded() ? War3Map::Update::DiscoverInstalledUpdateBaseline(
        std::filesystem::path(install.toStdWString()), current.toStdString(), *key.value) : War3Map::Result<War3Map::Update::UpdateBaselineLocation>{};
    if (!baseline.Succeeded()) return fail(ErrorText(baseline.diagnostics));
    const auto baselineManifest = QString::fromStdWString(baseline.value->manifestPath.native());
    const auto baselineSignature = QString::fromStdWString(baseline.value->signaturePath.native());
    if (Read(baselineManifest) != expectedBaseline.manifest || Read(baselineSignature) != expectedBaseline.signature
        || QString::fromStdString(baseline.value->manifestSha256).toLatin1() != Hash(expectedBaseline.manifest))
        return fail(QStringLiteral("当前签名baseline不等于预期精确版本和原字节。"));
    const auto rawPath = owner.Path(tag + "-baseline.json"), signaturePath = owner.Path(tag + "-baseline.sig");
    if (!Write(rawPath, expectedBaseline.manifest) || !Write(signaturePath, expectedBaseline.signature))
        return fail(QStringLiteral("树外baseline写入失败。"));

    ExitEvent exit; if (!exit.IsValid()) return fail(QStringLiteral("原创父进程自然退出event无法创建。"));
    NaturalProcess parent;
    if (!parent.Start(QDir(install).filePath("bin/war3-map-editor.exe"),
        {QStringLiteral("--fixture-parent-exit-event"), exit.Name(), QStringLiteral("--fixture-receipts"), receipts}, install, owner.Path(tag + "-parent")))
        return fail(QStringLiteral("签名旧版父进程夹具无法启动。"));
    const auto before = Read(QDir(install).filePath("bin/war3-map-editor.exe"));
    QStringList arguments{QStringLiteral("--install-dir"), install, QStringLiteral("--package"), result.prepared.packagePath,
        QStringLiteral("--manifest"), result.prepared.manifestPath, QStringLiteral("--signature"), result.prepared.signaturePath,
        QStringLiteral("--current-version"), current, QStringLiteral("--baseline-manifest"), rawPath, QStringLiteral("--baseline-signature"), signaturePath,
        QStringLiteral("--parent-pid"), QString::number(parent.Identity().processId), QStringLiteral("--parent-created"), QString::number(parent.Identity().creationTime100ns),
        QStringLiteral("--parent-timeout-ms"), QStringLiteral("10000"), QStringLiteral("--health-timeout-ms"), QStringLiteral("4000"),
        QStringLiteral("--exe"), QStringLiteral("bin/war3-map-editor.exe"), QStringLiteral("--startup-argument"), QStringLiteral("--fixture-receipts"),
        QStringLiteral("--startup-argument"), receipts};
    if (failHealth) arguments << QStringLiteral("--startup-argument") << QStringLiteral("--fixture-fail");
    NaturalProcess helper;
    if (!helper.Start(executables.updater, arguments, owner.Path(), owner.Path(tag + "-helper"))) return fail(QStringLiteral("同源树外test helper无法启动。"));
    QString error;
    if (!client->RetainPreparedForInstaller(result.prepared, &error)) return fail(error);
    client.reset(); // 真实Qt服务释放后，独立helper仍读取已保留的原输入。
    if (Read(result.prepared.packagePath) != target.package) return fail(QStringLiteral("Qt退出后更新包未保留。"));
    QTest::qWait(100);
    if (parent.Finished() || helper.Finished() || Read(QDir(install).filePath("bin/war3-map-editor.exe")) != before)
        return fail(QStringLiteral("父进程未退出时安装器提前结束或改变安装树。 ") + helper.Debug());
    if (!exit.Signal() || !parent.Wait(12000) || parent.ExitCode() != 0) return fail(QStringLiteral("父进程未自然退出。 ") + parent.Debug());
    if (!helper.Wait(20000)) return fail(QStringLiteral("helper仍活；保留TEMP，不结束进程。 ") + helper.Debug());
    result.receipt = helper.Output();
    result.receipt["helper_exit_code"] = qint64(helper.ExitCode());
    result.receipt["helper_pid"] = qint64(helper.Identity().processId);
    result.receipt["helper_creation_time_100ns"] = qint64(helper.Identity().creationTime100ns);
    result.receipt["parent_pid"] = qint64(parent.Identity().processId);
    result.receipt["parent_creation_time_100ns"] = qint64(parent.Identity().creationTime100ns);
    result.receipt["qt_manifest_sha256"] = result.prepared.manifestSha256;
    result.receipt["qt_package_sha256"] = result.prepared.packageSha256;
    if (result.receipt.value("outcome").toString().isEmpty() || !owner.IsOwned()) return fail(QStringLiteral("helper无完整回执或TEMP身份变化。 ") + helper.Debug());
    return result;
}
} // namespace

class UpdateEndToEndTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void SignedLoopbackUpgradesTwiceAndTheNextStartKeepsOneBackup();
    void FailingNewChildReturnsNaturallyAndAutomaticallyRollsBack();
};

void UpdateEndToEndTests::initTestCase() {
    QCOMPARE(Signer().KeyHex(), QString::fromLatin1(kTestKey));
    for (const auto& path : {executables.first, executables.second, executables.third, executables.updater}) {
        QVERIFY2(QFileInfo(path).isAbsolute() && QFileInfo(path).isFile() && !Read(path).isEmpty(), qPrintable(path));
    }
}

void UpdateEndToEndTests::SignedLoopbackUpgradesTwiceAndTheNextStartKeepsOneBackup() {
    OwnedTemp owner; QVERIFY(owner.IsOwned());
    const auto install = owner.Path("install"), receipts = owner.Path("receipts"); QVERIFY(QDir().mkpath(receipts));
    const auto firstFiles = ProgramFiles(executables.first, "0.1.0"), secondFiles = ProgramFiles(executables.second, "0.2.0"), thirdFiles = ProgramFiles(executables.third, "0.3.0");
    const auto first = SignedBundle(firstFiles, "0.1.0", "0.0.0"), second = SignedBundle(secondFiles, "0.2.0", "0.1.0"), third = SignedBundle(thirdFiles, "0.3.0", "0.1.0");
    QVERIFY(WriteProgram(install, firstFiles)); QVERIFY(Write(QDir(install).filePath("update-manifest.json"), first.manifest));
    QVERIFY(Write(QDir(install).filePath("update-manifest.json.sig"), first.signature));
    const QMap<QString, QByteArray> userFiles{{"user/settings.ini", "original user preference\r\n"}, {"user/notes.txt", "原创用户内容\n"},
        {"logs/current.log", "original local log"}, {"feedback/queue.bin", QByteArray::fromHex("001122ffaabb")}};
    QVERIFY(WriteProgram(install, userFiles)); QVERIFY(QDir().mkpath(QDir(install).filePath("user/empty-folder")));
    QVERIFY(owner.IsOwned()); QVERIFY(ExactProgram(install, firstFiles));

    const auto one = Upgrade(owner, "0.1.0", "0.2.0", second, first, "upgrade-one");
    QVERIFY2(one.error.isEmpty(), qPrintable(one.error)); QCOMPARE(one.receipt.value("outcome").toString(), QStringLiteral("committed"));
    QCOMPARE(one.receipt.value("helper_exit_code").toInteger(), 0); QVERIFY(one.receipt.value("health_verified").toBool());
    QVERIFY(one.receipt.value("user_files_preserved").toBool()); QVERIFY(ExactProgram(install, secondFiles)); QVERIFY(UserFilesIntact(install, userFiles));
    QVERIFY(WaitFor([&] { return RecordExited(RecordFor(receipts, "health-reply", "0.2.0")); }));
    const auto firstBackup = one.receipt.value("backup_directory").toString(); QVERIFY(ExactProgram(firstBackup, firstFiles)); QVERIFY(UserFilesIntact(firstBackup, userFiles));
    CaptureReceipt("localhost-upgrade-one", one.receipt);

    const auto two = Upgrade(owner, "0.2.0", "0.3.0", third, second, "upgrade-two");
    QVERIFY2(two.error.isEmpty(), qPrintable(two.error)); QCOMPARE(two.receipt.value("outcome").toString(), QStringLiteral("committed"));
    QCOMPARE(two.receipt.value("helper_exit_code").toInteger(), 0); QVERIFY(two.receipt.value("health_verified").toBool());
    QVERIFY(two.receipt.value("user_files_preserved").toBool()); QVERIFY(ExactProgram(install, thirdFiles)); QVERIFY(UserFilesIntact(install, userFiles));
    QVERIFY(WaitFor([&] { return RecordExited(RecordFor(receipts, "health-reply", "0.3.0")); }));
    const auto latestBackup = two.receipt.value("backup_directory").toString(); QVERIFY(ExactProgram(latestBackup, secondFiles)); QVERIFY(UserFilesIntact(latestBackup, userFiles));
    QVERIFY(QFileInfo(firstBackup).isDir()); QVERIFY(QFileInfo(latestBackup).isDir()); CaptureReceipt("localhost-upgrade-two", two.receipt);

    NaturalProcess nextStart;
    QVERIFY(nextStart.Start(QDir(install).filePath("bin/war3-map-editor.exe"), {"--fixture-cleanup", "--fixture-receipts", receipts}, install, owner.Path("next-start")));
    QVERIFY2(nextStart.Wait(), qPrintable(nextStart.Debug())); QCOMPARE(nextStart.ExitCode(), DWORD(0)); const auto cleanup = nextStart.Output();
    QCOMPARE(cleanup.value("version").toString(), QStringLiteral("0.3.0")); QVERIFY(cleanup.value("cleanup_succeeded").toBool());
    QCOMPARE(cleanup.value("removed_backups").toInteger(), 1); QCOMPARE(cleanup.value("retained_backups").toInteger(), 1);
    QCOMPARE(cleanup.value("retained_unknown").toInteger(), 0); QVERIFY(!QFileInfo::exists(firstBackup)); QVERIFY(QFileInfo(latestBackup).isDir());
    QVERIFY(ExactProgram(latestBackup, secondFiles)); QVERIFY(UserFilesIntact(latestBackup, userFiles));
    QVERIFY(owner.IsOwned()); QVERIFY(ExactProgram(install, thirdFiles)); QVERIFY(UserFilesIntact(install, userFiles));
    CaptureReceipt("localhost-next-start-cleanup", cleanup); QVERIFY(owner.AllowCleanup());
}

void UpdateEndToEndTests::FailingNewChildReturnsNaturallyAndAutomaticallyRollsBack() {
    OwnedTemp owner; QVERIFY(owner.IsOwned());
    const auto install = owner.Path("install"), receipts = owner.Path("receipts"); QVERIFY(QDir().mkpath(receipts));
    const auto firstFiles = ProgramFiles(executables.first, "0.1.0"), secondFiles = ProgramFiles(executables.second, "0.2.0");
    const auto first = SignedBundle(firstFiles, "0.1.0", "0.0.0"), second = SignedBundle(secondFiles, "0.2.0", "0.1.0");
    QVERIFY(WriteProgram(install, firstFiles)); QVERIFY(Write(QDir(install).filePath("update-manifest.json"), first.manifest));
    QVERIFY(Write(QDir(install).filePath("update-manifest.json.sig"), first.signature));
    const QMap<QString, QByteArray> userFiles{{"user/settings.ini", "keep user preference"}, {"user/notes.txt", "keep original user notes"}};
    QVERIFY(WriteProgram(install, userFiles)); QVERIFY(QDir().mkpath(QDir(install).filePath("user/empty-folder")));
    const auto result = Upgrade(owner, "0.1.0", "0.2.0", second, first, "failed-upgrade", true);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QCOMPARE(result.receipt.value("outcome").toString(), QStringLiteral("rolled-back"));
    QCOMPARE(result.receipt.value("helper_exit_code").toInteger(), 1); QVERIFY(!result.receipt.value("health_verified").toBool());
    QVERIFY(result.receipt.value("old_version_restarted").toBool()); QVERIFY(!result.receipt.value("reason_code").toString().isEmpty());
    const auto failedChild = RecordFor(receipts, "natural-health-failure", "0.2.0"); QVERIFY(!failedChild.isEmpty()); QCOMPARE(failedChild.value("exit_code").toInteger(), 4);
    QVERIFY(WaitFor([&] { return RecordExited(failedChild); }));
    QVERIFY(WaitFor([&] { const auto old = RecordFor(receipts, "ordinary-start", "0.1.0"); return old.value("rollback_reason_present").toBool() && RecordExited(old); }));
    QVERIFY(owner.IsOwned()); QVERIFY(ExactProgram(install, firstFiles)); QVERIFY(UserFilesIntact(install, userFiles));
    QCOMPARE(Read(QDir(install).filePath("update-manifest.json")), first.manifest); QCOMPARE(Read(QDir(install).filePath("update-manifest.json.sig")), first.signature);
    QVERIFY(ExactProgram(result.receipt.value("failed_directory").toString(), secondFiles));
    CaptureReceipt("localhost-natural-failure-rollback", result.receipt); QVERIFY(owner.AllowCleanup());
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QList<QByteArray> arguments; arguments.push_back(QByteArray(argv[0]));
    const QMap<QByteArray, QString*> paths{{"--fixture-v1", &executables.first}, {"--fixture-v2", &executables.second},
        {"--fixture-v3", &executables.third}, {"--test-updater", &executables.updater}};
    for (int index = 1; index < argc; ++index) {
        const QByteArray name(argv[index]);
        if (paths.contains(name)) {
            if (index + 1 >= argc || !paths[name]->isEmpty()) return 2;
            *paths[name] = QString::fromLocal8Bit(argv[++index]);
        } else arguments.push_back(name);
    }
    std::vector<char*> forwarded; for (auto& argument : arguments) forwarded.push_back(argument.data());
    UpdateEndToEndTests tests; return QTest::qExec(&tests, static_cast<int>(forwarded.size()), forwarded.data());
}

#include "UpdateEndToEndTests.moc"
