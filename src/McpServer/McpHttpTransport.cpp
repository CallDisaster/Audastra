#include "McpHttpTransport.h"
#include "McpServer.h"

#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace War3Map::Mcp {
namespace {

QByteArray Reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

void Reply(QTcpSocket& socket, int status, const QByteArray& body = {}) {
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + Reason(status) + "\r\n";
    response += "Connection: close\r\nCache-Control: no-store\r\n";
    if (!body.isEmpty()) response += "Content-Type: application/json\r\n";
    if (status == 401) response += "WWW-Authenticate: Bearer realm=\"FORGE local MCP\"\r\n";
    if (status == 405) response += "Allow: POST\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    response += body;
    socket.write(response);
    socket.disconnectFromHost();
}

bool ExactSecret(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) return false;
    unsigned difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    return difference == 0;
}

bool OriginAllowed(const QByteArray& origin, quint16 port) {
    if (origin.isEmpty()) return false; // 缺省 Origin 与显式空 Origin 区分。
    const QUrl url(QString::fromLatin1(origin), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("http") ||
        !url.userName().isEmpty() || !url.password().isEmpty() || !url.query().isEmpty() ||
        !url.fragment().isEmpty() || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) return false;
    const auto host = url.host().toLower();
    return (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost")) &&
        url.port(80) == port;
}

bool HostAllowed(const QByteArray& host, quint16 port) {
    return host == "127.0.0.1:" + QByteArray::number(port) ||
        host.toLower() == "localhost:" + QByteArray::number(port) ||
        (port == 80 && (host == "127.0.0.1" || host.toLower() == "localhost"));
}

bool Accepts(const QByteArray& header, const QByteArray& mediaType) {
    for (const auto& entry : header.split(',')) {
        const auto pieces = entry.trimmed().split(';');
        if (pieces[0].trimmed().toLower() != mediaType) continue;
        bool rejected = false;
        for (qsizetype index = 1; index < pieces.size(); ++index) {
            const auto parameter = pieces[index].trimmed().toLower();
            if (!parameter.startsWith("q=")) continue;
            bool valid = false;
            const double quality = parameter.mid(2).toDouble(&valid);
            rejected = !valid || quality <= 0.0 || quality > 1.0;
        }
        if (!rejected) return true;
    }
    return false;
}

} // namespace

class McpHttpTransport::Impl {
public:
    Impl(McpServer& server, McpHttpTransport& owner) : dispatcher_(server), owner_(owner), listener_(&owner) {
        QObject::connect(&listener_, &QTcpServer::newConnection, &owner_, [this] { AcceptConnection(); });
    }

    bool Start(McpHttpOptions options, QString* error) {
        if (error) error->clear();
        const auto fail = [error](const QString& message) {
            if (error) *error = message;
            return false;
        };
        if (QThread::currentThread() != owner_.thread()) return fail(QStringLiteral("MCP 传输必须由宿主线程启动。"));
        if (listener_.isListening()) return fail(QStringLiteral("MCP 端口已开启；修改端口前先关闭。"));
        if (options.bearerToken.size() < 32 || options.bearerToken.size() > 256 ||
            std::any_of(options.bearerToken.begin(), options.bearerToken.end(), [](char byte) {
                return static_cast<unsigned char>(byte) < 0x21 || static_cast<unsigned char>(byte) > 0x7e;
            })) return fail(QStringLiteral("本机 MCP 访问令牌必须是32..256字节可打印 ASCII，不能含空白。"));
        if (options.maxHeaderBytes < 1024 || options.maxHeaderBytes > 64 * 1024 ||
            options.maxBodyBytes < 1024 || options.maxBodyBytes > 16 * 1024 * 1024 ||
            options.maxResponseBytes < 1024 || options.maxResponseBytes > 32 * 1024 * 1024 ||
            options.receiveTimeoutMs < 100 || options.receiveTimeoutMs > 120000 ||
            options.maxConnections < 1 || options.maxConnections > 64)
            return fail(QStringLiteral("MCP 网络预算不在支持范围内。"));
        if (!listener_.listen(QHostAddress::LocalHost, options.port))
            return fail(QStringLiteral("无法开启本机 MCP 端口：%1").arg(listener_.errorString()));
        options_ = std::move(options);
        // 本传输没有真实服务器通知通道，不能对客户端承诺 listChanged。
        dispatcher_.SetNotificationSink({});
        return true;
    }

    void Stop() {
        listener_.close();
        const auto connections = sockets_;
        for (auto* socket : connections) socket->abort();
        options_.bearerToken.fill('\0');
        options_.bearerToken.clear();
    }

    bool IsListening() const noexcept { return listener_.isListening(); }
    quint16 Port() const noexcept { return listener_.isListening() ? listener_.serverPort() : 0; }

private:
    struct Request {
        QByteArray bytes;
        QByteArray method;
        QHash<QByteArray, QByteArray> headers;
        qsizetype bodyOffset = 0;
        qsizetype bodyLength = 0;
        bool parsedHeaders = false;
        bool done = false;
    };

    void AcceptConnection() {
        while (listener_.hasPendingConnections()) {
            auto* socket = listener_.nextPendingConnection();
            if (!socket) continue;
            socket->setParent(&owner_);
            if (!socket->peerAddress().isLoopback() || sockets_.size() >= options_.maxConnections) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                Reply(*socket, 503);
                continue;
            }
            sockets_.insert(socket);
            socket->setReadBufferSize(options_.maxHeaderBytes + options_.maxBodyBytes + 1);
            auto request = std::make_shared<Request>();
            auto* deadline = new QTimer(socket);
            deadline->setSingleShot(true);
            deadline->start(options_.receiveTimeoutMs);
            QObject::connect(deadline, &QTimer::timeout, &owner_, [socket, request] {
                if (request->done) return;
                request->done = true;
                Reply(*socket, 408);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, &owner_, [this, socket] {
                sockets_.remove(socket);
                socket->deleteLater();
            });
            QObject::connect(socket, &QTcpSocket::readyRead, &owner_, [this, socket, deadline, request] {
                Read(*socket, *deadline, *request);
            });
            if (socket->bytesAvailable() > 0) Read(*socket, *deadline, *request);
        }
    }

    void Reject(QTcpSocket& socket, QTimer& deadline, Request& request, int status) {
        request.done = true;
        deadline.stop();
        Reply(socket, status);
    }

    void Read(QTcpSocket& socket, QTimer& deadline, Request& request) {
        if (request.done) return;
        const qsizetype budget = options_.maxHeaderBytes + options_.maxBodyBytes;
        const qint64 remaining = static_cast<qint64>(budget - request.bytes.size());
        request.bytes += socket.read(remaining + 1);
        if (request.bytes.size() > budget) { Reject(socket, deadline, request, 413); return; }
        if (!request.parsedHeaders) {
            const qsizetype boundary = request.bytes.indexOf("\r\n\r\n");
            if (boundary < 0) {
                if (request.bytes.size() > options_.maxHeaderBytes) Reject(socket, deadline, request, 431);
                return;
            }
            if (boundary + 4 > options_.maxHeaderBytes) { Reject(socket, deadline, request, 431); return; }
            const auto lines = request.bytes.left(boundary).split('\n');
            const auto first = lines[0].trimmed().split(' ');
            if (first.size() != 3 || first[2] != "HTTP/1.1") { Reject(socket, deadline, request, 400); return; }
            if (first[1] != "/mcp") { Reject(socket, deadline, request, 404); return; }
            request.method = first[0];
            for (qsizetype index = 1; index < lines.size(); ++index) {
                const auto line = lines[index].trimmed();
                const auto colon = line.indexOf(':');
                if (colon <= 0) { Reject(socket, deadline, request, 400); return; }
                const auto name = line.left(colon).toLower();
                if (name.contains(' ') || name.contains('\t') || request.headers.contains(name)) {
                    Reject(socket, deadline, request, 400); return;
                }
                request.headers.insert(name, line.mid(colon + 1).trimmed());
            }
            const auto& headers = request.headers;
            if (!HostAllowed(headers.value("host"), Port()) ||
                (headers.contains("origin") && !OriginAllowed(headers.value("origin"), Port()))) {
                Reject(socket, deadline, request, 403); return;
            }
            if (!ExactSecret(headers.value("authorization"), "Bearer " + options_.bearerToken)) {
                Reject(socket, deadline, request, 401); return;
            }
            if (request.method != "POST") { Reject(socket, deadline, request, 405); return; }
            if (!Accepts(headers.value("accept"), "application/json") ||
                !Accepts(headers.value("accept"), "text/event-stream")) {
                Reject(socket, deadline, request, 406); return;
            }
            if (headers.value("content-type").split(';')[0].trimmed().toLower() != "application/json") {
                Reject(socket, deadline, request, 415); return;
            }
            if (headers.contains("transfer-encoding") || !headers.contains("content-length")) {
                Reject(socket, deadline, request, 400); return;
            }
            const auto contentLength = headers.value("content-length");
            if (contentLength.isEmpty() || std::any_of(contentLength.begin(), contentLength.end(), [](char character) {
                return character < '0' || character > '9';
            })) { Reject(socket, deadline, request, 400); return; }
            bool valid = false;
            const auto length = contentLength.toLongLong(&valid);
            if (!valid || length > options_.maxBodyBytes) { Reject(socket, deadline, request, 413); return; }
            request.bodyOffset = boundary + 4;
            request.bodyLength = static_cast<qsizetype>(length);
            request.parsedHeaders = true;
        }
        const auto completeBytes = request.bodyOffset + request.bodyLength;
        if (request.bytes.size() < completeBytes) return;
        if (request.bytes.size() != completeBytes) { Reject(socket, deadline, request, 400); return; }
        request.done = true;
        deadline.stop();
        // 等游戏结果、等检查这类工具会在处理中转嵌套事件循环，客户端在这期间断开时连接会被删除。先让套接字的
        // 读信号处理完再分发：在它自己的信号里被删会让 Qt 回到已释放的对象上。
        QMetaObject::invokeMethod(&owner_, [this, alive = QPointer<QTcpSocket>(&socket), copy = request] {
            if (alive) Dispatch(*alive, copy);
        }, Qt::QueuedConnection);
    }

    void Dispatch(QTcpSocket& socket, const Request& request) {
        const auto body = request.bytes.mid(request.bodyOffset, request.bodyLength);
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) {
            Reply(socket, 400, SerializeResponse(JsonRpcResponse::Error(QJsonValue(), JsonRpcResponse::kParseError,
                QStringLiteral("HTTP MCP 只接受一条 JSON-RPC 对象。"))));
            return;
        }
        const auto object = json.object();
        // Streamable HTTP 的旧请求无版本头时使用正式规范默认2025-03-26。
        const QString version = QString::fromLatin1(request.headers.value("mcp-protocol-version", "2025-03-26"));
        if (version == QStringLiteral("2024-11-05") || !McpServer::SupportsProtocolVersion(version)) {
            Reply(socket, 400); return;
        }
        if (object.value("jsonrpc") != QStringLiteral("2.0")) { Reply(socket, 400); return; }
        if (!object.contains("method")) {
            // 本服务器不主动发起请求；格式正确的客户端响应仍按规范202接收。
            if (!object.contains("id") || (object.contains("result") == object.contains("error"))) {
                Reply(socket, 400); return;
            }
            Reply(socket, 202); return;
        }
        const auto parsed = ParseRequest(body);
        if (!parsed) { Reply(socket, 400); return; }
        if (parsed->method == QStringLiteral("initialize") &&
            parsed->params.value("protocolVersion") == QStringLiteral("2024-11-05")) {
            Reply(socket, 400); return;
        }
        // 工具处理期间客户端断开时连接已被删除，处理完不能再写回应。
        const QPointer<QTcpSocket> alive(&socket);
        const auto response = dispatcher_.HandleMessage(body);
        if (!alive) return;
        if (!response) { Reply(*alive, 202); return; }
        const auto serialized = SerializeResponse(*response);
        if (serialized.size() > options_.maxResponseBytes) {
            Reply(*alive, 200, SerializeResponse(JsonRpcResponse::Error(response->id,
                JsonRpcResponse::kInternalError, QStringLiteral("MCP 响应超过网络预算；缩小查询范围。"))));
            return;
        }
        Reply(*alive, 200, serialized);
    }

    McpServer& dispatcher_;
    McpHttpTransport& owner_;
    QTcpServer listener_;
    QSet<QTcpSocket*> sockets_;
    McpHttpOptions options_;
};

McpHttpTransport::McpHttpTransport(McpServer& server, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(server, *this)) {}
McpHttpTransport::~McpHttpTransport() { impl_->Stop(); }
bool McpHttpTransport::Start(McpHttpOptions options, QString* error) { return impl_->Start(std::move(options), error); }
void McpHttpTransport::Stop() { impl_->Stop(); }
bool McpHttpTransport::IsListening() const noexcept { return impl_->IsListening(); }
quint16 McpHttpTransport::Port() const noexcept { return impl_->Port(); }
QUrl McpHttpTransport::EndpointUrl() const {
    return IsListening() ? QUrl(QStringLiteral("http://127.0.0.1:%1/mcp").arg(Port())) : QUrl();
}

} // namespace War3Map::Mcp
