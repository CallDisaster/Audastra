#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

namespace War3Map::Agent {

struct MemoryFileSnapshot {
    QString scope;       // "project" | "global"
    QString displayName; // 如 "测试地图.memory.md" 或 "全局 MEMORY.md"
    QString filePath;
    QString content;
    bool exists = false;
    qint64 tokens = 0;
    QString error;
};

// 地图专属记忆（<地图名>.memory.md / 目录工程 MEMORY.md）与全局记忆管理服务。
// 默认在测试环境中不主动读取作者本机全局目录，仅在宿主显式开启用户配置或指定路径后启用。
class ProjectMemoryService final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kMaximumMemoryBytes = 64 * 1024;

    explicit ProjectMemoryService(QObject* parent = nullptr);

    void SetMapSourcePath(const QString& mapSourcePath);
    [[nodiscard]] QString MapSourcePath() const { return mapSourcePath_; }
    [[nodiscard]] QString ProjectMemoryFilePath() const;

    void UseDefaultGlobalMemoryPath();
    void SetGlobalMemoryFilePath(QString filePath);
    [[nodiscard]] QString GlobalMemoryFilePath() const { return globalMemoryPath_; }

    [[nodiscard]] static QString ResolveProjectMemoryPath(const QString& mapSourcePath);

    [[nodiscard]] MemoryFileSnapshot ReadScope(const QString& scope) const;
    [[nodiscard]] QList<MemoryFileSnapshot> Snapshots() const;
    [[nodiscard]] QString BuildPromptSection() const;

    [[nodiscard]] bool WriteScope(const QString& scope, const QString& content,
        const QString& mode = QStringLiteral("replace"), QString* error = nullptr);

    [[nodiscard]] static QJsonArray ToolDefinitions();
    [[nodiscard]] static bool IsMemoryTool(const QString& toolName);
    [[nodiscard]] QJsonObject InvokeTool(const QString& toolName, const QJsonObject& arguments, bool allowEdits);

signals:
    void MemoryChanged();

private:
    QString mapSourcePath_;
    QString globalMemoryPath_;
};

} // namespace War3Map::Agent
