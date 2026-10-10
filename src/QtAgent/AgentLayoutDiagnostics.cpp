#include "AgentLayoutDiagnostics.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <utility>

namespace War3Map::Agent {
namespace {
constexpr qint64 kMaximumLogBytes = 1024 * 1024;
constexpr int kMaximumRecords = 64;

QJsonArray Size(const QSize& size) { return {size.width(), size.height()}; }
QJsonArray Rect(const QRect& rect) { return {rect.x(), rect.y(), rect.width(), rect.height()}; }
QString Kind(AgentLayoutRowKind kind) {
    switch (kind) {
    case AgentLayoutRowKind::Assistant: return QStringLiteral("assistant");
    case AgentLayoutRowKind::User: return QStringLiteral("user");
    case AgentLayoutRowKind::Tool: return QStringLiteral("tool");
    case AgentLayoutRowKind::Change: return QStringLiteral("change");
    default: return QStringLiteral("unknown");
    }
}
QJsonObject Row(const AgentLayoutRowSnapshot& row) {
    QJsonObject result{{"row_index", row.index}, {"kind", Kind(row.kind)}, {"geometry", Rect(row.geometry)},
        {"minimum_size", Size(row.minimumSize)}, {"size_hint", Size(row.sizeHint)}};
    if (row.hasDocument) {
        result.insert("body_geometry", Rect(row.bodyGeometry));
        result.insert("document_height", row.documentHeight);
    }
    return result;
}
} // namespace

AgentLayoutDiagnostics::AgentLayoutDiagnostics(QString filePath)
    : filePath_(filePath.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(
            QStringLiteral("agent/diagnostics/layout-gaps-%1.jsonl").arg(QCoreApplication::applicationPid()))
        : std::move(filePath)) {}

int AgentLayoutDiagnostics::RetryAfterMs(qint64 monotonicMs) const {
    return static_cast<int>(std::clamp<qint64>(1000 - (monotonicMs - lastRecordMs_), 1, 1000));
}

AgentLayoutDiagnostics::Observation AgentLayoutDiagnostics::Observe(const AgentLayoutSnapshot& snapshot, qint64 monotonicMs) {
    if (stopped_) return Observation::Suppressed;
    if (!std::isfinite(snapshot.devicePixelRatio) || snapshot.devicePixelRatio <= 0) return Observation::Quiet;
    QJsonArray gaps;
    // 调用方最多传512行；异常输入同样有界，最多记录8对，不制造长日志。
    const auto count = std::min<qsizetype>(512, snapshot.rows.size());
    for (qsizetype i = 1; i < count && gaps.size() < 8; ++i) {
        const auto& previous = snapshot.rows[i - 1]; const auto& next = snapshot.rows[i];
        const qint64 gap = static_cast<qint64>(next.geometry.y()) - previous.geometry.y() - previous.geometry.height();
        if (gap <= 48) continue;
        gaps.append(QJsonObject{{"gap_logical_px", gap}, {"previous", Row(previous)}, {"next", Row(next)}});
    }
    if (gaps.isEmpty()) return Observation::Quiet;
    QJsonObject record{{"schema_version", 1}, {"event", "agent_layout_gap"},
        {"coordinate_unit", "qt_logical_pixel"}, {"device_pixel_ratio", snapshot.devicePixelRatio},
        {"viewport_size", Size(snapshot.viewportSize)}, {"column_size", Size(snapshot.columnSize)},
        {"omitted_rows", snapshot.omittedRows}, {"gaps", gaps}};
    const auto fingerprint = QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (fingerprints_.contains(fingerprint)) return Observation::Suppressed;
    if (monotonicMs - lastRecordMs_ < 1000) return Observation::RateLimited;
    if (records_ >= kMaximumRecords) {
        stopped_ = true; lastError_ = QStringLiteral("AGENT_LAYOUT_LOG_BUDGET"); return Observation::BudgetExhausted;
    }
    record.insert("application_version", QCoreApplication::applicationVersion());
    record.insert("qt_version", QString::fromLatin1(qVersion()));
    record.insert("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    record.insert("elapsed_ms", monotonicMs);
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
    const auto fail = [this] {
        stopped_ = true; lastError_ = QStringLiteral("AGENT_LAYOUT_LOG_WRITE_FAILED"); return Observation::WriteFailed;
    };
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath())) return fail();
    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return fail();
    if (file.size() > kMaximumLogBytes - bytes.size()) {
        stopped_ = true; lastError_ = QStringLiteral("AGENT_LAYOUT_LOG_BUDGET"); return Observation::BudgetExhausted;
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) return fail();
    fingerprints_.insert(fingerprint); fingerprintOrder_.append(fingerprint);
    if (fingerprintOrder_.size() > 64) fingerprints_.remove(fingerprintOrder_.takeFirst());
    ++records_; lastRecordMs_ = monotonicMs;
    return Observation::Recorded;
}

} // namespace War3Map::Agent
