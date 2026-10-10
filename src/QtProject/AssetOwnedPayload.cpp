#include "AssetOwnedPayload.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"
#include "War3MapCore/Text/SourceEncoding.h"

#include <QFileInfo>

#include <algorithm>

namespace War3Map::QtProject {
namespace {
template<class T> Result<T> Failed(std::string code, std::string message) {
    return Result<T>::Failure("Asset.Lua", std::move(code), std::move(message));
}
}

Result<OwnedAssetSource> ReadOwnedAsset(const EditorProject& project, const QString& path, std::size_t maxBytes) {
    const auto entries = project.AssetEntries();
    const auto key = EditorProject::AssetKey(path);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == key; });
    if (found == entries.end()) return Failed<OwnedAssetSource>("ASSET_SOURCE_MISSING", "当前有效资源中没有此路径。");
    if (found->ambiguous) return Failed<OwnedAssetSource>("ASSET_SOURCE_AMBIGUOUS", "当前资源路径具有多份来源，不能选择其中一份正文。");
    if (!maxBytes || maxBytes > kOwnedLuaAssetBytes || found->size < 0 || static_cast<std::uint64_t>(found->size) > maxBytes)
        return Failed<OwnedAssetSource>("ASSET_TEXT_BUDGET", "Lua 正文超过 2 MiB 编辑预算；来源保持原样。");
    std::shared_ptr<const std::vector<std::byte>> bytes = found->ownedPayload;
    if (bytes) {
        if (bytes->size() != static_cast<std::size_t>(found->size)) return Failed<OwnedAssetSource>("ASSET_PAYLOAD_SIZE", "内存载荷长度与资源记录不一致。");
    } else if (found->Pending()) {
        const QFileInfo disk(found->diskPath);
        if (!disk.isFile() || disk.size() != found->size) return Failed<OwnedAssetSource>("ASSET_SOURCE_CHANGED", "导入文件长度已改变，请重新选择文件。");
        const auto hash = Sha256File(std::filesystem::path(found->diskPath.toStdWString()));
        if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        const auto read = ReadVerifiedFile(std::filesystem::path(found->diskPath.toStdWString()), *hash.value, maxBytes);
        if (!read.Succeeded()) return {std::nullopt, read.diagnostics};
        bytes = std::make_shared<const std::vector<std::byte>>(std::move(*read.value));
    } else {
        if (project.archiveSession_) {
            auto read = project.archiveSession_->ReadMember(found->originalPath.toStdString());
            if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
            bytes = std::make_shared<const std::vector<std::byte>>(std::move(*read.value));
        } else if (project.directorySnapshot_) {
            const WorkspaceFile* member = nullptr;
            for (const auto& file : project.directorySnapshot_->Snapshot().files) {
                const auto name = QString::fromStdWString(file.relativePath.wstring());
                if (EditorProject::AssetKey(name) != EditorProject::AssetKey(found->originalPath)) continue;
                if (member) return Failed<OwnedAssetSource>("ASSET_SOURCE_AMBIGUOUS", "目录来源规范路径具有多份成员，不能选择其中一份正文。");
                member = &file;
            }
            if (!member) return Failed<OwnedAssetSource>("ASSET_SOURCE_MISSING", "当前目录快照中没有此实际成员。");
            const auto relative = member->relativePath;
            if (relative.empty() || relative.is_absolute() || relative.has_root_path()
                || std::any_of(relative.begin(), relative.end(), [](const auto& component) { return component == ".."; }))
                return Failed<OwnedAssetSource>("ASSET_DIRECTORY_PATH_UNSAFE", "目录来源不是安全的相对成员路径。");
            // 复用共同只读来源捕获：祖先和叶项均拒绝 reparse，内部按只读租约核对字节。
            auto read = Script::CaptureReadOnlyScriptSource(project.directorySnapshot_->Snapshot().root / relative, true, maxBytes);
            if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
            if (read.value->binding.sha256 != member->sha256 || read.value->binding.bytes != member->size)
                return Failed<OwnedAssetSource>("ASSET_SOURCE_CHANGED", "目录成员内容与打开时的 SHA 或长度不同，请重新载入来源。");
            bytes = std::make_shared<const std::vector<std::byte>>(std::move(read.value->bytes));
        } else return Failed<OwnedAssetSource>("ASSET_SOURCE_MISSING", "当前工程没有可核验的资源来源快照。");
    }
    if (bytes->size() != static_cast<std::size_t>(found->size) || bytes->size() > maxBytes)
        return Failed<OwnedAssetSource>("ASSET_SOURCE_CHANGED", "资源长度与当前来源快照不同，请重新载入。");
    const auto hash = Sha256(*bytes);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    OwnedAssetSource source;
    source.path = found->path; source.bytes = std::move(bytes); source.sha256 = *hash.value;
    source.originalPath = found->originalPath; source.diskPath = found->diskPath;
    source.archive = project.archiveSession_.get(); source.directory = project.directorySnapshot_.get();
    source.sourcePath = project.sourceArchivePath_; source.projectPath = project.projectFilePath_;
    source.sourceSha256 = source.archive ? source.archive->SourceSha256()
        : source.directory ? source.directory->Snapshot().aggregateSha256 : std::string{};
    source.generation = project.editRevision_.Generation(); source.runtimeLuaDraft = found->runtimeLuaDraft;
    return Result<OwnedAssetSource>::Success(std::move(source));
}

Status VerifyOwnedAsset(const EditorProject& project, const OwnedAssetSource& source) {
    const auto stale = [](const char* reason) {
        return Status::Failure("Asset.Lua", "ASSET_SOURCE_STALE", std::string(reason) + " Lua 草稿仍保留，请核验来源或重新载入。");
    };
    if (project.sourceChangeSuspected_ || project.sourceChangeConfirmed_) return stale("当前来源尚待核验。");
    if (project.archiveSession_.get() != source.archive || project.directorySnapshot_.get() != source.directory)
        return stale("当前工程会话已更换。");
    if (project.sourceArchivePath_ != source.sourcePath) return stale("当前地图来源路径已改变。");
    if (project.projectFilePath_ != source.projectPath) return stale("当前工程文件路径已改变。");
    if (project.editRevision_.Generation() != source.generation) return stale("当前工程已重新载入或重建编辑基线。");
    // 归档工程同时保留提取目录投影；捕获 sourceSha256 的权威是归档，否则才是目录。
    // 两者摘要不同，不能用一个归档 SHA 同时核验提取目录的 aggregate SHA。
    if (source.archive ? source.archive->SourceSha256() != source.sourceSha256
        : source.directory && source.directory->Snapshot().aggregateSha256 != source.sourceSha256)
        return stale("当前来源内容身份已改变。");
    const auto current = ReadOwnedAsset(project, source.path);
    if (!current.Succeeded()) return {current.diagnostics};
    if (current.value->path != source.path || current.value->sha256 != source.sha256
        || current.value->runtimeLuaDraft != source.runtimeLuaDraft)
        return Status::Failure("Asset.Lua", "ASSET_SOURCE_STALE", "Lua 文件的路径或正文已在其他页面修改；草稿仍保留，请放弃或重新载入。");
    return Status::Ok();
}

QString PutOwnedAsset(const EditorProject& project, AssetState& candidate, const QString& path,
    std::shared_ptr<const std::vector<std::byte>> bytes, bool runtimeLuaDraft) {
    if (const auto problem = EditorProject::CheckAssetPath(path); !problem.isEmpty()) return problem;
    if (!path.endsWith(".lua", Qt::CaseInsensitive)) return QObject::tr("此入口只编辑 .lua 文件。");
    if (!bytes || bytes->size() > kOwnedLuaAssetBytes) return QObject::tr("Lua 正文超过 2 MiB 编辑预算。");
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (text.find('\0') != std::string_view::npos) return QObject::tr("Lua 草稿不能包含 NUL 字符。");
    for (std::uint32_t offset = 0; offset < text.size();) {
        const auto scalar = Text::ReadSourceScalar(text, offset, Text::SourceEncoding::Utf8);
        if (!scalar.Succeeded() || !scalar.value) return QObject::tr("Lua 草稿必须是有效的 UTF-8；来源原字节保持不变。");
        offset += scalar.value->bytes;
    }
    const auto key = EditorProject::AssetKey(path);
    const auto entries = project.AssetEntries(candidate);
    if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == key && entry.ambiguous; }))
        return QObject::tr("资源路径具有多份来源，不能选择其中一份编辑。");
    // const shared_ptr 仍可能来自可变别名；发布前复制，保证撤销和恢复快照的字节恒定。
    bytes = std::make_shared<const std::vector<std::byte>>(*bytes);
    auto pending = std::find_if(candidate.imports.begin(), candidate.imports.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.archivePath) == key; });
    // 所有通过此入口拥有的新 Lua 正文均等待运行时档案；调用方不能降级已有角色。
    runtimeLuaDraft = runtimeLuaDraft || path.endsWith(".lua", Qt::CaseInsensitive)
        || (pending != candidate.imports.end() && pending->runtimeLuaDraft);
    if (pending == candidate.imports.end()) {
        candidate.imports.push_back({path, {}, static_cast<qint64>(bytes->size()), std::move(bytes), runtimeLuaDraft});
    } else {
        pending->archivePath = path; pending->diskPath.clear(); pending->size = static_cast<qint64>(bytes->size());
        pending->ownedPayload = std::move(bytes); pending->runtimeLuaDraft = runtimeLuaDraft;
    }
    return {};
}

} // namespace War3Map::QtProject
