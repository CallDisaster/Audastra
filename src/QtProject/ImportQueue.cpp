#include "EditorProject.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUndoStack>

#include <algorithm>
#include <array>

// 会话级导入队列：只记录磁盘来源与归档内路径，文件字节在保存时按快照校验后读取。
namespace War3Map::QtProject {
namespace {

QString CanonicalArchivePath(QString path) {
    return path.replace('\\', '/');
}

} // namespace

QString EditorProject::SuggestImportPath(const QString& diskPath) {
    const QFileInfo info(diskPath);
    const QString ext = info.suffix().toLower();
    const QString fileName = info.fileName();
    // 按钮图标放进暴雪的标准文件夹：游戏按“CommandButtonsDisabled\DIS + 文件名”找暂停 / 禁用图标，
    // 放在别处时暂停画面会显示成纯绿方块。
    const QString upper = fileName.toUpper();
    if (ext == QLatin1String("blp") || ext == QLatin1String("tga")) {
        if (upper.startsWith(QLatin1String("DISBTN")) || upper.startsWith(QLatin1String("DISPAS"))) {
            return QStringLiteral("ReplaceableTextures\\CommandButtonsDisabled\\") + fileName;
        }
        if (upper.startsWith(QLatin1String("BTN"))) return QStringLiteral("ReplaceableTextures\\CommandButtons\\") + fileName;
        if (upper.startsWith(QLatin1String("PAS"))) return QStringLiteral("ReplaceableTextures\\PassiveButtons\\") + fileName;
    }
    // 按扩展名建议归档内默认路径；模型、地图与其他文件放在根目录。
    if (ext == QLatin1String("blp") || ext == QLatin1String("tga") ||
        ext == QLatin1String("png") || ext == QLatin1String("dds")) {
        return QStringLiteral("Textures\\") + fileName;
    }
    if (ext == QLatin1String("mp3") || ext == QLatin1String("wav") || ext == QLatin1String("flac")) {
        return QStringLiteral("Sounds\\") + fileName;
    }
    if (ext == QLatin1String("txt") || ext == QLatin1String("j") || ext == QLatin1String("ai")) {
        return QStringLiteral("Scripts\\") + fileName;
    }
    return fileName;
}

int EditorProject::AddImportFiles(const QStringList& diskPaths, const QString& folder) {
    auto candidate = CaptureAssets();
    const int added = AddImportFiles(candidate, diskPaths, folder);
    if (!PublishAssetState(std::move(candidate)).Succeeded()) return 0;
    return added;
}

int EditorProject::AddImportFiles(AssetState& state, const QStringList& diskPaths, const QString& folder) const {
    int added = 0;
    for (const QString& path : diskPaths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            continue;
        }
        PendingImport entry;
        entry.diskPath = QDir::toNativeSeparators(path);
        entry.archivePath = folder.isEmpty()
            ? (campaignScope_ ? QStringLiteral("war3campaignImported\\") + info.fileName() : SuggestImportPath(path))
            : folder + QLatin1Char('\\') + info.fileName();
        entry.size = info.size();
        // 同一归档路径（大小写与斜杠等价）只保留最后一次选择。
        const auto previous = std::find_if(state.imports.begin(), state.imports.end(), [&entry](const auto& value) {
            return CanonicalArchivePath(value.archivePath)
                .compare(CanonicalArchivePath(entry.archivePath), Qt::CaseInsensitive) == 0;
        });
        if (previous == state.imports.end()) {
            state.imports.push_back(std::move(entry));
        } else {
            entry.runtimeLuaDraft = previous->runtimeLuaDraft;
            *previous = std::move(entry);
        }
        ++added;
    }
    std::erase_if(state.edits, [&](const AssetEdit& edit) {
        return edit.to.isEmpty() && std::any_of(state.imports.begin(), state.imports.end(),
            [&](const PendingImport& pending) { return AssetKey(pending.archivePath) == AssetKey(edit.from); });
    });
    return added;
}

QString EditorProject::AddImportPayloads(std::vector<ImportPayload>& payloads, bool replaceExisting,
    const QString& label) {
    return RecordAssetEdit(label, [&](AssetState& state) {
        return PrepareImportPayloads(state, payloads, replaceExisting);
    });
}

QString EditorProject::PrepareImportPayloads(AssetState& state, std::vector<ImportPayload>& payloads,
    bool replaceExisting) const {
    if (payloads.empty()) return tr("没有要导入的文件。");
    const auto entries = AssetEntries(state);
    QSet<QString> requested;
    std::uint64_t totalBytes = 0;
    for (auto& payload : payloads) {
        if (const auto problem = CheckAssetPath(payload.archivePath); !problem.isEmpty())
            return payload.archivePath + QStringLiteral("：") + problem;
        if (!payload.bytes) return payload.archivePath + tr("：缺少文件内容。");
        if (payload.bytes->size() > Services::ImportPayloadPolicy::ownedFileBytes
            || payload.bytes->size() > Services::ImportPayloadPolicy::ownedBatchBytes - totalBytes)
            return tr("IMPORT_PAYLOAD_BUDGET · 单文件不能超过64MiB，内存导入合计不能超过256MiB。");
        totalBytes += payload.bytes->size();
        const auto key = AssetKey(payload.archivePath);
        if (requested.contains(key)) return payload.archivePath + tr("：同一路径出现多次。");
        requested.insert(key);
        const auto existing = std::find_if(entries.begin(), entries.end(),
            [&](const AssetEntry& entry) { return AssetKey(entry.path) == key; });
        if (existing != entries.end() && existing->ambiguous)
            return payload.archivePath + tr("：该路径有多份来源，未导入。");
        payload.replaced = existing != entries.end();
        if (!payload.replaced && archiveSession_) {
            // 未列入文件清单的成员也要按名字核对，不能当作不存在。
            const std::array<std::string, 1> names{payload.archivePath.toStdString()};
            const auto exact = archiveSession_->CaptureExactMembers(names);
            if (!exact.Succeeded() || !exact.value || exact.value->size() != 1)
                return payload.archivePath + tr("：无法确认地图里是否已有该文件，未导入。");
            payload.replaced = exact.value->front().present;
        }
        if (payload.replaced && !replaceExisting)
            return payload.archivePath + tr("：地图里已有该文件；确认覆盖时请明确要求替换。");
    }
    // 完整验证后才修改候选；复制正文，避免调用方的可变别名改变撤销快照。
    auto candidate = state;
    for (const auto& payload : payloads) {
        PendingImport entry;
        entry.archivePath = payload.archivePath;
        entry.size = static_cast<qint64>(payload.bytes->size());
        entry.ownedPayload = std::make_shared<const std::vector<std::byte>>(*payload.bytes);
        const auto key = AssetKey(entry.archivePath);
        const auto previous = std::find_if(candidate.imports.begin(), candidate.imports.end(),
            [&](const PendingImport& value) { return AssetKey(value.archivePath) == key; });
        if (previous == candidate.imports.end()) candidate.imports.push_back(std::move(entry));
        else { entry.runtimeLuaDraft = previous->runtimeLuaDraft; *previous = std::move(entry); }
        std::erase_if(candidate.edits, [&](const AssetEdit& edit) { return edit.to.isEmpty() && AssetKey(edit.from) == key; });
    }
    state = std::move(candidate);
    return {};
}

void EditorProject::RemoveImports(std::vector<int> rows) {
    // 从后往前删除避免索引失效。
    std::sort(rows.rbegin(), rows.rend());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : rows) {
        if (row >= 0 && row < static_cast<int>(pendingImports_.size())) {
            pendingImports_.erase(pendingImports_.begin() + row);
        }
    }
    NotifyImportsChanged(true);
}

void EditorProject::ClearImports() {
    pendingImports_.clear();
    NotifyImportsChanged(true);
}

void EditorProject::NotifyImportsChanged(bool userEdit) {
    // 整体打开或保存换了归档基线，旧资源收据不再属于当前来源。
    if (!userEdit && assetUndoStack_) assetUndoStack_->clear();
    // 导入文件属于保存范围：修订跟踪就绪时立即核对快照。
    if (editTrackingReady_) {
        (void)ObserveEditState("更新导入资源");
    }
    emit ImportsChanged();
    if (userEdit) {
        emit ImportsEdited();
    }
}

} // namespace War3Map::QtProject
