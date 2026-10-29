#include "PendingImportPayload.h"
#include "EditorProject.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/ProjectSaveControl.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

namespace War3Map::QtProject {
Status ValidatePendingImports(std::span<const PendingImport> entries) {
    std::uint64_t ownedBytes = 0;
    for (const auto& entry : entries) {
        if (entry.size < 0) return Status::Failure("Import", "OWNED_IMPORT_INVALID", "导入长度无效。");
        const auto role = entry.ownedPayload ? (entry.runtimeLuaDraft ? Services::ImportPayloadRole::OwnedLuaDraft
            : Services::ImportPayloadRole::OwnedResource) : Services::ImportPayloadRole::DiskResource;
        if (const auto status = Services::ImportPayloadPolicy::Check(role, static_cast<std::uint64_t>(entry.size)); !status.Succeeded()) return status;
        if (entry.ownedPayload) {
            if (static_cast<std::uint64_t>(entry.size) > Services::ImportPayloadPolicy::ownedBatchBytes - ownedBytes)
                return Status::Failure("Import", "IMPORT_PAYLOAD_BUDGET", "待发布的内存导入合计超过256MiB预算。");
            ownedBytes += static_cast<std::uint64_t>(entry.size);
        }
    }
    return Status::Ok();
}
Result<std::string> FingerprintPendingImport(const PendingImport& entry, const ProjectSaveControl* control) {
    if (entry.ownedPayload) {
        if (!entry.diskPath.isEmpty() || entry.size < 0 || static_cast<std::size_t>(entry.size) != entry.ownedPayload->size())
            return Result<std::string>::Failure("资源", "OWNED_IMPORT_INVALID", "内存导入的来源、长度或预算不一致。");
        const auto role = entry.runtimeLuaDraft ? Services::ImportPayloadRole::OwnedLuaDraft : Services::ImportPayloadRole::OwnedResource;
        if (const auto status = Services::ImportPayloadPolicy::Check(role, entry.ownedPayload->size()); !status.Succeeded()) return {{}, status.diagnostics};
        return Sha256(*entry.ownedPayload, control);
    }
    if (entry.diskPath.isEmpty() || entry.size < 0)
        return Result<std::string>::Failure("资源", "IMPORT_SOURCE_INVALID", "导入缺少完整来源。");
    return Sha256File(std::filesystem::path(entry.diskPath.toStdWString()), control);
}
Result<std::vector<std::byte>> ReadFrozenPendingImport(const PendingImport& entry, std::string_view expectedSha256,
    const ProjectSaveControl* control) {
    if (!entry.ownedPayload)
        return ReadVerifiedFile(std::filesystem::path(entry.diskPath.toStdWString()), expectedSha256, Services::ImportPayloadPolicy::diskFileBytes, control);
    const auto fingerprint = FingerprintPendingImport(entry, control);
    if (!fingerprint.Succeeded() || !fingerprint.value)
        return {{}, fingerprint.diagnostics};
    if (*fingerprint.value != expectedSha256)
        return Result<std::vector<std::byte>>::Failure("资源", "OWNED_IMPORT_CHANGED", "内存导入与保存开始时的SHA不一致。");
    std::vector<std::byte> bytes(entry.ownedPayload->size());
    if (!CopyProjectSaveBytes(bytes, *entry.ownedPayload, control)) return {{}, ProjectSaveCancelledStatus().diagnostics};
    return Result<std::vector<std::byte>>::Success(std::move(bytes));
}
} // namespace War3Map::QtProject
