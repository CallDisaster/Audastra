#pragma once

#include "UpdateClient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <miniz.h>
#include <sodium.h>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace UpdateFixtures {

inline QByteArray Hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
inline QByteArray Read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
inline QByteArray Http(const QByteArray& body, int code = 200, const QByteArray& extra = {}) {
    return QByteArray("HTTP/1.1 ") + QByteArray::number(code) + " Fixture\r\nContent-Length: " + QByteArray::number(body.size())
        + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

// 本头仅供原创Qt更新测试编译；seed不是作者密钥，不进入产品BuildInfo或发布包。
struct Signer {
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> publicKey{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    Signer() {
        if (sodium_init() < 0) throw std::runtime_error("fixture sodium init failed");
        std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(0x32 + i);
        if (crypto_sign_seed_keypair(publicKey.data(), secret.data(), seed.data()) != 0) throw std::runtime_error("fixture signing key failed");
    }
    QString KeyHex() const { return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(publicKey.data()), publicKey.size()).toHex()); }
    QByteArray Sign(const QByteArray& raw) const {
        QByteArray result(crypto_sign_BYTES, Qt::Uninitialized); unsigned long long length = 0;
        if (crypto_sign_detached(reinterpret_cast<unsigned char*>(result.data()), &length,
            reinterpret_cast<const unsigned char*>(raw.constData()), raw.size(), secret.data()) != 0 || length != crypto_sign_BYTES)
            throw std::runtime_error("fixture signature failed");
        return result;
    }
};

inline QByteArray Zip(const QMap<QString, QByteArray>& files) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0)) throw std::runtime_error("fixture zip init failed");
    for (auto i = files.begin(); i != files.end(); ++i) {
        const auto path = i.key().toUtf8();
        if (!mz_zip_writer_add_mem(&archive, path.constData(), i.value().constData(), i.value().size(), MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&archive); throw std::runtime_error("fixture zip member failed");
        }
    }
    void* memory = nullptr; size_t bytes = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &memory, &bytes)) { mz_zip_writer_end(&archive); throw std::runtime_error("fixture zip finalize failed"); }
    const QByteArray result(static_cast<const char*>(memory), static_cast<qsizetype>(bytes));
    mz_free(memory); mz_zip_writer_end(&archive); return result;
}

struct Bundle {
    Signer signer;
    QMap<QString, QByteArray> files;
    QByteArray package;
    QByteArray manifest;
    QByteArray signature;
    explicit Bundle(const QString& version = QStringLiteral("0.2.0"), const QString& minimum = QStringLiteral("0.1.0")) {
        files = {{QStringLiteral("war3-map-editor.exe"), QByteArray("original fixture executable data")},
            {QStringLiteral("plugins/fixture.txt"), QByteArray("original fixture plugin data")}};
        package = Zip(files); QJsonArray rows;
        for (auto i = files.begin(); i != files.end(); ++i) rows.append(QJsonObject{{"path", i.key()}, {"bytes", qint64(i.value().size())}, {"sha256", QString::fromLatin1(Hash(i.value()))}});
        const QJsonObject object{{"format", "audastra-update"}, {"schema", 1}, {"product", "Audastra"}, {"platform", "windows-x64"},
            {"version", version}, {"minimum_direct_upgrade", minimum}, {"source_commit", QString(40, QLatin1Char('b'))},
            {"zip", QJsonObject{{"filename", "fixture.zip"}, {"bytes", qint64(package.size())}, {"sha256", QString::fromLatin1(Hash(package))}}},
            {"release_notes", "原创本地更新测试\r\n保留原始清单字节"}, {"files", rows}};
        // 缩进和末尾CRLF进入签名；产品不能重序列化后验签。
        manifest = QJsonDocument(object).toJson(QJsonDocument::Indented) + "\r\n"; signature = signer.Sign(manifest);
    }
};

inline bool WriteFixtureFile(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// 当前安装和远端新包是两份独立的原创签名输入；客户端必须实际核验磁盘文件，
// 没有“测试时默认为可信”的开关。故障用例在生成后修改此目录或显式传空目录。
inline QString CreateSignedInstallation(const QString& root, const QString& version = QStringLiteral("0.1.0")) {
    const Bundle baseline(version, QStringLiteral("0.0.0"));
    for (auto file = baseline.files.begin(); file != baseline.files.end(); ++file)
        if (!WriteFixtureFile(QDir(root).filePath(file.key()), file.value()))
            throw std::runtime_error("fixture installation file failed");
    if (!WriteFixtureFile(QDir(root).filePath(QStringLiteral("update-manifest.json")), baseline.manifest)
        || !WriteFixtureFile(QDir(root).filePath(QStringLiteral("update-manifest.json.sig")), baseline.signature))
        throw std::runtime_error("fixture installation baseline failed");
#ifdef Q_OS_WIN
    // TEMP 可能含 8.3 名；仅展开原创目录的短名，原更新路径/reparse 校验仍然照常执行。
    const auto native = QDir::toNativeSeparators(QDir(root).absolutePath()).toStdWString();
    const auto size = GetLongPathNameW(native.c_str(), nullptr, 0);
    if (!size || size > 32768) throw std::runtime_error("fixture installation path failed");
    std::wstring expanded(size, L'\0');
    const auto length = GetLongPathNameW(native.c_str(), expanded.data(), size);
    if (!length || length >= size) throw std::runtime_error("fixture installation path failed");
    expanded.resize(length);
    return QDir::fromNativeSeparators(QString::fromStdWString(expanded));
#else
    return QDir(root).canonicalPath();
#endif
}

class Server final : public QObject {
public:
    Server() {
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (listener.hasPendingConnections()) {
                auto* socket = listener.nextPendingConnection(); auto input = std::make_shared<QByteArray>(); auto done = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input, done] {
                    input->append(socket->readAll()); if (*done || !input->contains("\r\n\r\n")) return;
                    *done = true; requests.push_back(*input); const auto parts = input->split(' ');
                    const QByteArray target = parts.size() >= 2 ? parts[1] : QByteArray{};
                    if (responder) responder(socket, target);
                    else { socket->write(Http(routes.value(target, QByteArray("not found")), routes.contains(target) ? 200 : 404)); socket->disconnectFromHost(); }
                });
            }
        });
    }
    bool Start() { return listener.listen(QHostAddress::LocalHost, 0); }
    QUrl Url(const QString& path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(listener.serverPort()).arg(path)); }
    QByteArray Release(const Bundle& bundle, const QString& version = QStringLiteral("0.2.0"), bool prerelease = false,
        const QString& notes = QStringLiteral("原创本地发布说明")) const {
        QJsonArray assets;
        for (const auto& [name, body] : {std::pair{QStringLiteral("update-manifest.json"), bundle.manifest},
            std::pair{QStringLiteral("update-manifest.json.sig"), bundle.signature}, std::pair{QStringLiteral("fixture.zip"), bundle.package}})
            assets.append(QJsonObject{{"name", name}, {"size", qint64(body.size())}, {"browser_download_url", Url(QStringLiteral("/assets/") + name).toString()}});
        return QJsonDocument(QJsonObject{{"tag_name", QStringLiteral("v") + version}, {"draft", false}, {"prerelease", prerelease}, {"body", notes}, {"assets", assets}}).toJson(QJsonDocument::Compact);
    }
    void Publish(const Bundle& bundle, const QString& version = QStringLiteral("0.2.0"), bool prerelease = false) {
        routes.insert("/repos/fixture/editor/releases/latest", Release(bundle, version, prerelease));
        routes.insert("/assets/update-manifest.json", bundle.manifest); routes.insert("/assets/update-manifest.json.sig", bundle.signature); routes.insert("/assets/fixture.zip", bundle.package);
    }
    War3Map::QtUpdate::LoopbackUpdateSource Source(const Bundle& bundle, const QString& staging,
        const QString& installation, const QString& current = QStringLiteral("0.1.0")) const {
        return {Url(QStringLiteral("/repos/fixture/editor")), current, bundle.signer.KeyHex(), staging, 1000, installation};
    }
    QTcpServer listener;
    QMap<QByteArray, QByteArray> routes;
    QList<QByteArray> requests;
    std::function<void(QTcpSocket*, const QByteArray&)> responder;
};

} // namespace UpdateFixtures
