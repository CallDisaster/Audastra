// 编辑器自有的地图成员（运行库设置、WarVK 设置等）的共用读取：待保存导入优先，其次已打开的地图或目录。
#include "EditorProject.h"
#include "PendingImportPayload.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"

#include <algorithm>

namespace War3Map::QtProject {
namespace {
using Bytes = std::optional<std::vector<std::byte>>;
using Output = Result<Bytes>;

Output Fail(const EditorProject::OwnedMemberSpec& spec, std::string_view message) {
    return Output::Failure(std::string(spec.domain), std::string(spec.code), std::string(spec.label) + std::string(message));
}

QString MemberPath(const EditorProject::OwnedMemberSpec& spec) {
    return QString::fromUtf8(spec.member.data(), static_cast<qsizetype>(spec.member.size()));
}
} // namespace

Output EditorProject::ReadOwnedMapMember(const OwnedMemberSpec& spec) const {
    const auto wanted = AssetKey(MemberPath(spec));
    for (const auto& edit : assetEdits_)
        if (AssetKey(edit.from) == wanted || AssetKey(edit.to) == wanted)
            return Fail(spec, "被普通资源命令移动或删除；请先撤销该改动，再使用共用设置事务。");
    const PendingImport* pending = nullptr;
    for (const auto& item : pendingImports_) if (AssetKey(item.archivePath) == wanted) {
        if (pending) return Fail(spec, "存在重复导入来源。");
        pending = &item;
    }
    if (!pending) return ReadBaseOwnedMapMember(spec);
    if (pending->size < 0 || static_cast<std::uint64_t>(pending->size) > spec.maxBytes)
        return Fail(spec, "超过 " + std::to_string(spec.maxBytes) + " 字节预算。");
    const auto identity = FingerprintPendingImport(*pending);
    if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
    auto bytes = pending->ownedPayload ? ReadFrozenPendingImport(*pending, *identity.value)
        : ReadVerifiedFile(std::filesystem::path(pending->diskPath.toStdWString()), *identity.value, spec.maxBytes);
    if (!bytes.Succeeded() || !bytes.value) return {std::nullopt, bytes.diagnostics};
    return Output::Success(Bytes(std::move(*bytes.value)));
}

Output EditorProject::ReadBaseOwnedMapMember(const OwnedMemberSpec& spec) const {
    if (archiveSession_) {
        const auto* archive = archiveSession_.get();
        const auto& sha = archive->SourceSha256();
        for (const auto& entry : ownedMemberCache_)
            if (entry.archive == archive && !sha.empty() && entry.sourceSha256 == sha && entry.member == spec.member
                && entry.maxBytes == spec.maxBytes)
                return Output::Success(entry.bytes);
        ArchiveExactMemberLimits limits;
        limits.memberCount = 1;
        limits.memberBytes = limits.totalBytes = spec.maxBytes;
        auto captured = archive->CaptureExactMembers(std::vector<std::string>{std::string(spec.member)}, limits);
        if (!captured.Succeeded()) return {std::nullopt, captured.diagnostics};
        if (!captured.value || captured.value->size() != 1) return Fail(spec, "的地图读取结果不完整。");
        Bytes bytes;
        if (captured.value->front().present) bytes = std::move(captured.value->front().bytes);
        if (!sha.empty()) {
            std::erase_if(ownedMemberCache_, [&](const OwnedMemberCache& entry) {
                return entry.archive != archive || entry.sourceSha256 != sha || entry.member == spec.member;
            });
            ownedMemberCache_.push_back({archive, sha, std::string(spec.member), spec.maxBytes, bytes});
        }
        return Output::Success(std::move(bytes));
    }
    if (directorySnapshot_) {
        const auto wanted = AssetKey(MemberPath(spec));
        const auto& snapshot = directorySnapshot_->Snapshot();
        const WorkspaceFile* found = nullptr;
        for (const auto& file : snapshot.files)
            if (AssetKey(QString::fromStdWString(file.relativePath.wstring())) == wanted) {
                if (found) return Fail(spec, "在目录中存在大小写或斜杠歧义。");
                found = &file;
            }
        if (!found) return Output::Success(std::nullopt);
        auto bytes = ReadVerifiedFile(snapshot.root / found->relativePath, found->sha256, spec.maxBytes);
        if (!bytes.Succeeded() || !bytes.value) return {std::nullopt, bytes.diagnostics};
        return Output::Success(Bytes(std::move(*bytes.value)));
    }
    return Fail(spec, "无法读取：工程缺少可核验的地图或目录来源。");
}

} // namespace War3Map::QtProject
