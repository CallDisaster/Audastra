#include "GameUiSources.h"

#include "EditorProject.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/Text/TextUtil.h"

#include <QObject>

#include <algorithm>
#include <set>

namespace War3Map::Modules {
namespace {

constexpr const char* kTocPath = "UI\\FrameDef\\FrameDef.toc";
constexpr const char* kSkinPath = "UI\\war3skins.txt";
constexpr const char* kMapSkinPath = "war3mapSkin.txt";
// 界面文本与贴图的单文件上限；游戏最大的界面贴图不到 1 MiB，中文字体约 10 MiB。
constexpr std::size_t kMaxUiFileBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxFontBytes = 32u * 1024u * 1024u;
// 地图脚本只用于查找加载界面目录的调用；超大脚本不读。
constexpr std::size_t kMaxScriptBytes = 64u * 1024u * 1024u;

std::string OriginLabel(const Resources::ResolvedResource& resource) {
    switch (resource.location.layer) {
    case Resources::ResourceLayer::SessionOverride: return QObject::tr("会话导入").toStdString();
    case Resources::ResourceLayer::MapImport: return QObject::tr("地图").toStdString();
    case Resources::ResourceLayer::Game:
        if (resource.location.readProvenance && !resource.location.readProvenance->containerIdentity.empty())
            return QObject::tr("游戏 · %1").arg(QString::fromStdString(resource.location.readProvenance->containerIdentity)).toStdString();
        return QObject::tr("游戏").toStdString();
    default: return resource.location.sourceId;
    }
}

} // namespace

GameUiSources CaptureGameUiSources(const QtProject::EditorProject& project) {
    GameUiSources sources;
    sources.gameRoot = project.war3GameRoot_;
    if (project.archiveSession_ || project.readOnlyArchive_) {
        auto captured = CaptureWorldTerrainSources(project);
        if (captured.Succeeded() && captured.value) {
            sources.map = std::move(*captured.value);
        } else if (!captured.diagnostics.empty()) {
            sources.mapNote = QString::fromStdString(captured.diagnostics.front().message);
        }
    } else if (!project.projectRoot_.empty()) {
        sources.mapNote = QObject::tr("目录工程的地图界面文件尚未接入，只显示游戏界面");
    }
    return sources;
}

struct GameUiReader::State {
    std::unique_ptr<WorldResourceReader> world;
    std::optional<Game::GameDataSource> game;
    bool opened = false;
};

GameUiReader::GameUiReader(GameUiSources sources) : sources_(std::move(sources)), state_(std::make_unique<State>()) {}
GameUiReader::~GameUiReader() = default;

Status GameUiReader::Open() {
    if (state_->opened) return Status::Ok();
    if (sources_.map) {
        auto opened = WorldResourceReader::Open(*sources_.map);
        if (!opened.Succeeded() || !opened.value) return Status{std::move(opened.diagnostics)};
        state_->world = std::move(*opened.value);
    } else {
        if (sources_.gameRoot.empty())
            return Status::Failure("GameUi", "GAMEUI_GAME_ROOT_MISSING", QObject::tr("未设置游戏目录").toStdString());
        auto mounted = Game::GameDataSource::Mount(sources_.gameRoot);
        if (!mounted.Succeeded() || !mounted.value) return Status{std::move(mounted.diagnostics)};
        state_->game.emplace(std::move(*mounted.value));
    }
    state_->opened = true;
    return Status::Ok();
}

Result<GameUiFile> GameUiReader::Read(const std::string& path) {
    if (const auto opened = Open(); !opened.Succeeded()) return {std::nullopt, opened.diagnostics};
    if (state_->world) {
        const auto current = state_->world->VerifyCurrentSource();
        if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
        auto resource = state_->world->Resolver().Read(path);
        if (!resource.Succeeded() || !resource.value) return {std::nullopt, std::move(resource.diagnostics)};
        const auto layer = resource.value->location.layer;
        GameUiFile file{std::move(resource.value->bytes), OriginLabel(*resource.value),
            layer == Resources::ResourceLayer::SessionOverride || layer == Resources::ResourceLayer::MapImport};
        return Result<GameUiFile>::Success(std::move(file));
    }
    auto data = state_->game->ReadFileBoundedWithSource(path, kMaxFontBytes);
    if (!data.Succeeded() || !data.value) return {std::nullopt, std::move(data.diagnostics)};
    GameUiFile file{std::move(data.value->bytes),
        QObject::tr("游戏 · %1").arg(QString::fromStdString(data.value->archiveName)).toStdString()};
    return Result<GameUiFile>::Success(std::move(file));
}

Result<std::vector<std::byte>> GameUiReader::ReadMapMember(const std::string& path, std::size_t maxBytes) {
    if (const auto opened = Open(); !opened.Succeeded()) return {std::nullopt, opened.diagnostics};
    if (!state_->world) return Result<std::vector<std::byte>>::Failure("GameUi", "GAMEUI_NO_MAP_LAYER", "没有打开地图");
    if (const auto current = state_->world->VerifyCurrentSource(); !current.Succeeded()) return {std::nullopt, current.diagnostics};
    return state_->world->ReadMapArchiveMember(path, maxBytes);
}

std::vector<std::string> GameUiReader::MapFiles() const {
    std::vector<std::string> files;
    if (!sources_.map) return files;
    files.reserve(sources_.map->entries.size());
    for (const auto& entry : sources_.map->entries) files.push_back(entry.path);
    return files;
}

namespace {

// 游戏只执行一份 JASS 脚本（先找根目录，再找 scripts 目录）；Lua 地图另有 war3map.lua。
// 脚本是地图数据、不在资源清单里，直接从已验证的地图归档读取（即最近一次保存的脚本）。
std::vector<GameUi::MapScriptText> ReadMapScripts(GameUiReader& reader, std::vector<Diagnostic>& diagnostics) {
    std::vector<GameUi::MapScriptText> scripts;
    bool jass = false;
    for (const auto& candidate : GameUi::MapScriptPaths()) {
        if (candidate.language == GameUi::ScriptLanguage::Jass && jass) continue;
        const std::string path(candidate.path);
        auto file = reader.ReadMapMember(path, kMaxScriptBytes);
        if (!file.Succeeded() || !file.value) {
            const bool missing = std::any_of(file.diagnostics.begin(), file.diagnostics.end(),
                [](const Diagnostic& diagnostic) { return diagnostic.code == "WORLD_MAP_MEMBER_MISSING"; });
            if (!missing) {
                diagnostics.push_back(Diagnostic{DiagnosticSeverity::Warning, "GameUi", "GAMEUI_MAP_SCRIPT_UNREADABLE",
                    path + " 读取失败或超过脚本大小上限，未分析其中的界面调用", std::nullopt});
            }
            continue;
        }
        scripts.push_back({path, candidate.language, GameUi::BytesToText(*file.value)});
        if (candidate.language == GameUi::ScriptLanguage::Jass) jass = true;
    }
    return scripts;
}

// 地图导入的界面描述：只认地图层（含会话导入）的文件；文件名须与描述编号一致，运行库按编号找文件。
std::vector<GameUiDescriptionFile> ReadDescriptions(GameUiReader& reader, std::vector<Diagnostic>& diagnostics) {
    std::vector<GameUiDescriptionFile> descriptions;
    const auto warn = [&](const char* code, std::string message) {
        diagnostics.push_back(Diagnostic{DiagnosticSeverity::Warning, "GameUi", code, std::move(message), std::nullopt});
    };
    for (const auto& path : reader.MapFiles()) {
        if (!GameUi::IsUiDescriptionMember(path)) continue;
        auto file = reader.Read(path);
        if (!file.Succeeded() || !file.value || !file.value->fromMap) {
            warn("AUI_UNREADABLE", path + " 读取失败，未显示这份界面描述");
            continue;
        }
        if (file.value->bytes.size() > kMaxUiFileBytes) {
            warn("AUI_UNREADABLE", path + " 超过界面文件大小上限");
            continue;
        }
        const auto& bytes = file.value->bytes;
        auto parsed = GameUi::ParseUiDescription(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path);
        diagnostics.insert(diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
        if (!parsed.value) continue;
        std::string expected = Text::LowerAscii(GameUi::UiDescriptionMemberPath(parsed.value->id));
        std::string actual = Text::LowerAscii(path);
        std::replace(actual.begin(), actual.end(), '/', '\\');
        if (expected != actual) {
            warn("AUI_ID_PATH", path + " 里的界面编号是 " + parsed.value->id + "，与文件名不一致，未显示");
            continue;
        }
        descriptions.push_back({path, std::move(*parsed.value)});
    }
    std::sort(descriptions.begin(), descriptions.end(),
        [](const auto& a, const auto& b) { return a.description.id < b.description.id; });
    return descriptions;
}

} // namespace

GameUiDefinitions LoadGameUiDefinitions(GameUiReader& reader) {
    GameUiDefinitions result;
    auto library = std::make_shared<GameUi::FrameLibrary>();
    const GameUi::FrameFileReader read = [&reader](const std::string& path) -> Result<std::vector<std::byte>> {
        auto file = reader.Read(path);
        if (!file.Succeeded() || !file.value) return {std::nullopt, std::move(file.diagnostics)};
        if (file.value->bytes.size() > kMaxUiFileBytes)
            return Result<std::vector<std::byte>>::Failure("GameUi", "GAMEUI_FILE_TOO_LARGE", path + " 超过界面文件大小上限");
        return Result<std::vector<std::byte>>::Success(std::move(file.value->bytes));
    };
    library->LoadToc(read, kTocPath, "game");
    std::vector<Diagnostic> mapDiagnostics;
    if (reader.HasMapLayer()) {
        auto scripts = ReadMapScripts(reader, mapDiagnostics);
        auto plan = GameUi::PlanMapTocs(reader.MapFiles(), scripts);
        mapDiagnostics.insert(mapDiagnostics.end(), plan.diagnostics.begin(), plan.diagnostics.end());
        for (const auto& toc : plan.tocs) library->LoadToc(read, toc, "map");
        result.tocCalls = std::move(plan.calls);
        result.scriptFrames = GameUi::FindScriptFrames(scripts);
        // 脚本文字常写成 TRIGSTR_编号，显示时查地图字符串表（同样取最近一次保存的版本）。
        if (auto strings = reader.ReadMapMember("war3map.wts", kMaxScriptBytes); strings.Succeeded() && strings.value) {
            if (auto table = MapData::ParseStringTable(*strings.value); table.value) {
                for (auto& entry : table.value->entries) result.scriptFrames.strings[entry.id] = std::move(entry.value);
            }
        }
        mapDiagnostics.insert(mapDiagnostics.end(), result.scriptFrames.diagnostics.begin(),
            result.scriptFrames.diagnostics.end());
        result.scripts = std::move(scripts);
        result.descriptions = ReadDescriptions(reader, mapDiagnostics);
    }
    result.diagnostics = library->Diagnostics();
    result.diagnostics.insert(result.diagnostics.end(), mapDiagnostics.begin(), mapDiagnostics.end());
    if (auto skins = read(kSkinPath); skins.Succeeded() && skins.value) {
        result.gameSkins = GameUi::BytesToText(*skins.value);
    } else {
        result.diagnostics.insert(result.diagnostics.end(), skins.diagnostics.begin(), skins.diagnostics.end());
    }
    // 地图皮肤表只来自地图；不存在是常态，不记诊断。
    if (reader.HasMapLayer()) {
        if (auto mapSkin = read(kMapSkinPath); mapSkin.Succeeded() && mapSkin.value) {
            result.mapSkin = GameUi::BytesToText(*mapSkin.value);
        }
    }
    result.library = std::move(library);
    return result;
}

std::vector<GameUiAsset> LoadGameUiAssets(GameUiReader& reader, const std::vector<std::string>& textures,
    const std::vector<std::string>& fonts) {
    std::vector<GameUiAsset> assets;
    std::set<std::string> seen;
    const auto load = [&](const std::string& path, bool font) {
        if (!seen.insert(GameUiAssetKey(QString::fromStdString(path)).toStdString()).second) return;
        GameUiAsset asset;
        asset.path = path;
        auto file = reader.Read(path);
        if (!file.Succeeded() || !file.value) {
            asset.error = file.diagnostics.empty() ? std::string("读取失败") : file.diagnostics.front().message;
        } else {
            asset.origin = file.value->origin;
            if (font) {
                asset.fontBytes = std::move(file.value->bytes);
            } else if (file.value->bytes.size() > kMaxUiFileBytes) {
                asset.error = "贴图超过大小上限";
            } else {
                auto decoded = Image::DecodeTexture(path, file.value->bytes);
                if (decoded.Succeeded() && decoded.value) asset.image = std::move(*decoded.value);
                else asset.error = decoded.diagnostics.empty() ? std::string("无法解码") : decoded.diagnostics.front().message;
            }
        }
        assets.push_back(std::move(asset));
    };
    for (const auto& path : textures) load(path, false);
    for (const auto& path : fonts) load(path, true);
    return assets;
}

QString GameUiAssetKey(const QString& path) {
    QString key = path.toLower();
    key.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return key;
}

} // namespace War3Map::Modules
