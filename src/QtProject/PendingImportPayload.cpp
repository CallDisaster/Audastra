#include "PendingImportPayload.h"
#include "EditorProject.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/ProjectSaveControl.h"

namespace War3Map::QtProject {
Result<std::string> FingerprintPendingImport(const PendingImport& entry, const ProjectSaveControl* control) {
    if (entry.ownedPayload) {
        if (!entry.diskPath.isEmpty() || entry.size < 0 || static_cast<std::size_t>(entry.size) != entry.ownedPayload->size()
            || entry.ownedPayload->size() > 2 * 1024 * 1024)
            return Result<std::string>::Failure("资源", "OWNED_IMPORT_INVALID", "内存导入的来源、长度或预算不一致。");
        return Sha256(*entry.ownedPayload, control);
    }
    if (entry.diskPath.isEmpty() || entry.size < 0)
        return Result<std::string>::Failure("资源", "IMPORT_SOURCE_INVALID", "导入缺少完整来源。");
    return Sha256File(std::filesystem::path(entry.diskPath.toStdWString()), control);
}
Result<std::vector<std::byte>> ReadFrozenPendingImport(const PendingImport& entry, std::string_view expectedSha256,
    const ProjectSaveControl* control) {
    if (!entry.ownedPayload)
        return ReadVerifiedFile(std::filesystem::path(entry.diskPath.toStdWString()), expectedSha256, 256ull * 1024 * 1024, control);
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
