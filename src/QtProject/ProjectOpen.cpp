#include "ProjectOpen.h"
#include "ProjectFileIO.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSet>
#include <algorithm>
#include <utility>

namespace War3Map::QtProject {
namespace {
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Path(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
QByteArray Array(const std::vector<std::byte>& value) {
    return QByteArray(reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size()));
}
} // namespace

PreparedProjectOpen::PreparedProjectOpen() = default;
PreparedProjectOpen::~PreparedProjectOpen() = default;

std::filesystem::path PreparedProjectOpen::BaselinePath() const {
    return root / (map ? "baseline/source.w3x" : "baseline/workspace");
}

Result<std::unique_ptr<PreparedProjectOpen>> PrepareProjectOpen(const std::filesystem::path& path,
    const std::filesystem::path& gameRoot) {
    using PreparedResult = Result<std::unique_ptr<PreparedProjectOpen>>;
    const auto fail = [](const QString& message) {
        return PreparedResult::Failure("工程", "PROJECT_OPEN_FAILED", message.toStdString());
    };
    auto opened = MapArchiveSession::Open(path);
    if (!opened.Succeeded() || !opened.value) return fail(QObject::tr("工程文件不完整"));
    auto& archive = *opened.value;
    const auto json = archive.ReadMember("project.json");
    if (!json.Succeeded()) return fail(QObject::tr("缺少工程清单"));
    const auto manifest = QJsonDocument::fromJson(Array(*json.value)).object();
    if (manifest["format"] != "forge-project" || manifest["version"].toInt() != 1) return fail(QObject::tr("不支持的工程版本"));
    const bool map = manifest["sourceKind"] == "archive";
    if (!map && manifest["sourceKind"] != "directory") return fail(QObject::tr("工程来源类型不正确"));
    auto extraction = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/ForgeProject-XXXXXX");
    if (!extraction->isValid()) return fail(QObject::tr("不能创建独立恢复目录"));
    const auto root = std::filesystem::path(extraction->path().toStdWString());
    QSet<QString> ownedImportMembers;
    for (const auto& value : manifest["imports"].toArray()) {
        const auto record = value.toObject();
        if ((record.contains("owned") && !record["owned"].isBool())
            || (record.contains("runtimeLuaDraft") && !record["runtimeLuaDraft"].isBool()))
            return fail(QObject::tr("导入载荷身份类型不正确"));
        if (record["owned"].toBool()) ownedImportMembers.insert(record["member"].toString().replace('\\', '/').toLower());
    }
    const auto extract = [&](std::string_view prefix, const std::filesystem::path& output) {
        for (const auto& member : archive.Members()) {
            auto name = member.name;
            std::replace(name.begin(), name.end(), '\\', '/');
            if (!Text(name).startsWith(Text(prefix), Qt::CaseInsensitive)) continue;
            if (prefix == "imports/" && ownedImportMembers.contains(Text(name).toLower())) continue;
            // MapArchiveSession 已验证虚拟路径；重新按路径组件确认目标不跳出根。
            const auto relative = std::filesystem::path(Text(name.substr(prefix.size())).toStdWString());
            if (relative.empty() || relative.is_absolute()) return false;
            for (const auto& part : relative) if (part == "..") return false;
            const auto target = output / relative;
            if (!QDir().mkpath(Path(target.parent_path()))) return false;
            QFile file(Path(target));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(Array(member.data)) != static_cast<qint64>(member.data.size())) return false;
        }
        return true;
    };
    if (!extract("baseline/", root / "baseline") || !extract("imports/", root / "imports")
        || !extract("definitions/", root / "definitions")) return fail(QObject::tr("无法完整提取工程基线"));
    std::unique_ptr<MapArchiveSession> baseline;
    std::unique_ptr<DirectoryWorkspace> workspace;
    if (map) {
        auto parsed = MapArchiveSession::Open(root / "baseline/source.w3x");
        if (!parsed.Succeeded() || !parsed.value) return fail(QObject::tr("工程地图基线损坏"));
        baseline = std::make_unique<MapArchiveSession>(std::move(*parsed.value));
    } else {
        auto openedWorkspace = DirectoryWorkspace::Open(root / "baseline/workspace");
        if (!openedWorkspace.Succeeded() || !openedWorkspace.value) return fail(QObject::tr("工程目录基线不完整"));
        workspace = std::make_unique<DirectoryWorkspace>(std::move(*openedWorkspace.value));
    }
    std::unique_ptr<Trigger::UiCatalog> catalog;
    const QString uiRoot = manifest["uiEmbedded"].toBool() ? Path(root / "definitions") : manifest["uiRoot"].toString();
    if (!manifest["uiSha256"].toString().isEmpty()) {
        auto loaded = Trigger::UiCatalog::Load(std::filesystem::path(uiRoot.toStdWString()));
        if (!loaded.Succeeded() || Text(loaded.value->Sha256()) != manifest["uiSha256"].toString()) return fail(QObject::tr("触发器定义已变化或缺失，请恢复工程使用的定义版本"));
        catalog = std::make_unique<Trigger::UiCatalog>(std::move(*loaded.value));
    }
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, 7> objects;
    std::array<std::vector<ObjectData::LniObjectDocument::CloneSource>, 7> cloneSources;
    std::array<std::string, 7> cloneNativeHashes;
    std::array<bool, 7> cloneNativePresent;
    cloneNativePresent.fill(true); // 旧工程只有实际存在的 native 来源。
    if (manifest.contains("objectCloneSources")) {
        if (!manifest["objectCloneSources"].isArray()) return fail(QObject::tr("工程克隆来源格式无效"));
        const auto categories = manifest["objectCloneSources"].toArray();
        if (categories.size() > 7) return fail(QObject::tr("工程克隆来源类别超过预算"));
        std::array<bool, 7> seen{};
        for (const auto& value : categories) {
            if (!value.isObject()) return fail(QObject::tr("工程克隆来源类别记录无效"));
            const auto category = value.toObject();
            if ((category.size() != 3 && category.size() != 4) || !category["kind"].isString()
                || !category["native_sha256"].isString() || !category["sources"].isArray())
                return fail(QObject::tr("工程克隆来源类别字段无效"));
            if (category.size() == 4 && !category["source_present"].isBool())
                return fail(QObject::tr("工程克隆来源 presence 字段无效"));
            int index = -1;
            for (int candidate = 0; candidate < 7; ++candidate)
                if (category["kind"].toString() == Text(ObjectData::ToString(static_cast<ObjectData::ObjectType>(candidate)))) index = candidate;
            if (index < 0 || seen[static_cast<std::size_t>(index)]) return fail(QObject::tr("工程克隆来源类别未知或重复"));
            seen[static_cast<std::size_t>(index)] = true;
            cloneNativePresent[static_cast<std::size_t>(index)] = category.contains("source_present")
                ? category["source_present"].toBool() : true;
            const auto nativeHash = category["native_sha256"].toString().toLatin1();
            if (nativeHash.size() != 64 || !std::all_of(nativeHash.begin(), nativeHash.end(), [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            })) return fail(QObject::tr("工程克隆来源 native SHA 格式无效"));
            const auto sources = category["sources"].toArray();
            if (sources.isEmpty() || static_cast<std::size_t>(sources.size()) > ObjectData::LniObjectDocument::kMaximumCloneSources)
                return fail(QObject::tr("工程克隆来源为空或超过预算"));
            auto& rows = cloneSources[static_cast<std::size_t>(index)];
            rows.reserve(static_cast<std::size_t>(sources.size()));
            for (const auto& source : sources) {
                if (!source.isObject()) return fail(QObject::tr("工程克隆来源记录无效"));
                const auto row = source.toObject();
                if (row.size() != 2 || !row["new_id"].isString() || !row["source_id"].isString())
                    return fail(QObject::tr("工程克隆来源 ID 字段无效"));
                if (row["new_id"].toString().toUtf8().size() != 4 || row["source_id"].toString().toUtf8().size() != 4)
                    return fail(QObject::tr("工程克隆来源 ID 必须是四字节"));
                rows.push_back({row["new_id"].toString().toStdString(), row["source_id"].toString().toStdString()});
            }
            cloneNativeHashes[static_cast<std::size_t>(index)] = category["native_sha256"].toString().toStdString();
        }
    }
    for (int i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectData::ObjectType>(i);
        const auto name = "overlay/table/" + std::string(ObjectData::LniFileName(type));
        if (!archive.HasMember(name)) {
            if (!cloneSources[i].empty()) return fail(QObject::tr("工程克隆来源缺少对应物编覆盖"));
            continue;
        }
        const auto bytes = archive.ReadMember(name);
        auto parsed = ObjectData::LniObjectCodec(type).Parse(*bytes.value);
        if (!parsed.Succeeded() || !parsed.document) return fail(QObject::tr("工程物编覆盖无法解析"));
        std::optional<std::vector<std::byte>> originalBytes;
        if (baseline && baseline->HasMember(ObjectData::NativeFileName(type))) {
            const auto original = baseline->ReadMember(ObjectData::NativeFileName(type));
            if (!original.Succeeded() || !original.value) return fail(QObject::tr("工程物编 native 基线读取失败"));
            originalBytes = *original.value;
        } else if (workspace && !cloneSources[i].empty()) {
            // 目录工程仅接受明确的根成员或 map/成员；多个候选不能猜测来源。
            const std::string rootName(ObjectData::NativeFileName(type));
            for (const auto& file : workspace->Snapshot().files) {
                const auto relative = file.relativePath.generic_string();
                if (relative != rootName && relative != "map/" + rootName) continue;
                if (originalBytes) return fail(QObject::tr("工程克隆来源存在多个 native 基线"));
                const auto original = ReadProjectFileBytes(root / "baseline/workspace" / file.relativePath);
                if (!original) return fail(QObject::tr("工程克隆来源 native 基线不可读"));
                originalBytes = *original;
            }
        }
        if (!cloneSources[i].empty() && originalBytes.has_value() != cloneNativePresent[i])
            return fail(QObject::tr("工程克隆来源 presence 与实际基线不符"));
        if (!originalBytes && !cloneSources[i].empty()) {
            const auto empty = ObjectData::NativeObjectDocument::CreateEmpty(type);
            if (!empty.Succeeded() || !empty.value) return fail(QObject::tr("缺失成员的经典空基线无法建立"));
            const auto encoded = ObjectData::NativeObjectCodec(type).Serialize(*empty.value);
            if (!encoded.Succeeded() || !encoded.value) return fail(QObject::tr("缺失成员的经典空基线不能编码"));
            originalBytes = *encoded.value;
        }
        if (originalBytes) {
            auto native = ObjectData::NativeObjectCodec(type).Parse(*originalBytes);
            const auto* document = dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.get());
            if (!native.Succeeded() || !document
                || !document->Supports(ObjectData::NativeObjectOperation::EditExistingValue))
                return fail(QObject::tr("物编基线不支持已有字段修改，不能以工程覆盖替代损坏成员"));
            if (!cloneSources[i].empty()) {
                const auto hash = Sha256(*originalBytes);
                if (!hash.Succeeded() || !hash.value || *hash.value != cloneNativeHashes[i])
                    return fail(QObject::tr("工程克隆来源 native SHA 与实际基线不符"));
                auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get());
                if (!editable || !editable->RestoreCloneSources(cloneSources[i], *document).Succeeded())
                    return fail(QObject::tr("工程克隆来源缺失、重复、循环、父级不符或目标已在基线中"));
            }
        }
        objects[i].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    }
    std::unique_ptr<Trigger::TriggerDocument> triggers;
    const bool wtg = archive.HasMember("overlay/map/war3map.wtg");
    const bool wct = archive.HasMember("overlay/map/war3map.wct");
    if (wtg != wct || (wtg && !catalog)) return fail(QObject::tr("触发器覆盖或定义不完整"));
    if (wtg) {
        if (baseline) {
            const bool baselineWtg = baseline->HasMember("war3map.wtg");
            const bool baselineWct = baseline->HasMember("war3map.wct");
            if (baselineWtg != baselineWct) return fail(QObject::tr("触发器基线不成对，不能用覆盖补齐损坏来源"));
            if (baselineWtg) {
                const auto originalWtg = baseline->ReadMember("war3map.wtg");
                const auto originalWct = baseline->ReadMember("war3map.wct");
                if (!Trigger::WtgWctCodec{}.Parse(*originalWtg.value, *originalWct.value, *catalog).Succeeded())
                    return fail(QObject::tr("触发器基线无法解析，不能直接以工程覆盖替代"));
            }
        }
        const auto a = archive.ReadMember("overlay/map/war3map.wtg");
        const auto b = archive.ReadMember("overlay/map/war3map.wct");
        auto parsed = Trigger::WtgWctCodec{}.Parse(*a.value, *b.value, *catalog);
        if (!parsed.Succeeded()) return fail(QObject::tr("触发器覆盖校验失败"));
        triggers = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    }
    Trigger::GuiConversionSnapshots guiSnapshots;
    constexpr std::string_view snapshotMember = "overlay/editor/gui-conversion-snapshots.bin";
    const bool hasSnapshots = archive.HasMember(snapshotMember);
    if (manifest.contains("guiConversionSnapshots") || hasSnapshots) {
        if (manifest["guiConversionSnapshots"].toInt(-1) != 1 || !hasSnapshots || !triggers || !catalog)
            return fail(QObject::tr("转换前快照版本未知或成员不完整"));
        const auto bytes = archive.ReadMember(snapshotMember);
        if (!bytes.Succeeded() || !bytes.value) return fail(QObject::tr("转换前快照不能读取"));
        const auto source = map ? "archive:" + baseline->SourceSha256() : "directory:" + workspace->Snapshot().aggregateSha256;
        auto restored = Trigger::GuiConversionSnapshots::Parse(*bytes.value, *triggers, *catalog, source);
        if (!restored.Succeeded() || !restored.value) return fail(QObject::tr("转换前快照已损坏或与工程来源、触发器正文不符"));
        guiSnapshots = std::move(*restored.value);
    }
    std::vector<PendingImport> imports;
    for (const auto& value : manifest["imports"].toArray()) {
        const auto record = value.toObject();
        const auto member = record["member"].toString();
        if (!member.startsWith("imports/") || member.contains("..") || !archive.HasMember(member.toStdString())) return fail(QObject::tr("导入快照不存在"));
        if (!Resources::CanonicalImportPath(record["path"].toString().toStdString()).Succeeded()) return fail(QObject::tr("导入资源路径无效"));
        if (record["owned"].toBool()) {
            const auto payload = archive.ReadMember(member.toStdString());
            if (!payload.Succeeded() || !payload.value || payload.value->size() > 2 * 1024 * 1024)
                return fail(QObject::tr("内存导入快照不存在或超过单文件预算"));
            const auto fingerprint = Sha256(*payload.value);
            if (!fingerprint.Succeeded() || QString::fromStdString(*fingerprint.value) != record["sha256"].toString())
                return fail(QObject::tr("内存导入快照SHA不一致"));
            PendingImport entry{record["path"].toString(), {}, static_cast<qint64>(payload.value->size())};
            entry.ownedPayload = std::make_shared<const std::vector<std::byte>>(*payload.value);
            entry.runtimeLuaDraft = record["runtimeLuaDraft"].toBool();
            imports.push_back(std::move(entry));
            continue;
        }
        const auto diskPath = Path(root / std::filesystem::path(member.toStdWString()));
        const auto extracted = War3Map::QtProject::ReadProjectFileBytes(
            std::filesystem::path(diskPath.toStdWString()));
        const auto payload = archive.ReadMember(member.toStdString());
        if (!extracted || !payload.Succeeded() || *extracted != *payload.value) return fail(QObject::tr("导入快照提取不完整"));
        imports.push_back({record["path"].toString(), diskPath, static_cast<qint64>(payload.value->size())});
        imports.back().runtimeLuaDraft = record["runtimeLuaDraft"].toBool();
    }
    std::optional<MapDocument> dataDocument;
    if (baseline) {
        dataDocument = MapDocument::FromArchive(*baseline);
    } else {
        auto parsed = MapDocument::FromDirectory(*workspace);
        if (!parsed.Succeeded() || !parsed.value) return fail(QObject::tr("工程地图数据来源不完整"));
        dataDocument = std::move(*parsed.value);
    }
    constexpr std::string_view terrainJournalMember = "overlay/editor/terrain-strokes.bin";
    const bool hasTerrainJournal = archive.HasMember(terrainJournalMember);
    if (hasTerrainJournal != manifest.contains("terrainStrokeJournal")
        || (hasTerrainJournal && (!manifest["terrainStrokeJournal"].isDouble() || manifest["terrainStrokeJournal"].toDouble() != 1)))
        return fail(QObject::tr("地形前向记录声明、载荷或版本不完整"));
    if (manifest.contains("terrainJournalWarning") && (!manifest["terrainJournalWarning"].isString()
        || manifest["terrainJournalWarning"].toString().size() > 1024))
        return fail(QObject::tr("地形草稿恢复警告格式无效"));
    std::string terrainJournalWarning = manifest["terrainJournalWarning"].toString().toStdString();
    std::optional<MapData::TerrainStrokeJournal> terrainJournal;
    if (hasTerrainJournal) {
        const auto payload = archive.ReadMember(terrainJournalMember);
        if (!payload.Succeeded() || !payload.value) return fail(QObject::tr("地形前向记录无法读取"));
        auto decoded = MapData::DecodeTerrainStrokeJournal(*payload.value);
        if (!decoded.Succeeded() || !decoded.value) return {{}, decoded.diagnostics};
        terrainJournal = std::move(*decoded.value);
    }
    std::vector<std::byte> journalSourceTerrain, journalSourcePathing;
    std::vector<MapData::MapDataEditSession::EditReceipt> terrainReceipts;
    std::size_t terrainCurrentIndex = 0;
    std::optional<std::size_t> terrainSavedIndex;
    auto mapDataEdits = std::make_unique<MapData::MapDataEditSession>(*dataDocument);
    {
        // 地形基线与覆盖：基线读取或解析失败时，存在地形覆盖的工程不能恢复。
        auto terrain = baseline
            ? (baseline->HasMember("war3map.w3e") ? baseline->ReadMember("war3map.w3e")
                : Result<std::vector<std::byte>>::Failure("Project", "TERRAIN_ABSENT", "基线没有地形"))
            : EditorProject::ReadWorkspaceMember(*workspace, "war3map.w3e");
        auto pathing = baseline
            ? (baseline->HasMember("war3map.wpm") ? baseline->ReadMember("war3map.wpm")
                : Result<std::vector<std::byte>>::Failure("Project", "PATHING_ABSENT", "基线没有通行性图"))
            : EditorProject::ReadWorkspaceMember(*workspace, "war3map.wpm");
        if (baseline) {
            // 命名快照缺项不证明MPQ缺失；同批精确读取未列名的W3E/WPM，不修改原清单。
            // 已命名成员沿原内存路径，避免将其既有容量另套进64MiB精确捕获合计帽。
            std::vector<std::string> unlisted;
            if (!baseline->HasMember("war3map.w3e")) unlisted.push_back("war3map.w3e");
            if (!baseline->HasMember("war3map.wpm")) unlisted.push_back("war3map.wpm");
            if (!unlisted.empty()) {
                ArchiveExactMemberLimits limits;
                limits.memberCount = unlisted.size(); // 单项/合计仍是原64MiB硬上限。
                auto captured = baseline->CaptureExactMembers(unlisted, limits);
                if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
                for (auto& member : *captured.value) {
                    if (!member.present) continue; // 仅真实Missing保留上面的原诊断及overlay拒绝。
                    auto& target = member.name == "war3map.w3e" ? terrain : pathing;
                    target = Result<std::vector<std::byte>>::Success(std::move(member.bytes));
                }
            }
        }
        const bool attached = terrain.Succeeded() && terrain.value && mapDataEdits->AttachTerrain(*terrain.value).Succeeded();
        const bool pathingAttached = attached && pathing.Succeeded() && pathing.value
            && mapDataEdits->AttachPathing(*pathing.value).Succeeded();
        if (terrainJournal) {
            if (!attached || !pathingAttached) return fail(QObject::tr("地形前向记录缺少可验证的实际 W3E/WPM 基线"));
            const auto actualSha = baseline ? baseline->SourceSha256() : workspace->Snapshot().aggregateSha256;
            if (terrainJournal->sourceSha256 != actualSha) return fail(QObject::tr("地形前向记录绑定了另一份工程基线"));
            // APR绑定原W3E/原WPM；先严格核格式和P/savedS，再允许资源缺失时降raw。
            if (!terrainJournal->baselinePathingLayers.empty()) {
                const auto layers = MapData::DecodePathingRestrictionLayers(*mapDataEdits->Terrain(), *pathing.value,
                    *pathing.value, terrainJournal->baselinePathingLayers);
                if (!layers.Succeeded()) return {{}, layers.diagnostics};
            }
            journalSourceTerrain = *terrain.value; journalSourcePathing = *pathing.value;
        }
        const bool terrainOverlay = archive.HasMember("overlay/map/war3map.w3e");
        const bool pathingOverlay = archive.HasMember("overlay/map/war3map.wpm");
        const bool restrictionDraft = archive.HasMember("overlay/editor/pathing-restrictions.bin");
        if (restrictionDraft != manifest.contains("pathingRestrictions")
            || (restrictionDraft && (!manifest["pathingRestrictions"].isDouble()
                || (manifest["pathingRestrictions"].toDouble() != 1 && manifest["pathingRestrictions"].toDouble() != 2
                    && manifest["pathingRestrictions"].toDouble() != 3)
                || (manifest["pathingRestrictions"].toDouble() == 1 && !pathingOverlay) || !pathingAttached)))
            return fail(QObject::tr("工程附加通行限制声明缺失、版本未知或没有对应 WPM 覆盖"));
        if (terrainOverlay || pathingOverlay || restrictionDraft) {
            auto nextTerrain = terrainOverlay ? archive.ReadMember("overlay/map/war3map.w3e") : std::move(terrain);
            auto nextPathing = pathingOverlay ? archive.ReadMember("overlay/map/war3map.wpm") : std::move(pathing);
            if (!attached || !nextTerrain.Succeeded() || !nextTerrain.value)
                return fail(QObject::tr("工程地形覆盖无法安全恢复：源 W3E 缺失或未能验证"));
            if (pathingAttached) {
                if (!nextPathing.Succeeded() || !nextPathing.value)
                    return fail(QObject::tr("工程通行性覆盖无法读取"));
                auto draft = restrictionDraft ? archive.ReadMember("overlay/editor/pathing-restrictions.bin")
                    : Result<std::vector<std::byte>>::Success({});
                if (!draft.Succeeded() || !draft.value)
                    return fail(QObject::tr("工程附加通行限制声明无法读取"));
                if (restrictionDraft && (draft.value->size() < 4
                    || std::to_integer<unsigned>((*draft.value)[3]) != manifest["pathingRestrictions"].toDouble()))
                    return fail(QObject::tr("工程附加通行限制声明版本与清单不一致"));
                const auto restored = restrictionDraft
                    ? mapDataEdits->SetTerrainAndPathingDraft(*nextTerrain.value, *nextPathing.value, *draft.value)
                    : mapDataEdits->SetTerrainAndPathing(*nextTerrain.value, *nextPathing.value);
                if (!restored.Succeeded())
                    return fail(QObject::tr("工程地形与通行性覆盖无法作为同一事务安全恢复"));
            } else {
                if (pathingOverlay) return fail(QObject::tr("工程通行性覆盖无法安全恢复：源 WPM 缺失、模糊或未能解析"));
                // 允许恢复未获 WPM 证据的地形草稿；正式地图保存仍须通过通行性门禁。
                if (!mapDataEdits->SetTerrain(*nextTerrain.value).Succeeded())
                    return fail(QObject::tr("工程地形覆盖无法安全恢复"));
            }
        }
    }
    constexpr std::array<std::string_view, 7> nativeDataNames{
        "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3r",
        "war3map.w3c", "war3map.w3s", "war3map.wts"};
    for (std::size_t index = 0; index < nativeDataNames.size(); ++index) {
        const auto memberName = "overlay/map/" + std::string(nativeDataNames[index]);
        if (!archive.HasMember(memberName)) continue;
        const auto payload = archive.ReadMember(memberName);
        if (!payload.Succeeded() || !payload.value) return fail(QObject::tr("工程地图覆盖缺失"));
        const auto status = mapDataEdits->SetSerialized(static_cast<MapDataKind>(index), *payload.value);
        if (!status.Succeeded()) return fail(QObject::tr("工程地图覆盖无法安全解析或写出"));
    }
    if (manifest.contains("mapDataRemovals") && !manifest["mapDataRemovals"].isArray())
        return fail(QObject::tr("工程地图删除清单格式错误"));
    for (const auto& value : manifest["mapDataRemovals"].toArray()) {
        if (!value.isString()) return fail(QObject::tr("工程地图删除条目格式错误"));
        const auto name = value.toString().toStdString();
        const auto found = std::find(nativeDataNames.begin(), nativeDataNames.end(), name);
        if (found == nativeDataNames.end() || archive.HasMember("overlay/map/" + name))
            return fail(QObject::tr("工程地图删除条目未知或与覆盖冲突"));
        const auto status = mapDataEdits->Remove(static_cast<MapDataKind>(found - nativeDataNames.begin()));
        if (!status.Succeeded()) return fail(QObject::tr("工程地图删除条目不能安全应用"));
    }
    if (terrainJournal) {
        auto restored = baseline
            ? Services::TerrainPathingSave::RestoreJournal(*baseline, *dataDocument, journalSourceTerrain,
                journalSourcePathing, *terrainJournal, *mapDataEdits, gameRoot)
            : Services::TerrainPathingSave::RestoreJournal(workspace->Snapshot(), *dataDocument, journalSourceTerrain,
                journalSourcePathing, *terrainJournal, *mapDataEdits, gameRoot);
        if (restored.Succeeded() && restored.value) {
            // 工厂已复核全部owners及来源；保持同一堆对象，真实回执中的session地址不变。
            terrainReceipts = std::move(restored.value->receipts);
            terrainCurrentIndex = restored.value->currentIndex;
            terrainSavedIndex = restored.value->savedIndex;
            if (terrainReceipts.size() > 128 || terrainCurrentIndex > terrainReceipts.size()
                || (terrainSavedIndex && *terrainSavedIndex > terrainReceipts.size()))
                return fail(QObject::tr("恢复的地形历史位置超出实际回执或128条撤销预算"));
            mapDataEdits = std::move(restored.value->session);
        } else {
            const bool invalid = std::any_of(restored.diagnostics.begin(), restored.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.code == "TERRAIN_JOURNAL_INVALID" || diagnostic.code == "TERRAIN_JOURNAL_BASELINE_MISMATCH"
                    || diagnostic.code == "TERRAIN_REPLAY_SOURCE_MISMATCH" || diagnostic.code == "TERRAIN_REPLAY_SOURCE_READ_FAILED"
                    || diagnostic.code == "TERRAIN_JOURNAL_OUTPUT_MISMATCH"
                    || diagnostic.code == "TERRAIN_HISTORY_SOURCE_MISMATCH"
                    || diagnostic.code == "TERRAIN_HISTORY_STATION_MISMATCH"
                    || diagnostic.code == "TERRAIN_HISTORY_REPLAY_FAILED";
            });
            if (invalid) return {{}, restored.diagnostics};
            if (baseline) {
                // 资源owner可能早于工厂末端来源回读失败；降raw前仍须确证完整实际基线未变。
                const auto unchanged = baseline->VerifySourceUnchanged();
                if (!unchanged.Succeeded()) return {{}, unchanged.diagnostics};
            }
            terrainJournalWarning = "草稿已完整保留；水刷资源缺失、依赖变化或重放预算不足，水刷来源未恢复。";
        }
    }
    std::vector<AssetEdit> assetEdits;
    for (const auto& value : manifest["assetEdits"].toArray()) {
        const auto record = value.toObject();
        AssetEdit edit{record["from"].toString(), record["to"].toString()};
        if (edit.from.isEmpty() || (!edit.to.isEmpty() && !EditorProject::CheckAssetPath(edit.to).isEmpty())) {
            return fail(QObject::tr("资源改动记录无效"));
        }
        assetEdits.push_back(std::move(edit));
    }
    if (workspace && !workspace->VerifyUnchanged().Succeeded())
        return fail(QObject::tr("工程目录基线在恢复期间发生变化，请重新打开工程"));
    auto candidate = std::make_unique<PreparedProjectOpen>();
    candidate->extraction = std::move(extraction);
    candidate->root = root;
    candidate->sourcePath = archive.SourcePath();
    candidate->sourceSha256 = archive.SourceSha256();
    candidate->map = map;
    candidate->recovery = manifest["recovery"].toBool();
    if (map && manifest["sourceMapPath"].isString() && !manifest["sourceMapPath"].toString().isEmpty()) {
        candidate->sourceMapPath = std::filesystem::path(manifest["sourceMapPath"].toString().toStdWString());
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate->sourceMapPath, error) && !error) {
            const auto original = Sha256File(candidate->sourceMapPath);
            const auto baseline = Sha256File(root / "baseline/source.w3x");
            candidate->sourceMapMatchesBaseline = original.Succeeded() && baseline.Succeeded()
                && original.value && baseline.value && *original.value == *baseline.value;
        }
    }
    candidate->uiRoot = std::filesystem::path(uiRoot.toStdWString());
    candidate->compilerRoot = std::filesystem::path(manifest["uiRoot"].toString().toStdWString());
    candidate->journalJson = QJsonDocument(manifest["journal"].toArray()).toJson(QJsonDocument::Compact).toStdString();
    candidate->objects = std::move(objects);
    candidate->triggers = std::move(triggers);
    candidate->guiConversionSnapshots = std::move(guiSnapshots);
    candidate->catalog = std::move(catalog);
    candidate->imports = std::move(imports);
    candidate->mapDataEdits = std::move(mapDataEdits);
    candidate->terrainReceipts = std::move(terrainReceipts);
    candidate->terrainCurrentIndex = terrainCurrentIndex;
    candidate->terrainSavedIndex = terrainSavedIndex;
    candidate->terrainJournalWarning = terrainJournalWarning;
    candidate->assetEdits = std::move(assetEdits);
    auto result = PreparedResult::Success(std::move(candidate));
    if (terrainJournal && terrainJournalWarning.empty()) {
        Diagnostic info;
        info.severity = DiagnosticSeverity::Info; info.subsystem = "Terrain.Journal";
        info.code = "TERRAIN_JOURNAL_RESTORED";
        info.message = terrainJournal->history
            ? "地形历史已从实际来源重放并核对保存站、撤销位置和完整草稿，可继续撤销及重做。地图保存仍须通过原生派生与回读检查。"
            : "前向笔划已从实际来源重放并核对完整草稿，可撤销本次恢复的地形前缀。地图保存仍须通过原生派生与回读检查。";
        result.diagnostics.push_back(std::move(info));
    }
    if (!terrainJournalWarning.empty()) {
        auto warning = Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_DRAFT_ONLY", terrainJournalWarning);
        for (auto& diagnostic : warning.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
        result.diagnostics = std::move(warning.diagnostics);
    }
    return result;
}

} // namespace War3Map::QtProject
