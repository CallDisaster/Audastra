#include "EditorProject.h"
#include "../QtCommon/DiagnosticTimings.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectFreeze.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"
#include <algorithm>
#include <array>
#include <QDir>
#include <QUndoStack>

// 地图数据（地图信息、放置物、区域、镜头、声音等）：按当前来源重新读取只读文档。
namespace War3Map::QtProject {

Status EditorProject::ConfigureRealtimeTerrainPathing() {
    const auto fail = [](const char* code, const char* message) {
        return Status::Failure("Terrain.Edit", code, message);
    };
    if (!mapEdits_ || !mapEdits_->Terrain())
        return fail("TERRAIN_REALTIME_PATHING_SOURCE", "当前没有可编辑的地形会话。");
    // 没有可读WPM时仍沿用原地形编辑/保存资格，查询明确不可用，不能捏造源通行位。
    if (!mapEdits_->HasTerrainPathing()) return Status::Ok();
    if (sourceChangeSuspected_ || sourceChangeConfirmed_)
        return fail("TERRAIN_REALTIME_PATHING_SOURCE_CHANGED", "地图来源可能已变化，请先重新核对来源。");
    if (HasPendingAssetEdits())
        return fail("TERRAIN_REALTIME_PATHING_PENDING_ASSETS", "资源修改尚未冻结，请先保存或撤销资源修改。");
    constexpr std::size_t maximumBytes = 64u * 1024u * 1024u;
    std::size_t owned = 0;
    std::vector<MemberReplacement> members;
    for (std::size_t index = 0; index < objectDirty_.size(); ++index) {
        if (!objectDirty_[index]) continue;
        const auto& native = nativeObjectDocuments_[index];
        const auto& edited = objectDocuments_[index];
        if (!native || !edited)
            return fail("TERRAIN_REALTIME_PATHING_OBJECT_BASELINE", "物编基线尚未就绪，不能推测当前通行。");
        const auto type = native->Type();
        if (edited->Objects().empty() && archiveSession_ && !archiveSession_->HasMember(ObjectData::NativeFileName(type)))
            continue;
        ObjectData::NativeObjectFreezeOptions options;
        options.outputType = type;
        options.maximumBytes = std::min<std::size_t>(8u * 1024u * 1024u, maximumBytes - owned);
        options.projectionFailure = {"TERRAIN_REALTIME_PATHING_OBJECT_FREEZE", "物编修改未能冻结。"};
        options.serializationFailure = {"TERRAIN_REALTIME_PATHING_OBJECT_FREEZE", "物编修改未能序列化。"};
        options.budgetFailure = {"TERRAIN_REALTIME_PATHING_BUDGET", "物编覆盖超过实时通行预算。"};
        auto frozen = ObjectData::FreezeNativeObjectChanges(*native, *edited,
            objectFieldSchema_.get(), standardCatalog_.get(), options);
        if (!frozen.value) return fail(frozen.failure.code.c_str(), frozen.failure.message.c_str());
        if (frozen.value->bytes.capacity() > maximumBytes - owned)
            return fail("TERRAIN_REALTIME_PATHING_BUDGET", "物编覆盖保留容量超过实时通行预算。");
        owned += frozen.value->bytes.capacity();
        members.push_back({std::string(ObjectData::NativeFileName(type)), std::move(frozen.value->bytes)});
    }
    if (archiveSession_)
        Services::TerrainPathingSave::BindRealtimePathing(*mapEdits_, archiveSession_, war3GameRoot_, maximumBytes, std::move(members));
    else if (sourceArchivePath_.empty() && directorySnapshot_)
        Services::TerrainPathingSave::BindRealtimePathing(*mapEdits_, directorySnapshot_->Snapshot(), war3GameRoot_, maximumBytes, std::move(members));
    else return fail("TERRAIN_REALTIME_PATHING_SOURCE", "当前地形缺少匹配的归档或目录来源。");
    return Status::Ok();
}

void EditorProject::LoadMapDocument(bool afterMapSave) {
    if (!afterMapSave && mapDataUndoStack_) mapDataUndoStack_->clear();
    const auto report = [this](const Diagnostic& diagnostic, bool error) {
        AddDiagnostic(error ? tr("错误") : tr("警告"), tr("地图数据"),
            QString::fromUtf8(diagnostic.code), QString::fromUtf8(diagnostic.message));
    };
    QtCommon::DiagnosticTimingScope documentTiming("map-data-document-parse");
    mapDocument_.reset();
    if (archiveSession_) {
        mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*archiveSession_));
    } else if (readOnlyArchive_) {
        auto read = MapDocument::FromReadOnlyArchive(*readOnlyArchive_);
        for (const auto& diagnostic : read.diagnostics) report(diagnostic, true);
        if (read.Succeeded()) mapDocument_ = std::make_unique<MapDocument>(std::move(*read.value));
    } else if (sourceArchivePath_.empty() && directorySnapshot_) {
        auto read = MapDocument::FromDirectory(*directorySnapshot_);
        for (const auto& diagnostic : read.diagnostics) report(diagnostic, true);
        if (read.Succeeded()) mapDocument_ = std::make_unique<MapDocument>(std::move(*read.value));
    }
    documentTiming.Finish();
    QtCommon::DiagnosticTimingScope terrainTiming("map-data-terrain-attach");
    if (mapDocument_) {
        if (afterMapSave && mapEdits_) mapEdits_->CommitSaved(*mapDocument_);
        else {
            mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*mapDocument_);
            // 地形不在 MapDocument 中：从同一来源附上；读取或解析失败时地形只读显示，不可编辑。
            auto terrain = ReadMapMember("war3map.w3e");
            if (terrain.Succeeded() && terrain.value) {
                const auto attached = mapEdits_->AttachTerrain(*terrain.value);
                for (const auto& diagnostic : attached.diagnostics) report(diagnostic, false);
                if (attached.Succeeded()) {
                    auto pathing = ReadMapMember("war3map.wpm");
                    if (pathing.Succeeded() && pathing.value) {
                        const auto status = mapEdits_->AttachPathing(*pathing.value);
                        for (const auto& diagnostic : status.diagnostics) report(diagnostic, false);
                    } else for (const auto& diagnostic : pathing.diagnostics) report(diagnostic, false);
                }
            } else for (const auto& diagnostic : terrain.diagnostics) report(diagnostic, false);
        }
        for (const auto& diagnostic : mapDocument_->Diagnostics()) {
            report(diagnostic, diagnostic.severity == DiagnosticSeverity::Error);
        }
    } else mapEdits_.reset();
    terrainTiming.Finish();
    QtCommon::DiagnosticTimingScope notifyTiming("map-data-notify");
    emit MapDocumentChanged();
}

Result<std::vector<std::byte>> EditorProject::ReadMapMember(const std::string& name) const {
    const auto fail = [](std::string message) {
        return Result<std::vector<std::byte>>::Failure("MapData", "MAP_MEMBER_UNAVAILABLE", std::move(message));
    };
    if (archiveSession_) {
        if (archiveSession_->HasMember(name)) return archiveSession_->ReadMember(name);
        // 命名快照未列出不等于不存在；按准确名字在同一可信源租约内读取，不补进命名清单。
        const std::array<std::string, 1> names{name};
        ArchiveExactMemberLimits limits; // 共用硬上限：单成员及总计均为 64 MiB。
        limits.memberCount = 1;
        auto captured = archiveSession_->CaptureExactMembers(names, limits);
        if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
        auto& member = captured.value->front();
        if (!member.present) return fail("当前地图没有 " + name + "。");
        return Result<std::vector<std::byte>>::Success(std::move(member.bytes));
    }
    if (readOnlyArchive_) {
        std::error_code error;
        if (sourceArchivePath_.empty() || !std::filesystem::equivalent(sourceArchivePath_, readOnlyArchive_->SourcePath(), error) || error)
            return fail("当前地图路径与只读来源绑定不一致，不能读取 " + name + "。");
        return readOnlyArchive_->ReadMember(name, name == "war3map.wpm" ? 64ULL * 1024 * 1024 + 16 : 64ULL * 1024 * 1024);
    }
    if (!sourceArchivePath_.empty()) return fail("地图归档会话不可用，不能从另一来源推测 " + name + "。");
    if (!directorySnapshot_) return fail("当前工作区缺少可信来源快照，不能读取 " + name + "。");
    std::error_code error;
    if (projectRoot_.empty() || !std::filesystem::equivalent(projectRoot_, directorySnapshot_->Snapshot().root, error) || error)
        return fail("当前工作区路径与来源快照不一致，不能读取 " + name + "。");
    return ReadWorkspaceMember(*directorySnapshot_, name);
}

Result<std::vector<std::byte>> EditorProject::ReadWorkspaceMember(const DirectoryWorkspace& workspace, const std::string& name) {
    const auto fail = [](std::string message) {
        return Result<std::vector<std::byte>>::Failure("MapData", "MAP_MEMBER_UNAVAILABLE", std::move(message));
    };
    const auto expected = QString::fromStdString(name).toCaseFolded();
    const auto nested = QStringLiteral("map/") + expected;
    const WorkspaceFile* selected = nullptr;
    for (const auto& file : workspace.Snapshot().files) {
        const auto relative = QDir::fromNativeSeparators(QString::fromStdWString(file.relativePath.wstring())).toCaseFolded();
        if (relative != expected && relative != nested) continue;
        if (selected) return fail("目录同时存在多个 " + name + " 候选，来源不明确。");
        selected = &file;
    }
    if (!selected) return fail("当前地图来源快照没有 " + name + "。");
    const std::uint64_t maxBytes = name == "war3map.wpm" ? 64ULL * 1024 * 1024 + 16 : 64ULL * 1024 * 1024;
    if (selected->size > maxBytes) return fail(name + " 超过 64 MiB 读取上限。");
    auto bytes = ReadVerifiedFile(workspace.Snapshot().root / selected->relativePath, selected->sha256, maxBytes);
    if (!bytes.Succeeded() || !bytes.value) return bytes;
    if (bytes.value->size() != selected->size) return fail(name + " 的长度与工作区快照不同。");
    return bytes;
}

} // namespace War3Map::QtProject
