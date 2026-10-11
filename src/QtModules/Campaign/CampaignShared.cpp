#include "CampaignModule.h"
#include "EditorProject.h"
#include "ObjectModule.h"
#include "Icons/IconProvider.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QDir>
#include <QFileInfo>
#include <QScopedValueRollback>
#include <QTemporaryDir>
#include <QUndoStack>
#include <algorithm>
#include <set>

namespace War3Map::Modules {
void CampaignModule::ReloadShared() {
    const QScopedValueRollback guard(refreshing_, true);
    objects_->DetachDocuments();
    for (auto* stack : shared_->objectUndoStacks_) stack->clear();
    shared_->AssetUndoStack()->clear();
    for (auto& document : shared_->nativeObjectDocuments_) document.reset();
    for (auto& document : shared_->objectDocuments_) document.reset();
    shared_->objectDirty_.fill(false); shared_->pendingImports_.clear(); shared_->assetEdits_.clear();
    shared_->campaignScope_ = session_;
    shared_->sourceArchivePath_ = session_ ? session_->SourcePath() : std::filesystem::path{};
    shared_->mapDocument_.reset();
    if (!workspace_) workspace_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/Audastra-CampaignObjects-XXXXXX");
    shared_->projectRoot_ = workspace_->isValid() && session_ ? std::filesystem::path(workspace_->path().toStdWString()) : std::filesystem::path{};
    shared_->war3GameRoot_ = mainProject_.war3GameRoot_; shared_->uiDataRoot_ = mainProject_.uiDataRoot_;
    shared_->worldEditStrings_ = mainProject_.worldEditStrings_;
    shared_->standardCatalog_ = mainProject_.standardCatalog_ ? std::make_unique<ObjectData::StandardObjectCatalog>(*mainProject_.standardCatalog_) : nullptr;
    shared_->objectFieldSchema_ = mainProject_.objectFieldSchema_ ? std::make_unique<ObjectData::ObjectFieldSchema>(*mainProject_.objectFieldSchema_) : nullptr;
    shared_->iconProvider_->SetGameRoot(shared_->war3GameRoot_);
    shared_->iconProvider_->SetUiToolRoot(shared_->uiDataRoot_);
    shared_->iconProvider_->SetMapArchive(shared_->sourceArchivePath_);
    if (session_) {
        std::set<QString> names;
        for (std::size_t offset = 0;;) {
            const auto page = session_->Members(session_->Revision(), offset, 256);
            if (!page.Succeeded() || !page.value) { (void)Failed(page.diagnostics); return; }
            for (const auto& member : page.value->members) names.insert(QtProject::EditorProject::AssetKey(QString::fromStdString(member.path)));
            if (page.value->nextOffset >= page.value->total) break;
            offset = page.value->nextOffset;
        }
        if (names.contains(QStringLiteral("war3campaign.wts"))) {
            const auto strings = session_->ReadMember(session_->Revision(), "war3campaign.wts");
            if (strings.Succeeded() && strings.value) {
                // 仅名称展示的内存投影使用 MapDocument 的标准 WTS 槽；正文来自本战役，绝不取主地图的字符串。
                const std::array<MapArchiveMember, 1> source{{{"war3map.wts", *strings.value}}};
                shared_->mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(source, session_->SourcePath()));
            } else (void)Failed(strings.diagnostics);
        }
        for (std::size_t i = 0; i < shared_->objectDocuments_.size(); ++i) {
            const auto type = static_cast<ObjectData::ObjectType>(i);
            const auto name = Services::CampaignSession::ObjectMember(type);
            // 缺席与损坏不能混为一谈。只有清单证明缺席时才建立空的经典物编基线。
            const bool present = names.contains(QString::fromStdString(name));
            ObjectData::NativeObjectCodec codec(type);
            Result<std::vector<std::byte>> bytes;
            if (present) bytes = session_->ReadMember(session_->Revision(), name);
            else {
                auto empty = ObjectData::NativeObjectDocument::CreateEmpty(type, 2);
                if (!empty.Succeeded() || !empty.value) { (void)Failed(empty.diagnostics); continue; }
                bytes = codec.Serialize(*empty.value);
            }
            if (!bytes.Succeeded() || !bytes.value) { (void)Failed(bytes.diagnostics); continue; }
            auto native = codec.Parse(*bytes.value);
            auto* document = dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.get());
            if (!native.Succeeded() || !document || !document->IsEditable()) { (void)Failed(native.diagnostics); continue; }
            auto converted = ObjectData::ConvertNativeObjectDataToLni(type, *bytes.value);
            if (!converted.Succeeded() || !converted.value) { (void)Failed(converted.diagnostics); continue; }
            auto lni = ObjectData::LniObjectCodec(type).Parse(*converted.value);
            auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(lni.document.get());
            if (!lni.Succeeded() || !editable) { (void)Failed(lni.diagnostics); continue; }
            shared_->nativeObjectDocuments_[i] = std::make_unique<ObjectData::NativeObjectDocument>(std::move(*document));
            shared_->objectDocuments_[i] = std::make_unique<ObjectData::LniObjectDocument>(std::move(*editable));
        }
    }
    shared_->ResetEditTracking();
    shared_->NotifyImportsChanged();
    objects_->ResetView(); objects_->RefreshStandardCatalog(); objects_->SelectFirstType(); objects_->RefreshIcons();
}

bool CampaignModule::CommitShared() {
    if (!session_) return true;
    const bool dirty = shared_->HasPendingAssetEdits() || std::any_of(shared_->objectDirty_.begin(), shared_->objectDirty_.end(), [](bool value) { return value; });
    if (!dirty) return true;
    Services::CampaignCommand command; command.operation = Services::CampaignOperation::ReplaceSharedData;
    for (std::size_t i = 0; i < shared_->objectDirty_.size(); ++i) {
        if (!shared_->objectDirty_[i]) continue;
        if (!shared_->nativeObjectDocuments_[i] || !shared_->objectDocuments_[i]) {
            Notice(tr("共享物编缺少可编辑的原生基线，草稿保留。")); return false;
        }
        auto native = *shared_->nativeObjectDocuments_[i];
        const auto applied = ObjectData::ApplyLniChangesToNative(native, *shared_->objectDocuments_[i], shared_->objectFieldSchema_.get(), shared_->standardCatalog_.get());
        if (!applied.Succeeded()) return Failed(applied.diagnostics);
        auto bytes = ObjectData::NativeObjectCodec(static_cast<ObjectData::ObjectType>(i)).Serialize(native);
        if (!bytes.Succeeded() || !bytes.value) return Failed(bytes.diagnostics);
        command.replacements.push_back({Services::CampaignSession::ObjectMember(static_cast<ObjectData::ObjectType>(i)), std::move(*bytes.value)});
    }
    for (const auto& edit : shared_->assetEdits_) {
        if (!edit.to.isEmpty()) {
            auto bytes = session_->ReadMember(displayedRevision_, edit.from.toStdString());
            if (!bytes.Succeeded() || !bytes.value) return Failed(bytes.diagnostics);
            command.replacements.push_back({edit.to.toStdString(), std::move(*bytes.value)});
        }
        if (edit.to.isEmpty() || QtProject::EditorProject::AssetKey(edit.to) != QtProject::EditorProject::AssetKey(edit.from))
            command.removals.push_back(edit.from.toStdString());
    }
    for (const auto& pending : shared_->pendingImports_) {
        if (pending.runtimeLuaDraft) { Notice(tr("Lua 草稿尚未确认运行时目标，不能作为战役资源写出；请先导出草稿。")); return false; }
        std::vector<std::byte> bytes;
        if (pending.ownedPayload) bytes = *pending.ownedPayload;
        else {
            const QFileInfo info(pending.diskPath);
            if (!info.isFile() || info.size() != pending.size) { Notice(tr("导入来源已改变，请重新选择文件。")); return false; }
            const auto path = std::filesystem::path(pending.diskPath.toStdWString());
            const auto hash = Sha256File(path);
            if (!hash.Succeeded() || !hash.value) return Failed(hash.diagnostics);
            auto captured = ReadVerifiedFile(path, *hash.value, 64u * 1024u * 1024u);
            if (!captured.Succeeded() || !captured.value) return Failed(captured.diagnostics);
            bytes = std::move(*captured.value);
        }
        if (pending.size < 0 || bytes.size() != static_cast<std::uint64_t>(pending.size)) { Notice(tr("导入载荷长度不一致，草稿保留。")); return false; }
        command.replacements.push_back({pending.archivePath.toStdString(), std::move(bytes)});
    }
    const auto applied = session_->Apply(displayedRevision_, command);
    if (!applied.Succeeded()) return Failed(applied.diagnostics);
    displayedRevision_ = session_->Revision(); ReloadShared(); RebuildHistory(); emit Changed(); return true;
}
} // namespace War3Map::Modules
