#include "CredentialStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace War3Map::Agent {
namespace {

const QByteArray kHeader("FORGE-DPAPI-1\n");

bool Fail(QString* error, const QString& message) {
    if (error != nullptr) *error = message;
    return false;
}

} // namespace

CredentialStore::CredentialStore(QString directory)
    : directory_(directory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("agent/credentials")) : std::move(directory)) {}

QString CredentialStore::Directory() const { return directory_; }

QString CredentialStore::Path(const QString& reference) const {
    return QDir(directory_).filePath(QString::fromLatin1(
        QCryptographicHash::hash(reference.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".bin"));
}

bool CredentialStore::Exists(const QString& reference) const {
    return !reference.isEmpty() && reference.size() <= 16384 && QFileInfo::exists(Path(reference));
}

bool CredentialStore::Write(const QString& reference, const QString& secret, QString* error) const {
    if (error != nullptr) error->clear();
    if (reference.isEmpty() || reference.size() > 16384 || secret.isEmpty() || secret.size() > 32768)
        return Fail(error, QStringLiteral("密钥引用或密钥长度无效。"));
#ifdef Q_OS_WIN
    QByteArray plain = secret.toUtf8();
    QByteArray entropy = QCryptographicHash::hash(reference.toUtf8(), QCryptographicHash::Sha256);
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB salt{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    const BOOL protectedData = CryptProtectData(&input, L"FORGE provider credential", &salt, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output);
    SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
    if (protectedData == FALSE) return Fail(error, QStringLiteral("Windows 无法加密密钥（DPAPI 错误 %1）。").arg(GetLastError()));
    QByteArray encrypted = kHeader;
    encrypted.append(reinterpret_cast<const char*>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    if (!QDir().mkpath(directory_)) return Fail(error, QStringLiteral("无法创建本机密钥目录。"));
    QSaveFile file(Path(reference));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() || !file.commit())
        return Fail(error, QStringLiteral("无法原子保存加密密钥。"));
    return true;
#else
    Q_UNUSED(secret)
    return Fail(error, QStringLiteral("当前平台尚未接入安全凭据存储；不会以明文保存密钥。"));
#endif
}

QString CredentialStore::Read(const QString& reference, QString* error) const {
    if (error != nullptr) error->clear();
    if (reference.isEmpty()) return {};
    if (reference.size() > 16384) { Fail(error, QStringLiteral("密钥引用过长。")); return {}; }
    QFile file(Path(reference));
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024) {
        Fail(error, QStringLiteral("无法读取本机密钥，或加密文件大小无效。")); return {};
    }
    QByteArray encrypted = file.readAll();
    if (!encrypted.startsWith(kHeader)) { Fail(error, QStringLiteral("加密密钥格式无效。")); return {}; }
#ifdef Q_OS_WIN
    encrypted.remove(0, kHeader.size());
    QByteArray entropy = QCryptographicHash::hash(reference.toUtf8(), QCryptographicHash::Sha256);
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB salt{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        Fail(error, QStringLiteral("当前 Windows 用户无法解密该密钥，请重新填写。")); return {};
    }
    const QString secret = QString::fromUtf8(reinterpret_cast<const char*>(output.pbData), static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return secret;
#else
    Fail(error, QStringLiteral("当前平台尚未接入安全凭据存储。")); return {};
#endif
}

bool CredentialStore::Remove(const QString& reference, QString* error) const {
    if (error != nullptr) error->clear();
    if (reference.isEmpty()) return true;
    if (reference.size() > 16384) return Fail(error, QStringLiteral("密钥引用过长。"));
    const auto path = Path(reference);
    return !QFileInfo::exists(path) || QFile::remove(path) || Fail(error, QStringLiteral("无法移除本机加密密钥。"));
}

} // namespace War3Map::Agent
