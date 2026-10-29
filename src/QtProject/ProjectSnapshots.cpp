#include "EditorProject.h"
#include "PendingImportPayload.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QCryptographicHash>

#include <algorithm>

namespace War3Map::QtProject {
namespace {
std::string ObjectScope(int index) {
    return "objects/" + std::string(ObjectData::LniFileName(static_cast<ObjectData::ObjectType>(index)));
}
QByteArray Array(const std::vector<std::byte>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}
} // namespace

std::optional<std::string> EditorProject::CaptureTriggerFingerprint() const {
    if (!triggerDocument_ || !uiCatalog_) return {};
    // 指纹只识别内容是否相同，字节与保存写出完全一致；函数库合法性由编辑命令与保存门禁负责，
    // 这里不再每次重复整图校验。
    const auto encoded = Trigger::WtgWctCodec{}.EncodeForIdentity(*triggerDocument_);
    if (!encoded.Succeeded() || !encoded.value) return {};
    const auto wtg = Sha256(encoded.value->wtg);
    const auto wct = Sha256(encoded.value->wct);
    if (!wtg.Succeeded() || !wct.Succeeded()) return {};
    return *wtg.value + ":" + *wct.value + ":" + uiCatalog_->Sha256();
}

std::optional<EditScopeSnapshot> EditorProject::CaptureEditScopes(const AssetState* candidateAssets) const {
    EditScopeSnapshot scopes;
    if (projectRoot_.empty()) return scopes;
    // 来源身份参与工程比较；另存地图以后工程必须保存新的完整基线。
    if (archiveSession_) scopes["source"] = "archive:" + archiveSession_->SourceSha256();
    else if (!sourceArchivePath_.empty()) scopes["source"] = "archive:incomplete";
    else if (directorySnapshot_) scopes["source"] = "directory:" + directorySnapshot_->Snapshot().aggregateSha256;
    else return {};
    const auto baseline = editTrackingReady_ ? editRevision_.Baseline(EditSaveTarget::Map) : std::nullopt;
    for (int i = 0; i < 7; ++i) {
        if (!objectDocuments_[i]) continue;
        const auto key = ObjectScope(i);
        // 创建后全部撤销，不凭空产生原地图没有的空类别。
        if (baseline && !baseline->scopes.contains(key) && objectDocuments_[i]->Objects().empty()) continue;
        scopes[key] = objectDocuments_[i]->DocumentFingerprint();
        if (scopes[key].empty()) return {};
    }
    if (triggerDocument_) {
        if (!uiCatalog_) return {};
        const auto value = CaptureTriggerFingerprint();
        if (!value) return {};
        scopes["triggers"] = *value;
    }
    // 使用导入顺序、虚拟路径和字节指纹，不使用磁盘时间戳冒充内容。
    const auto& imports = candidateAssets ? candidateAssets->imports : pendingImports_;
    const auto& edits = candidateAssets ? candidateAssets->edits : assetEdits_;
    const auto& approval = candidateAssets ? candidateAssets->runtimeApprovedPackageSha256 : runtimeApprovedPackageSha256_;
    if (!ValidatePendingImports(imports).Succeeded()) return {};
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const auto& entry = imports[i];
        const auto fingerprint = FingerprintPendingImport(entry);
        if (!fingerprint.Succeeded()) return {};
        scopes["imports/" + std::to_string(i)] = entry.archivePath.toStdString()
            + (entry.ownedPayload ? ":owned:" : ":disk:") + (entry.runtimeLuaDraft ? "lua:" : "asset:") + *fingerprint.value;
    }
    for (std::size_t i = 0; i < edits.size(); ++i) {
        scopes["assets/" + std::to_string(i)] = edits[i].from.toStdString() + ">" + edits[i].to.toStdString();
    }
    if (!approval.empty()) scopes["runtime/approval"] = approval;
    const auto mapScopes = CaptureMapEditScopes();
    if (!mapScopes) return {};
    scopes.insert(mapScopes->begin(), mapScopes->end());
    return scopes;
}

std::optional<EditScopeSnapshot> EditorProject::CaptureMapEditScopes() const {
    EditScopeSnapshot scopes;
    if (mapEdits_) {
        const auto journal = mapEdits_->TerrainJournalStateFingerprint();
        if (journal.empty()) return {};
        // 无前缀基线不增加scope，避免仅打开地图就产生虚假的地图净差。
        if (journal != "optional:empty") scopes["map/editor/terrain-journal"] = journal;
        if (!terrainJournalWarning_.empty()) scopes["map/editor/terrain-journal-warning"] = terrainJournalWarning_;
        for (const auto& member : mapEdits_->Replacements()) {
            const auto hash = Sha256(member.data);
            if (!hash.Succeeded()) return {};
            scopes["map/" + member.name] = *hash.value;
        }
        for (const auto& name : mapEdits_->Removals()) scopes["map/" + name] = "removed";
        const auto restrictions = mapEdits_->SerializePathingRestrictionDraft();
        if (!restrictions.Succeeded() || !restrictions.value) return {};
        if (!restrictions.value->empty()) {
            const auto hash = Sha256(*restrictions.value);
            if (!hash.Succeeded() || !hash.value) return {};
            scopes["map/editor/pathing-restrictions"] = *hash.value;
        }
    }
    return scopes;
}

std::string EditorProject::ProjectFingerprint() const {
    std::vector<std::string> imports;
    imports.reserve(pendingImports_.size());
    for (const auto& entry : pendingImports_) {
        const auto value = FingerprintPendingImport(entry);
        if (!value.Succeeded() || !value.value) return {};
        imports.push_back(*value.value);
    }
    return ProjectFingerprint(imports);
}

std::string EditorProject::ProjectFingerprint(std::span<const std::string> importSha256) const {
    if (importSha256.size() != pendingImports_.size()) return {};
    for (const auto& sha : importSha256) {
        if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const std::string& text) { hash.addData(QByteArray::fromStdString(text)); hash.addData(QByteArrayView("\0", 1)); };
    for (const auto& document : objectDocuments_) add(document ? document->DocumentFingerprint() : "absent");
    if (triggerDocument_ && uiCatalog_) {
        const auto encoded = Trigger::WtgWctCodec{}.Serialize(*triggerDocument_, *uiCatalog_);
        if (!encoded.Succeeded() || !encoded.value) return {};
        hash.addData(Array(encoded.value->wtg)); hash.addData(Array(encoded.value->wct));
    }
    add("gui-conversion-snapshots:" + (triggerDocument_ && uiCatalog_
        ? guiConversionSnapshots_.ActiveFingerprint(*triggerDocument_, *uiCatalog_) : std::string{}));
    for (std::size_t index = 0; index < pendingImports_.size(); ++index) {
        const auto& entry = pendingImports_[index];
        add(entry.archivePath.toStdString());
        add(entry.ownedPayload ? "owned" : "disk"); add(entry.runtimeLuaDraft ? "runtime-lua-draft" : "asset");
        add(importSha256[index]);
    }
    for (const auto& edit : assetEdits_) {
        add("asset:" + edit.from.toStdString());
        add(edit.to.toStdString());
    }
    if (!runtimeApprovedPackageSha256_.empty()) add("runtime-approval:" + runtimeApprovedPackageSha256_);
    if (mapEdits_) {
        const auto journal = mapEdits_->TerrainJournalStateFingerprint();
        if (journal.empty()) return {};
        add("terrain-journal:" + journal); add("terrain-journal-warning:" + terrainJournalWarning_);
        for (const auto& member : mapEdits_->Replacements()) {
            add("map:" + member.name);
            hash.addData(Array(member.data));
        }
        for (const auto& name : mapEdits_->Removals()) add("map-removed:" + name);
        const auto restrictions = mapEdits_->SerializePathingRestrictionDraft();
        if (!restrictions.Succeeded() || !restrictions.value) return {};
        add("pathing-intent:"); hash.addData(Array(*restrictions.value));
    }
    return hash.result().toHex().toStdString();
}

} // namespace War3Map::QtProject
