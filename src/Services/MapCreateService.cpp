#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <exception>
#include <utility>

namespace War3Map::Services {
namespace {
using CreateResult = Result<MapCreateReceipt>;

template<class T> T Forward(std::vector<Diagnostic> diagnostics) {
    T result;
    result.diagnostics = std::move(diagnostics);
    if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) {
        return d.severity == DiagnosticSeverity::Error;
    })) result.diagnostics.push_back({DiagnosticSeverity::Error, "MapCreate", "CREATE_DEPENDENCY_FAILED",
        "新建地图的依赖步骤未返回完整结果。"});
    return result;
}

// 仅编码本配置的经典 W3E/WPM/HM3W 固定字段；其余格式复用既有序列化器。
struct Bytes {
    std::vector<std::byte> data;
    void Raw(std::string_view text) {
        for (const unsigned char c : text) data.push_back(static_cast<std::byte>(c));
    }
    void UInt(std::uint32_t value, unsigned count = 4) {
        for (unsigned i = 0; i < count; ++i) data.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
    }
    void Float(float value) { UInt(std::bit_cast<std::uint32_t>(value)); }
    void Text(std::string_view text) { Raw(text); UInt(0, 1); }
};

std::vector<std::byte> Terrain() {
    Bytes b;
    b.Raw("W3E!"); b.UInt(11); b.UInt('L', 1); b.UInt(0);
    b.UInt(6); b.Raw("LdrtLdroLdrgLrokLgrsLgrd"); b.UInt(2); b.Raw("CLdiCLgr");
    b.UInt(65); b.UInt(65); b.Float(-4096); b.Float(-4096);
    for (unsigned y = 0; y != 65; ++y) for (unsigned x = 0; x != 65; ++x) {
        const bool edge = x < 64 && y < 64 && (x < 6 || x >= 58 || y < 4 || y >= 56);
        b.UInt(8192, 2); b.UInt(edge ? 24576 : 8192, 2); b.UInt(0, 1);
        // 固定变体保证重复生成一致；不复制原版随机纹理分布或猜测其 PRNG。
        b.UInt(0, 1); b.UInt(0xf2, 1);
    }
    return std::move(b.data);
}

std::vector<std::byte> Pathing() {
    Bytes b; b.Raw("MP3W"); b.UInt(0); b.UInt(256); b.UInt(256);
    for (unsigned y = 0; y != 256; ++y) for (unsigned x = 0; x != 256; ++x)
        b.UInt(x < 24 || x >= 232 || y < 16 || y >= 224 ? 0xce : 0x40, 1);
    return std::move(b.data);
}

MapData::MapSettings Settings(const MapCreateOptions& options) {
    MapData::MapSettings s;
    s.formatVersion = 25; s.saves = 0; s.editorVersion = 6052;
    s.mapName = options.name; s.author = options.author; s.recommendedPlayers = "1";
    s.cameraBounds = {-2816, -3328, 2816, 2816, -2816, 2816, 2816, -3328};
    s.cameraMargins = {6, 6, 4, 8}; s.playableWidth = 52; s.playableHeight = 52;
    s.flags = 56340; s.tileset = 'L'; s.loadingBackground = -1; s.gameDataSet = -1;
    s.environment.fogStart = 3000; s.environment.fogEnd = 5000; s.environment.fogDensity = 0.5f;
    s.environment.fogColor.alpha = 255; s.environment.waterColor = {255, 255, 255, 255};
    MapData::MapPlayerSettings player;
    player.controller = 1; player.race = 1; player.name = "Player 1";
    s.players.push_back(std::move(player));
    s.forces.push_back({0, 0xffffffffu, "Force 1"});
    s.custom.emplace();
    return s;
}

Result<std::vector<MapArchiveMember>> Seed(const MapCreateOptions& options,
    const Trigger::TriggerDocument& triggers, const Trigger::UiCatalog& catalog) {
    using SeedResult = Result<std::vector<MapArchiveMember>>;
    const auto settings = Settings(options).Serialize();
    const auto encoded = Trigger::WtgWctCodec{}.Serialize(triggers, catalog);
    MapData::UnitPlacements units;
    MapData::UnitPlacement start;
    start.rawcode = "sloc"; start.scale = {1, 1, 1}; start.rotation = 4.71238898f;
    start.flags = 2; start.hitPoints = 0; start.mana = 0;
    units.units.push_back(start);
    const auto placed = units.Serialize();
    const auto doodads = MapData::DoodadPlacements{}.Serialize();
    if (!settings.Succeeded()) return Forward<SeedResult>(settings.diagnostics);
    if (!encoded.Succeeded()) return Forward<SeedResult>(encoded.diagnostics);
    if (!placed.Succeeded()) return Forward<SeedResult>(placed.diagnostics);
    if (!doodads.Succeeded()) return Forward<SeedResult>(doodads.diagnostics);
    Bytes regions; regions.UInt(5); regions.UInt(0);
    Bytes cameras; cameras.UInt(0); cameras.UInt(0);
    return SeedResult::Success({{"war3map.w3i", *settings.value}, {"war3map.w3e", Terrain()},
        {"war3map.wpm", Pathing()}, {"war3mapUnits.doo", *placed.value}, {"war3map.doo", *doodads.value},
        {"war3map.wtg", encoded.value->wtg}, {"war3map.wct", encoded.value->wct},
        {"war3map.w3r", std::move(regions.data)}, {"war3map.w3c", std::move(cameras.data)},
        {"war3map.wts", {}}, {"war3map.shd", std::vector<std::byte>(256 * 256)},
        {"war3map.j", {}}});
}

// 与界面新建触发器同一编辑接口：初始化事件 + 若干“自定义代码”动作，生成的脚本照常经保存链检查。
Status AddInitTrigger(Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog, const MapCreateOptions& options) {
    using Trigger::TriggerDocumentEditor;
    const auto category = Trigger::TriggerDirectoryEditor::AddCategory(document, options.initTriggerName);
    if (!category.Succeeded() || !category.value) return {category.diagnostics};
    const auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, options.initTriggerName);
    if (!trigger.Succeeded() || !trigger.value) return {trigger.diagnostics};
    const auto event = TriggerDocumentEditor::AppendRootEca(document, catalog, *trigger.value,
        Trigger::UiDefinitionKind::Event, "MapInitializationEvent");
    if (!event.Succeeded()) return {event.diagnostics};
    for (const auto& line : options.initCustomScript) {
        const auto action = TriggerDocumentEditor::AppendRootEca(document, catalog, *trigger.value,
            Trigger::UiDefinitionKind::Action, "CustomScriptCode");
        if (!action.Succeeded() || !action.value) return {action.diagnostics};
        const auto* eca = TriggerDocumentEditor::FindEca(document, action.value->rootEcaId);
        if (!eca || eca->arguments.size() != 1)
            return Status::Failure("MapCreate", "CREATE_TRIGGER_DEFINITION", "当前函数库的“自定义代码”动作不是单参数定义。");
        const auto set = TriggerDocumentEditor::SetScalarArgument(document,
            document.argumentNodes[eca->arguments[0]].id, 3, line);
        if (!set.Succeeded()) return set;
    }
    return Status::Ok();
}

bool TextValid(std::string_view text, std::size_t maximum) {
    return text.size() <= maximum && std::none_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 32 || c == 127;
    }) && (text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0) > 0);
}
} // namespace

Status ValidateMapCreateOptions(const MapCreateOptions& options) {
    if (options.profile != kClassicFlatMapProfile)
        return Status::Failure("MapCreate", "CREATE_PROFILE_UNSUPPORTED", "该新建地图配置尚未实现；不能外推尺寸或地形规则。");
    if (options.name.empty() || !TextValid(options.name, 128) || !TextValid(options.author, 128))
        return Status::Failure("MapCreate", "CREATE_TEXT_INVALID", "地图名称不能为空；名称和作者须为至多 128 字节的 UTF-8 单行文本。");
    if (!options.initCustomScript.empty() && (options.initTriggerName.empty() || !TextValid(options.initTriggerName, 128)
            || std::any_of(options.initCustomScript.begin(), options.initCustomScript.end(),
                [](const auto& line) { return line.empty() || !TextValid(line, 1024); })))
        return Status::Failure("MapCreate", "CREATE_TRIGGER_INVALID", "初始化触发器需要名称；每行自定义代码须为非空单行 UTF-8 文本。");
    std::size_t extraBytes = 0;
    for (const auto& member : options.extraMembers) {
        extraBytes += member.data.size();
        const auto name = std::string_view(member.name);
        if (name.empty() || name.size() > 240 || name.starts_with("war3map") || name.starts_with("(")
            || name.find("..") != std::string_view::npos || name.find(':') != std::string_view::npos
            || name.front() == '\\' || name.front() == '/' || !TextValid(name, 240) || extraBytes > 16u * 1024u * 1024u)
            return Status::Failure("MapCreate", "CREATE_EXTRA_MEMBER_INVALID",
                "附加文件须为地图内相对路径，不能与地图自身成员同名，合计不超过 16 MiB。");
    }
    return Status::Ok();
}

CreateResult CreateMapCandidate(const MapCreateRequest& request, const MapSaveService::Progress& progress) {
    try {
        const auto valid = ValidateMapCreateOptions(request.options);
        if (!valid.Succeeded()) return Forward<CreateResult>(valid.diagnostics);
        std::error_code error;
        if (request.target.empty() || request.workspace.empty() || request.toolRoot.empty() || request.gameRoot.empty()
            || std::filesystem::exists(request.target, error) || error)
            return CreateResult::Failure("MapCreate", "CREATE_PATH_INVALID", "需要完整数据根、不存在的目标地图与独占工作目录。");
        auto catalog = Trigger::UiCatalog::Load(request.uiRoot.empty() ? request.toolRoot : request.uiRoot);
        if (!catalog.Succeeded() || !catalog.value) return Forward<CreateResult>(std::move(catalog.diagnostics));
        auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
        if (!empty.Succeeded() || !empty.value) return Forward<CreateResult>(std::move(empty.diagnostics));
        auto triggers = std::move(*empty.value);
        if (!request.options.initCustomScript.empty()) {
            const auto added = AddInitTrigger(triggers, *catalog.value, request.options);
            if (!added.Succeeded()) return Forward<CreateResult>(added.diagnostics);
        }
        auto members = Seed(request.options, triggers, *catalog.value);
        if (!members.Succeeded() || !members.value) return Forward<CreateResult>(std::move(members.diagnostics));
        members.value->insert(members.value->end(), request.options.extraMembers.begin(), request.options.extraMembers.end());
        if (!std::filesystem::create_directory(request.workspace, error) || error)
            return CreateResult::Failure("MapCreate", "CREATE_WORKSPACE_UNAVAILABLE", "工作目录必须不存在且父目录可写；未覆盖原目录。");
        Bytes prefix; prefix.Raw("HM3W"); prefix.UInt(0); prefix.Text(request.options.name);
        prefix.UInt(56340); prefix.UInt(1); prefix.data.resize(512);
        const auto seed = WriteVerifiedArchive(request.workspace / "uncompiled-seed.mpq", *members.value, prefix.data);
        if (!seed.Succeeded() || !seed.value) return Forward<CreateResult>(seed.diagnostics);
        auto opened = MapArchiveSession::Open(seed.value->path);
        if (!opened.Succeeded() || !opened.value) return Forward<CreateResult>(std::move(opened.diagnostics));

        // 新建地图的派生小地图也必须消费已写入并重新打开的真实 seed。
        // MapDataEditSession 只借用解析后的基础成员；W3E/WPM 仍由同一
        // seed 会话精确读取并 Attach，不复制 TerrainMinimapSave 的算法。
        const auto terrainBytes = opened.value->ReadMember("war3map.w3e");
        const auto pathingBytes = opened.value->ReadMember("war3map.wpm");
        if (!terrainBytes.Succeeded() || !terrainBytes.value) return Forward<CreateResult>(terrainBytes.diagnostics);
        if (!pathingBytes.Succeeded() || !pathingBytes.value) return Forward<CreateResult>(pathingBytes.diagnostics);
        const auto seedDocument = MapDocument::FromMembers(opened.value->Members(), opened.value->SourcePath());
        MapData::MapDataEditSession minimapEdits(seedDocument);
        auto attached = minimapEdits.AttachTerrain(*terrainBytes.value);
        if (!attached.Succeeded()) return Forward<CreateResult>(attached.diagnostics);
        attached = minimapEdits.AttachPathing(*pathingBytes.value);
        if (!attached.Succeeded()) return Forward<CreateResult>(attached.diagnostics);

        MapSaveTask task; task.target = request.target; task.requireGeneratedNativeByteEquality = true;
        const auto frozen = task.FreezeArchive(*opened.value);
        if (!frozen.Succeeded()) return Forward<CreateResult>(frozen.diagnostics);
        ScriptSaveInputSources sources;
        sources.layers = catalog.value->Layers(); sources.toolRoot = request.toolRoot; sources.gameRoot = request.gameRoot;
        sources.uiRoot = request.uiRoot;
        sources.catalog = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
        sources.triggers = std::make_unique<Trigger::TriggerDocument>(std::move(triggers));
        sources.scriptMember = "war3map.j";
        const auto prepared = PrepareScriptSaveInputs(task, std::move(sources));
        if (!prepared.Succeeded() || !prepared.value || *prepared.value != ScriptSavePreparationState::Ready)
            return Forward<CreateResult>(prepared.diagnostics);
        task.diagnostics.insert(task.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
        std::shared_ptr<const TerrainMinimapSave> minimapSave;
        std::shared_ptr<const TerrainMinimapPreserveGuard> minimapPreserve;
        task.prepareFinalDerived = [&task, &minimapEdits, &request, &minimapSave, &minimapPreserve](MapSaveTask&) -> Status {
            const auto canonical = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(c == '\\' ? '/' : std::tolower(c));
                });
                return value;
            };
            // 脚本阶段可能附加最终 native/W3I/摆放替换；先把这些最终正文
            // 交给同一个 MapDataEditSession，TerrainMinimapSave 才能绑定
            // 小地图与最终列表，而不是只看到 seed 的旧数据。
            for (const auto& member : task.replacements) {
                const auto name = canonical(member.name);
                Status synced = Status::Ok();
                if (name == "war3map.w3e") synced = minimapEdits.SetTerrain(member.data);
                else if (name == "war3map.wpm") synced = minimapEdits.SetPathing(member.data);
                else if (name == "war3map.w3i") synced = minimapEdits.SetSerialized(MapDataKind::Settings, member.data);
                else if (name == "war3mapunits.doo") synced = minimapEdits.SetSerialized(MapDataKind::Units, member.data);
                else if (name == "war3map.doo") synced = minimapEdits.SetSerialized(MapDataKind::Doodads, member.data);
                else if (name == "war3map.w3r") synced = minimapEdits.SetSerialized(MapDataKind::Regions, member.data);
                else if (name == "war3map.w3c") synced = minimapEdits.SetSerialized(MapDataKind::Cameras, member.data);
                else if (name == "war3map.w3s") synced = minimapEdits.SetSerialized(MapDataKind::Sounds, member.data);
                else if (name == "war3map.wts") synced = minimapEdits.SetSerialized(MapDataKind::Strings, member.data);
                if (!synced.Succeeded()) return synced;
            }
            auto generated = TerrainMinimapSave::PrepareAutomaticOrPreserve(task, minimapEdits, request.gameRoot);
            if (!generated.Succeeded() || !generated.value) return {generated.diagnostics};
            task.diagnostics.insert(task.diagnostics.end(), generated.diagnostics.begin(), generated.diagnostics.end());
            minimapSave = std::move(generated.value->generated);
            minimapPreserve = std::move(generated.value->preserved);
            if (!minimapSave) {
                if (minimapPreserve) {
                    const auto checked = minimapPreserve->VerifyCurrent(task, minimapEdits);
                    if (!checked.Succeeded()) return checked;
                    task.beforePublish = [&task, minimapPreserve] { return minimapPreserve->VerifyFrozenOutput(task); };
                }
                return Status::Ok();
            }
            task.replacements.push_back(minimapSave->MinimapReplacement());
            const auto current = minimapSave->VerifyCurrent(task, minimapEdits, request.gameRoot);
            if (!current.Succeeded()) return current;
            task.beforePublish = [&task, minimapSave] { return minimapSave->VerifyFrozenOutput(task); };
            return Status::Ok();
        };
        MapSaveService::Run(task, progress);
        if (!task.succeeded || !task.scriptRebuilt || !task.nextSession) return Forward<CreateResult>(std::move(task.diagnostics));
        if (minimapSave) {
            const auto readback = minimapSave->VerifyReadback(task, *task.nextSession);
            task.diagnostics.insert(task.diagnostics.end(), readback.diagnostics.begin(), readback.diagnostics.end());
            if (!readback.Succeeded()) return Forward<CreateResult>(std::move(task.diagnostics));
        }
        if (minimapPreserve) {
            const auto readback = minimapPreserve->VerifyReadback(task, *task.nextSession);
            task.diagnostics.insert(task.diagnostics.end(), readback.diagnostics.begin(), readback.diagnostics.end());
            if (!readback.Succeeded()) return Forward<CreateResult>(std::move(task.diagnostics));
        }
        auto result = CreateResult::Success({task.written, task.nextSession->SourceSha256(),
            request.options.profile, task.memberCount, true});
        FinalizeScriptWorkspace(task);
        result.diagnostics = std::move(task.diagnostics);
        return result;
    } catch (const std::exception&) {
        return CreateResult::Failure("MapCreate", "CREATE_FAILED", "新建地图发生异常；请核对目标与工作目录中的诊断，未切换当前工程。");
    }
}
} // namespace War3Map::Services
