#include "LiveEditorDiskImports.h"

#include "McpToolInternal.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

#include <QFile>
#include <QFileInfo>

#include <cstring>
#include <memory>

namespace War3Map::QtApp {

std::optional<QJsonObject> ReadDiskImports(const QJsonArray& files, std::vector<QtProject::EditorProject::ImportPayload>& payloads) {
    constexpr qint64 kMaxFileBytes = Services::ImportPayloadPolicy::ownedFileBytes,
        kMaxTotalBytes = Services::ImportPayloadPolicy::ownedBatchBytes;
    qint64 total = 0;
    for (const auto& item : files) {
        const auto file = item.toObject();
        const auto disk = file.value("disk_path").toString();
        QFile input(disk);
        if (!QFileInfo(disk).isFile() || !input.open(QIODevice::ReadOnly))
            return Mcp::Failure("IMPORTS_ADD_FILE_UNREADABLE", "无法读取本机文件：" + disk.toStdString());
        if (input.size() > kMaxFileBytes || (total += input.size()) > kMaxTotalBytes)
            return Mcp::Failure("IMPORTS_ADD_FILE_TOO_LARGE", "单个文件不能超过 64 MiB，一次合计不超过 256 MiB。");
        const auto data = input.readAll();
        if (data.size() != input.size())
            return Mcp::Failure("IMPORTS_ADD_FILE_UNREADABLE", "读取本机文件不完整：" + disk.toStdString());
        auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(data.size()));
        std::memcpy(bytes->data(), data.constData(), static_cast<std::size_t>(data.size()));
        payloads.push_back({file.value("archive_path").toString(), std::move(bytes)});
    }
    return std::nullopt;
}

} // namespace War3Map::QtApp
