#pragma once

#include <QByteArray>
#include <QList>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>

namespace War3Map::Agent {

// 诊断输入不接受正文、工具参数或对象名，只保存固定类型和几何。
enum class AgentLayoutRowKind { Assistant, User, Tool, Change, Unknown };
struct AgentLayoutRowSnapshot {
    int index = 0;
    AgentLayoutRowKind kind = AgentLayoutRowKind::Unknown;
    QRect geometry;
    QSize minimumSize, sizeHint;
    bool hasDocument = false;
    QRect bodyGeometry;
    double documentHeight = 0;
};
struct AgentLayoutSnapshot {
    QSize viewportSize, columnSize;
    double devicePixelRatio = 1;
    int omittedRows = 0;
    QList<AgentLayoutRowSnapshot> rows;
};

// 仅异常时打开本地文件。每秒最多一条、每个视图最多64条、文件最多1MiB；
// 最近64份异常几何去重，预算或写入失败可查询且不抛到UI。
class AgentLayoutDiagnostics final {
public:
    enum class Observation { Quiet, Recorded, Suppressed, RateLimited, WriteFailed, BudgetExhausted };
    explicit AgentLayoutDiagnostics(QString filePath = {});
    [[nodiscard]] Observation Observe(const AgentLayoutSnapshot& snapshot, qint64 monotonicMs);
    [[nodiscard]] QString FilePath() const { return filePath_; }
    [[nodiscard]] QString LastError() const { return lastError_; }
    // 仅RateLimited需要尾沿重采；重复几何不应持续轮询。
    [[nodiscard]] int RetryAfterMs(qint64 monotonicMs) const;
private:
    QString filePath_, lastError_;
    QSet<QByteArray> fingerprints_;
    QList<QByteArray> fingerprintOrder_;
    qint64 lastRecordMs_ = -1000;
    int records_ = 0;
    bool stopped_ = false;
};

} // namespace War3Map::Agent
