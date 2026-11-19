#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace War3Map::Agent {

struct MemoryFileSnapshot {
    QString scope;       // "project" | "global"
    QString displayName; // 如 "测试地图.memory.md" 或 "全局 MEMORY.md"
    QString filePath;
    QString content;
    bool exists = false;
    // 内容是否经作者确认：全局记忆恒为 true；地图记忆仅当内容哈希与信任记录一致（经本服务写入或作者在编辑器保存）。
    // 未确认的地图记忆不注入系统提示词。
    bool trusted = false;
    qint64 tokens = 0;
    QString error;
};

// 地图专属记忆（<地图名>.memory.md，目录地图为父目录中的 <目录名>.memory.md）与全局记忆管理服务。
// 默认在测试环境中不主动读取作者本机全局目录，仅在宿主显式开启用户配置或指定路径后启用。
class ProjectMemoryService final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kMaximumMemoryBytes = 64 * 1024;
    static constexpr qint64 kMaximumTrustRecordBytes = 1024 * 1024;
    static constexpr qsizetype kMaximumTrustEntries = 512;

    explicit ProjectMemoryService(QObject* parent = nullptr);

    void SetMapSourcePath(const QString& mapSourcePath);
    [[nodiscard]] QString MapSourcePath() const { return mapSourcePath_; }
    [[nodiscard]] QString ProjectMemoryFilePath() const;

    // 全局记忆与信任记录都放在应用配置目录 agent/ 下。
    void UseDefaultGlobalMemoryPath();
    void SetGlobalMemoryFilePath(QString filePath);
    [[nodiscard]] QString GlobalMemoryFilePath() const { return globalMemoryPath_; }

    // 信任记录（绝对路径 → SHA-256）；路径为空时只保存在内存中。设置路径时立即读取已有记录。
    void SetTrustRecordFilePath(QString filePath);
    [[nodiscard]] QString TrustRecordFilePath() const { return trustRecordPath_; }

    [[nodiscard]] static QString ResolveProjectMemoryPath(const QString& mapSourcePath);

    [[nodiscard]] MemoryFileSnapshot ReadScope(const QString& scope) const;
    [[nodiscard]] QList<MemoryFileSnapshot> Snapshots() const;
    [[nodiscard]] QString BuildPromptSection() const;

    // 写入成功后，地图记忆的新内容记为作者已确认（工具写入与编辑器保存均经过此处）。
    [[nodiscard]] bool WriteScope(const QString& scope, const QString& content,
        const QString& mode = QStringLiteral("replace"), QString* error = nullptr);

    [[nodiscard]] static QJsonArray ToolDefinitions();
    [[nodiscard]] static bool IsMemoryTool(const QString& toolName);
    [[nodiscard]] QJsonObject InvokeTool(const QString& toolName, const QJsonObject& arguments, bool allowEdits);

    // 生成提示词块时转义：正文中与外层块同名的起止标签改写为 &lt;，属性值中的引号与尖括号转义。
    [[nodiscard]] static QString EscapePromptBlockText(const QString& text, const QStringList& tagNames);
    [[nodiscard]] static QString EscapePromptAttribute(const QString& value);

signals:
    void MemoryChanged();

private:
    [[nodiscard]] static QString TrustKey(const QString& filePath);
    [[nodiscard]] bool IsTrusted(const QString& filePath, const QByteArray& sha256Hex) const;
    void RecordTrusted(const QString& filePath, const QByteArray& sha256Hex);
    void LoadTrustRecord();
    void SaveTrustRecord() const;

    QString mapSourcePath_;
    QString globalMemoryPath_;
    QString trustRecordPath_;
    QHash<QString, QByteArray> trustedHashes_;
    QStringList trustOrder_;
};

} // namespace War3Map::Agent
