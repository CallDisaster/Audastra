#include "McpResources.h"
#include "McpServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <vector>

namespace War3Map::Mcp {
namespace {

constexpr qint64 kFileBytes = 512 * 1024;
constexpr qint64 kTotalBytes = 2 * 1024 * 1024;
constexpr std::size_t kFilesPerSkill = 32;

struct SkillFile {
    QString relativePath;
    QByteArray bytes;
    QString text;
    QString sha256;
};

bool ContainedIn(const QString& root, const QString& file) {
    const QString prefix = QDir::cleanPath(root) + '/';
#ifdef _WIN32
    return file.startsWith(prefix, Qt::CaseInsensitive);
#else
    return file.startsWith(prefix, Qt::CaseSensitive);
#endif
}

} // namespace

void RegisterAllResources(McpServer& server, const std::filesystem::path& skillRoot) {
    // 旧 resources 只是引导调用工具的 note，占位不代表可读的地图资源。
    // 本轮只发现实际封装的 Skill；地图查询仍使用有身份/预算的真实工具。
    const auto root = skillRoot.empty() ? std::filesystem::path(
        (QCoreApplication::applicationDirPath() + QStringLiteral("/skills")).toStdWString()) : skillRoot;
    RegisterSkillResources(server, root);
}

void RegisterSkillResources(McpServer& server, const std::filesystem::path& skillRoot) {
    server.RemoveResourcesByPrefix(QStringLiteral("forge://skills/"));
    QJsonArray skills, diagnostics;
    const QString configuredRoot = QString::fromStdWString(skillRoot.wstring());
    const QString canonicalRoot = QFileInfo(configuredRoot).canonicalFilePath();
    qint64 totalBytes = 0;
    if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir()) {
        diagnostics.append(QJsonObject{{"code", "MCP_SKILLS_NOT_INSTALLED"},
            {"message", "当前包尚未安装可发现的 Skill；使用实际 tools/list 与 schema。"}});
    } else {
        constexpr std::array names{"forge-editor-mcp", "forge-t-event-optimization"};
        for (const auto* name : names) {
            const QString id = QString::fromLatin1(name);
            const QString directory = QDir(canonicalRoot).filePath(id);
            if (!QFileInfo(directory).isDir()) continue;
            std::vector<SkillFile> files;
            QString error;
            QDirIterator iterator(directory, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString path = iterator.next();
                const QString canonical = QFileInfo(path).canonicalFilePath();
                if (canonical.isEmpty() || !ContainedIn(canonicalRoot, canonical) ||
                    files.size() >= kFilesPerSkill) {
                    error = QStringLiteral("Skill 文件越过安装根或超过32项预算。");
                    break;
                }
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() > kFileBytes) {
                    error = QStringLiteral("Skill 文件读取失败或超过512 KiB。");
                    break;
                }
                QByteArray bytes = file.read(kFileBytes + 1);
                if (bytes.size() > kFileBytes || file.error() != QFileDevice::NoError ||
                    totalBytes + bytes.size() > kTotalBytes) {
                    error = QStringLiteral("Skill 读取不完整或超过2 MiB总预算。");
                    break;
                }
                QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
                QString content = decoder.decode(bytes);
                if (decoder.hasError()) {
                    error = QStringLiteral("Skill 不是有效的 UTF-8 文本。");
                    break;
                }
                totalBytes += bytes.size();
                files.push_back({QDir(directory).relativeFilePath(path), bytes, std::move(content),
                    QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())});
            }
            const auto entry = std::find_if(files.begin(), files.end(), [](const SkillFile& file) {
                return file.relativePath == QStringLiteral("SKILL.md");
            });
            if (entry == files.end() && error.isEmpty()) error = QStringLiteral("缺少真实 SKILL.md。");
            if (!error.isEmpty()) {
                diagnostics.append(QJsonObject{{"code", "MCP_SKILL_READ_REJECTED"}, {"skill", id}, {"message", error}});
                continue;
            }
            const QString entryText = entry->text;
            const QRegularExpression description(QStringLiteral("(?:^|\\n)description: ([^\\r\\n]+)"));
            const auto match = description.match(entryText.left(8192));
            const QString descriptionText = match.hasMatch() ? match.captured(1) : id;
            std::sort(files.begin(), files.end(), [](const SkillFile& left, const SkillFile& right) {
                return left.relativePath < right.relativePath;
            });
            QCryptographicHash bundle(QCryptographicHash::Sha256);
            for (const auto& file : files) {
                const auto relative = file.relativePath.toUtf8();
                bundle.addData(QByteArray::number(relative.size()));
                bundle.addData(QByteArray(1, ':'));
                bundle.addData(relative);
                bundle.addData(file.sha256.toLatin1());
            }
            const QString version = QString::fromLatin1(bundle.result().toHex());
            const QString prefix = QStringLiteral("forge://skills/") + id + '/' + version + '/';
            QJsonArray members;
            for (const auto& file : files) {
                const QString uri = prefix + file.relativePath;
                members.append(QJsonObject{{"path", file.relativePath}, {"uri", uri}, {"sha256", file.sha256},
                    {"bytes", QString::number(file.bytes.size())}});
                ResourceDefinition resource;
                resource.uri = uri;
                resource.name = id + '/' + file.relativePath;
                resource.description = file.relativePath == QStringLiteral("SKILL.md") ? descriptionText : QStringLiteral("Skill 关联文档");
                resource.mimeType = QStringLiteral("text/markdown");
                // 捕获不可变文本；版本、SHA 与 resources/read 始终对应相同字节。
                resource.handler = [content = file.text](const QString&) -> QJsonValue { return content; };
                server.RegisterResource(std::move(resource));
            }
            skills.append(QJsonObject{{"id", id}, {"version", version}, {"description", descriptionText},
                {"entry_uri", prefix + QStringLiteral("SKILL.md")}, {"files", members}});
        }
    }
    const QJsonObject manifest{{"schema_version", 1}, {"skills", skills}, {"diagnostics", diagnostics},
        {"discovery", "resources/list + resources/read"}, {"automatically_loaded", false}};
    ResourceDefinition resource;
    resource.uri = QStringLiteral("forge://skills/manifest");
    resource.name = QStringLiteral("Audastra Skill 清单");
    resource.description = QStringLiteral("实际已安装的版本化 Skill 与关联文档 SHA；客户端需主动读取。");
    resource.mimeType = QStringLiteral("application/json");
    resource.handler = [manifest](const QString&) -> QJsonValue { return manifest; };
    server.RegisterResource(std::move(resource));
}

} // namespace War3Map::Mcp
