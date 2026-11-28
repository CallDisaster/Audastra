#include "LiveEditorDiskImports.h"

#include "AssetOwnedPayload.h"
#include "McpToolInternal.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

namespace War3Map::QtApp {

std::optional<QJsonObject> ReadDiskImports(const QJsonArray& files, std::vector<QtProject::EditorProject::ImportPayload>& payloads) {
    std::uint64_t total = 0;
    for (const auto& item : files) {
        const auto file = item.toObject();
        auto payload = QtProject::ReadDiskImportPayload(file.value("disk_path").toString(), file.value("archive_path").toString());
        if (!payload.Succeeded() || !payload.value) return Mcp::Failure(payload);
        if ((total += payload.value->bytes->size()) > Services::ImportPayloadPolicy::ownedBatchBytes)
            return Mcp::Failure("IMPORTS_ADD_FILE_TOO_LARGE", "单个文件不能超过 64 MiB，一次合计不超过 256 MiB。");
        payloads.push_back(std::move(*payload.value));
    }
    return std::nullopt;
}

} // namespace War3Map::QtApp
