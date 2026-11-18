#include "ProjectMemoryService.h"
#include "TokenMeter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace War3Map::Agent {
namespace {

QJsonObject ToolErrorResult(const QString& code, const QString& message) {
    return {{QStringLiteral("isError"), true}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

MemoryFileSnapshot ReadMemoryFile(const QString& scope, const QString& displayName, const QString& path) {
    MemoryFileSnapshot snap;
    snap.scope = scope;
    snap.displayName = displayName;
    snap.filePath = path;
    if (path.isEmpty()) return snap;
    const QFileInfo info(path);
    if (info.isSymLink()) {
        snap.error = QStringLiteral("记忆文件不能是符号链接。");
        return snap;
    }
    if (!info.exists()) return snap;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > ProjectMemoryService::kMaximumMemoryBytes) {
        snap.exists = true;
        snap.error = QStringLiteral("无法读取记忆文件，或文件超过 64 KiB 上限。");
        return snap;
    }
    snap.exists = true;
    snap.content = QString::fromUtf8(file.readAll());
    snap.tokens = TokenMeter::EstimateText(snap.content);
    return snap;
}

} // namespace

ProjectMemoryService::ProjectMemoryService(QObject* parent) : QObject(parent) {}

QString ProjectMemoryService::ResolveProjectMemoryPath(const QString& mapSourcePath) {
    const QString trimmed = mapSourcePath.trimmed();
    if (trimmed.isEmpty()) return {};
    const QFileInfo info(trimmed);
    if (info.exists() && info.isDir()) {
        return QDir(info.absoluteFilePath()).filePath(QStringLiteral("MEMORY.md"));
    }
    const QString base = info.completeBaseName();
    if (base.isEmpty()) return {};
    return info.dir().filePath(base + QStringLiteral(".memory.md"));
}

void ProjectMemoryService::SetMapSourcePath(const QString& mapSourcePath) {
    const QString cleaned = mapSourcePath.trimmed().isEmpty()
        ? QString{}
        : QDir::cleanPath(QFileInfo(mapSourcePath).absoluteFilePath());
    if (mapSourcePath_ == cleaned) return;
    mapSourcePath_ = cleaned;
    emit MemoryChanged();
}

QString ProjectMemoryService::ProjectMemoryFilePath() const {
    return ResolveProjectMemoryPath(mapSourcePath_);
}

void ProjectMemoryService::UseDefaultGlobalMemoryPath() {
    SetGlobalMemoryFilePath(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("agent/MEMORY.md")));
}

void ProjectMemoryService::SetGlobalMemoryFilePath(QString filePath) {
    if (globalMemoryPath_ == filePath) return;
    globalMemoryPath_ = std::move(filePath);
    emit MemoryChanged();
}

MemoryFileSnapshot ProjectMemoryService::ReadScope(const QString& scope) const {
    if (scope == QStringLiteral("global")) {
        return ReadMemoryFile(QStringLiteral("global"), QStringLiteral("全局 MEMORY.md"), globalMemoryPath_);
    }
    const QString projectPath = ProjectMemoryFilePath();
    const QString name = projectPath.isEmpty()
        ? QStringLiteral("未打开地图（无项目记忆）")
        : QFileInfo(projectPath).fileName();
    return ReadMemoryFile(QStringLiteral("project"), name, projectPath);
}

QList<MemoryFileSnapshot> ProjectMemoryService::Snapshots() const {
    QList<MemoryFileSnapshot> list;
    const QString projectPath = ProjectMemoryFilePath();
    if (!projectPath.isEmpty()) {
        list.push_back(ReadScope(QStringLiteral("project")));
    }
    if (!globalMemoryPath_.isEmpty()) {
        const auto global = ReadScope(QStringLiteral("global"));
        if (global.exists || list.isEmpty()) list.push_back(global);
    }
    return list;
}

QString ProjectMemoryService::BuildPromptSection() const {
    QStringList blocks;
    const auto project = ReadScope(QStringLiteral("project"));
    if (!project.filePath.isEmpty()) {
        if (project.exists && project.error.isEmpty() && !project.content.trimmed().isEmpty()) {
            blocks.push_back(QStringLiteral("<project-memory file=\"%1\">\n%2\n</project-memory>")
                .arg(project.displayName, project.content.trimmed()));
        } else if (!project.exists) {
            blocks.push_back(QStringLiteral("当前地图记忆文件（%1）尚不存在；当作者说明长期作图习惯、命名规范、属性约定或关键触发器/物编架构且允许修改时，可调用 memory_write 记录。")
                .arg(project.displayName));
        }
    }
    if (!globalMemoryPath_.isEmpty()) {
        const auto global = ReadScope(QStringLiteral("global"));
        if (global.exists && global.error.isEmpty() && !global.content.trimmed().isEmpty()) {
            blocks.push_back(QStringLiteral("<global-memory file=\"MEMORY.md\">\n%1\n</global-memory>")
                .arg(global.content.trimmed()));
        }
    }
    if (blocks.isEmpty()) return {};
    return QStringLiteral("\n\n# 持久记忆（跨会话保留）\n") + blocks.join(QStringLiteral("\n\n"));
}

bool ProjectMemoryService::WriteScope(const QString& scope, const QString& content, const QString& mode, QString* error) {
    if (error != nullptr) error->clear();
    const QString targetPath = scope == QStringLiteral("global") ? globalMemoryPath_ : ProjectMemoryFilePath();
    if (targetPath.isEmpty()) {
        if (error != nullptr) *error = scope == QStringLiteral("global")
            ? QStringLiteral("全局记忆路径未配置。")
            : QStringLiteral("当前未打开地图文件，无法写入地图专属记忆。");
        return false;
    }
    const QFileInfo info(targetPath);
    if (info.isSymLink()) {
        if (error != nullptr) *error = QStringLiteral("目标记忆文件是符号链接，拒绝写入。");
        return false;
    }
    QString finalText = content;
    if (mode == QStringLiteral("append")) {
        const auto existing = ReadScope(scope);
        if (!existing.error.isEmpty()) {
            if (error != nullptr) *error = existing.error;
            return false;
        }
        if (!existing.content.isEmpty()) {
            finalText = existing.content.endsWith(QLatin1Char('\n'))
                ? (existing.content + content)
                : (existing.content + QLatin1Char('\n') + content);
        }
    } else if (mode != QStringLiteral("replace")) {
        if (error != nullptr) *error = QStringLiteral("写入模式仅支持 replace 或 append。");
        return false;
    }
    const QByteArray utf8 = finalText.toUtf8();
    if (utf8.size() > kMaximumMemoryBytes) {
        if (error != nullptr) *error = QStringLiteral("记忆内容超过 64 KiB 上限，请精简后再保存。");
        return false;
    }
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("无法创建记忆文件所在目录。");
        return false;
    }
    QSaveFile save(targetPath);
    save.setDirectWriteFallback(false);
    if (!save.open(QIODevice::WriteOnly) || save.write(utf8) != utf8.size() || !save.commit()) {
        if (error != nullptr) *error = QStringLiteral("无法原子写入记忆文件，原内容保留。");
        return false;
    }
    emit MemoryChanged();
    return true;
}

QJsonArray ProjectMemoryService::ToolDefinitions() {
    QJsonObject readSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("project"), QStringLiteral("global"), QStringLiteral("all")}},
                {QStringLiteral("description"), QStringLiteral("读取范围：project（当前地图同名 .memory.md）、global（全局 MEMORY.md）或 all。")},
            }},
        }},
    };
    QJsonObject writeSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("content")}},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("project"), QStringLiteral("global")}},
                {QStringLiteral("description"), QStringLiteral("写入目标：默认 project（保存在地图同目录 <地图名>.memory.md）。")},
            }},
            {QStringLiteral("mode"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("replace"), QStringLiteral("append")}},
                {QStringLiteral("description"), QStringLiteral("replace 覆盖整份记忆并整理结构；append 在末尾追加条目。")},
            }},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("Markdown 格式的记忆正文，不超过 64 KiB。")},
            }},
        }},
    };
    return {
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("memory_read")},
            {QStringLiteral("description"), QStringLiteral("读取当前地图专属记忆文件（<地图名>.memory.md）或全局记忆文件（MEMORY.md）。")},
            {QStringLiteral("inputSchema"), readSchema},
        },
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("memory_write")},
            {QStringLiteral("description"), QStringLiteral("写入或更新当前地图专属记忆文件（<地图名>.memory.md）或全局记忆，持久保存作图习惯、设计约定与关键对象架构。")},
            {QStringLiteral("inputSchema"), writeSchema},
        },
    };
}

bool ProjectMemoryService::IsMemoryTool(const QString& toolName) {
    return toolName == QStringLiteral("memory_read") || toolName == QStringLiteral("memory_write");
}

QJsonObject ProjectMemoryService::InvokeTool(const QString& toolName, const QJsonObject& arguments, bool allowEdits) {
    if (toolName == QStringLiteral("memory_read")) {
        const QString scope = arguments.value(QStringLiteral("scope")).toString(QStringLiteral("all"));
        QJsonArray files;
        const auto appendSnap = [&files](const MemoryFileSnapshot& s) {
            if (s.filePath.isEmpty() && s.scope == QStringLiteral("global")) return;
            files.append(QJsonObject{
                {QStringLiteral("scope"), s.scope},
                {QStringLiteral("name"), s.displayName},
                {QStringLiteral("exists"), s.exists},
                {QStringLiteral("tokens"), s.tokens},
                {QStringLiteral("content"), s.content},
                {QStringLiteral("error"), s.error},
            });
        };
        if (scope == QStringLiteral("project") || scope == QStringLiteral("all")) appendSnap(ReadScope(QStringLiteral("project")));
        if (scope == QStringLiteral("global") || scope == QStringLiteral("all")) appendSnap(ReadScope(QStringLiteral("global")));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("files"), files}};
    }
    if (toolName == QStringLiteral("memory_write")) {
        if (!allowEdits) {
            return ToolErrorResult(QStringLiteral("AGENT_MEMORY_WRITE_DENIED"),
                QStringLiteral("当前处于只读权限档位，未允许写入地图记忆文件；请切换至可修改权限后再记录。"));
        }
        const QString scope = arguments.value(QStringLiteral("scope")).toString(QStringLiteral("project"));
        const QString mode = arguments.value(QStringLiteral("mode")).toString(QStringLiteral("replace"));
        if (!arguments.value(QStringLiteral("content")).isString()) {
            return ToolErrorResult(QStringLiteral("AGENT_MEMORY_INVALID_ARGS"), QStringLiteral("memory_write 需要提供文本类型的 content。"));
        }
        QString error;
        if (!WriteScope(scope, arguments.value(QStringLiteral("content")).toString(), mode, &error)) {
            return ToolErrorResult(QStringLiteral("AGENT_MEMORY_WRITE_FAILED"), error);
        }
        const auto updated = ReadScope(scope);
        return {
            {QStringLiteral("ok"), true},
            {QStringLiteral("scope"), updated.scope},
            {QStringLiteral("name"), updated.displayName},
            {QStringLiteral("tokens"), updated.tokens},
        };
    }
    return ToolErrorResult(QStringLiteral("AGENT_UNKNOWN_MEMORY_TOOL"), QStringLiteral("未知记忆工具。"));
}

} // namespace War3Map::Agent
