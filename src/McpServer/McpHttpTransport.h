#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

namespace War3Map::Mcp {
class McpServer;

struct McpHttpOptions {
    quint16 port = 18564; // 0 允许系统分配，用于确定性本机测试。
    QByteArray bearerToken;
    qsizetype maxHeaderBytes = 16 * 1024;
    qsizetype maxBodyBytes = 4 * 1024 * 1024;
    qsizetype maxResponseBytes = 8 * 1024 * 1024;
    int receiveTimeoutMs = 15000;
    int maxConnections = 8;
};

// Streamable HTTP 的 JSON-only 子集：POST /mcp，GET 405，无 SSE/重连回放。
// 只监听 IPv4 localhost，不持有/复制文档或领域事务；所有调用在宿主线程串行。
// 超时约束接收网络消息；同步领域命令的执行预算由共用服务负责。
class McpHttpTransport final : public QObject {
public:
    explicit McpHttpTransport(McpServer& server, QObject* parent = nullptr);
    ~McpHttpTransport() override;

    [[nodiscard]] bool Start(McpHttpOptions options, QString* error = nullptr);
    void Stop();
    [[nodiscard]] bool IsListening() const noexcept;
    [[nodiscard]] quint16 Port() const noexcept;
    [[nodiscard]] QUrl EndpointUrl() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Mcp
