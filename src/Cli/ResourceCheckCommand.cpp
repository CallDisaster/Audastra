#include "ResourceCheckCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;

std::string Reason(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Info) continue;
        if (!text.empty()) text += " | ";
        text += diagnostic.code + ": " + diagnostic.message;
    }
    return text.empty() ? std::string("未知原因") : text;
}
std::string FirstCode(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity != DiagnosticSeverity::Info) return diagnostic.code;
    return "UNKNOWN";
}

struct Checker {
    Mpq::MpqArchive& archive;
    std::set<std::string, std::less<>> listed;
    Resources::ResourceResolver resolver;
    std::map<std::string, int, std::less<>> tally;
    std::set<std::string, std::less<>> checkedTextures;

    // 命中层：地图清单内 / 地图清单外（只能按名字哈希找到）/ 游戏。
    std::string Where(std::string_view path) const {
        const auto location = resolver.Locate(path);
        if (!location.Succeeded()) return "缺失";
        if (location.value->layer != Resources::ResourceLayer::MapImport) return "游戏";
        return listed.contains(location.value->canonicalPath) ? "地图" : "地图(清单外)";
    }
    void Fail(const std::string& code, const std::string& line) {
        ++tally[code];
        std::cout << "  失败 " << line << '\n';
    }
    void Texture(const std::string& path, const std::string& owner) {
        if (!checkedTextures.insert(Text::LowerAscii(path)).second) return;
        const auto where = Where(path);
        const auto read = resolver.Read(path);
        if (!read.Succeeded()) { Fail("TEXTURE_" + FirstCode(read.diagnostics), owner + " 贴图 " + path + " [" + where + "] " + Reason(read.diagnostics)); return; }
        const auto decoded = Image::DecodeTexture(path, read.value->bytes);
        if (!decoded.Succeeded()) { Fail("TEXTURE_DECODE_" + FirstCode(decoded.diagnostics), owner + " 贴图 " + path + " [" + where + "] " + Reason(decoded.diagnostics)); return; }
        ++tally["texture-ok/" + where];
    }
    void Model(const std::string& field, const std::string& owner, std::uint32_t variation = 0, std::uint32_t count = 1) {
        const auto path = Render::MdxModelFile(field, variation, count);
        if (path.empty()) return;
        const auto where = Where(path);
        const auto read = resolver.Read(path);
        if (!read.Succeeded()) { Fail("MODEL_" + FirstCode(read.diagnostics), owner + " 模型 " + path + " [" + where + "] " + Reason(read.diagnostics)); return; }
        const auto model = Render::ParseMdx(read.value->bytes);
        if (!model.Succeeded()) { Fail("MODEL_PARSE_" + FirstCode(model.diagnostics), owner + " 模型 " + path + " [" + where + "] " + Reason(model.diagnostics)); return; }
        ++tally["model-ok/" + where];
        for (const auto& texture : model.value->textures)
            if (texture.replaceableId == 0 && !texture.path.empty()) Texture(texture.path, owner + " " + path);
    }
};
} // namespace

int RunResourceCheck(const std::filesystem::path& mapPath, const std::filesystem::path& gameRoot) {
    auto opened = Mpq::MpqArchive::OpenReadOnly(mapPath);
    if (!opened.Succeeded()) { std::cerr << Reason(opened.diagnostics) << '\n'; return 2; }
    auto& archive = *opened.value;
    Checker checker{archive, {}, {}, {}, {}};
    for (const auto& name : archive.RawListfileNames())
        if (const auto key = Resources::CanonicalImportPath(name); key.Succeeded()) checker.listed.insert(*key.value);
    std::cout << "清单列出 " << checker.listed.size() << " 个 / 归档共 " << archive.FileCount() << " 个成员\n";

    const auto w3e = archive.ReadFileBounded("war3map.w3e", 64 * 1024 * 1024);
    if (!w3e.Succeeded()) { std::cerr << Reason(w3e.diagnostics) << '\n'; return 2; }
    const auto terrain = MapData::ParseMapTerrain(*w3e.value);
    if (!terrain.Succeeded()) { std::cerr << Reason(terrain.diagnostics) << '\n'; return 2; }
    const auto grid = terrain.value->Snapshot();
    if (!grid) { std::cerr << "war3map.w3e 没有地形网格\n"; return 2; }
    const auto& header = grid->header;

    Resources::ResourceSource mapSource;
    mapSource.id = "check-map"; mapSource.layer = Resources::ResourceLayer::MapImport;
    mapSource.priority = Resources::ResourcePriority::MapArchive;
    mapSource.probe = [&archive](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success(archive.HasFile(path)
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
    };
    mapSource.read = [&archive](std::string_view path) { return archive.ReadFileBounded(path, kMaxBytes); };
    if (const auto added = checker.resolver.AddSource(std::move(mapSource)); !added.Succeeded()) { std::cerr << Reason(added.diagnostics) << '\n'; return 2; }
    auto game = Game::GameDataSource::MountStrict(gameRoot, header.mainTileset);
    if (!game.Succeeded()) { std::cerr << Reason(game.diagnostics) << '\n'; return 2; }
    Resources::ResourceSource gameSource;
    gameSource.id = "check-game"; gameSource.layer = Resources::ResourceLayer::Game;
    gameSource.priority = Resources::ResourcePriority::GameBase;
    gameSource.probe = [&game](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success(game.value->HasFile(std::string(path))
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
    };
    gameSource.read = [&game](std::string_view path) { return game.value->ReadFileBounded(std::string(path), kMaxBytes); };
    if (const auto added = checker.resolver.AddSource(std::move(gameSource)); !added.Succeeded()) { std::cerr << Reason(added.diagnostics) << '\n'; return 2; }

    std::cout << "[地表] 主地形 " << static_cast<char>(header.mainTileset) << '\n';
    const auto catalog = Render::LoadTerrainMaterialCatalog(checker.resolver);
    if (!catalog.Succeeded()) checker.Fail("TERRAIN_CATALOG_" + FirstCode(catalog.diagnostics), "地形数据表 " + Reason(catalog.diagnostics));
    else {
        for (const auto& id : header.groundTilesets) {
            const auto row = catalog.value->ground.find(id);
            if (row == catalog.value->ground.end()) { checker.Fail("GROUND_UNDEFINED", id + " 在 Terrain.slk 中没有定义"); continue; }
            const auto& path = row->second.texturePath;
            const auto where = checker.Where(path);
            const auto atlas = Render::LoadTerrainTextureAtlas(checker.resolver, path);
            if (!atlas.Succeeded()) checker.Fail("GROUND_" + FirstCode(atlas.diagnostics), id + " " + path + " [" + where + "] " + Reason(atlas.diagnostics));
            else {
                ++checker.tally["ground-ok/" + where];
                std::cout << "  正常 " << id << ' ' << path << " [" << where << "] " << (*atlas.value)->sourceWidth << 'x' << (*atlas.value)->sourceHeight << '\n';
            }
        }
        const auto water = catalog.value->water.find(std::string(1, static_cast<char>(header.mainTileset)) + "Sha");
        if (water != catalog.value->water.end()) {
            const auto frame = Render::LoadTerrainWaterFrame(water->second, 0, checker.resolver);
            if (!frame.Succeeded()) checker.Fail("WATER_" + FirstCode(frame.diagnostics), "水面 " + water->second.texturePrefix + "00 " + Reason(frame.diagnostics));
            else std::cout << "  正常 水面 " << water->second.texturePrefix << "00\n";
        }
    }

    // 物编里显式改过模型的对象：模型文件字段 → 模型 → 模型贴图。
    const auto standardCatalog = ObjectData::StandardObjectCatalog::Load(*game.value);
    const auto* standard = standardCatalog.Succeeded() ? &*standardCatalog.value : nullptr;
    if (!standard) std::cout << "  注意 原版物编目录未能读取，变体数只按地图修改计算\n";
    const std::pair<ObjectData::ObjectType, const char*> fields[]{
        {ObjectData::ObjectType::Doodad, "dfil"}, {ObjectData::ObjectType::Destructable, "bfil"},
        {ObjectData::ObjectType::Unit, "umdl"}, {ObjectData::ObjectType::Item, "ifil"}};
    for (const auto& [type, fieldId] : fields) {
        const auto name = std::string(ObjectData::NativeFileName(type));
        if (!archive.HasFile(name)) continue;
        const auto bytes = archive.ReadFileBounded(name, 64 * 1024 * 1024);
        if (!bytes.Succeeded()) { checker.Fail("OBJECT_READ", name + ' ' + Reason(bytes.diagnostics)); continue; }
        auto parsed = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
        const auto* document = parsed.Succeeded() ? dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
        if (!document) { checker.Fail("OBJECT_PARSE", name + ' ' + Reason(parsed.diagnostics)); continue; }
        std::cout << "[" << name << "] " << document->Objects().size() << " 个对象\n";
        const bool variants = type == ObjectData::ObjectType::Doodad || type == ObjectData::ObjectType::Destructable;
        const auto* countField = type == ObjectData::ObjectType::Doodad ? "dvar" : "bvar";
        for (const auto& object : document->Objects()) {
            const std::string* model = nullptr;
            std::optional<std::int32_t> count;
            for (const auto& modification : object.modifications) {
                if (modification.key.fieldId == fieldId) model = std::get_if<std::string>(&modification.value);
                if (variants && modification.key.fieldId == countField)
                    if (const auto* value = std::get_if<std::int32_t>(&modification.value)) count = *value;
            }
            if (!model) continue;
            // 与世界页相同：空、"_"、"-" 或只有扩展名（".mdl"）表示不显示模型。
            const auto stem = Text::LowerAscii(Text::TrimCWhitespace(*model));
            if (stem.empty() || stem == "_" || stem == "-" || stem == ".mdl" || stem == ".mdx") { ++checker.tally["model-empty"]; continue; }
            // 装饰物/可破坏物按变体数加载 名字0.mdx…；未改变体数时取父对象的 numVar。
            if (variants && !count && standard) {
                const auto* parent = standard->Find(type, object.parentRawcode.value_or(object.key.rawcode));
                const auto raw = parent ? Text::TrimCWhitespace(parent->Field("numvar")) : std::string_view{};
                std::int32_t parsed = 1;
                if (!raw.empty() && std::from_chars(raw.data(), raw.data() + raw.size(), parsed).ec == std::errc{}) count = parsed;
            }
            const auto total = static_cast<std::uint32_t>(std::clamp(count.value_or(1), 1, 64));
            for (std::uint32_t variation = 0; variation < total; ++variation) {
                // 与世界页相同：变体文件确实不存在、无编号文件存在时改用无编号模型。
                const auto variant = Render::MdxModelFile(*model, variation, total);
                const auto located = checker.resolver.Locate(variant);
                const bool missing = !located.Succeeded() && !located.diagnostics.empty()
                    && located.diagnostics.front().code == "RESOURCE_NOT_FOUND";
                if (total > 1 && missing && checker.resolver.Locate(Render::MdxModelFile(*model, 0, 1)).Succeeded()) {
                    ++checker.tally["model-variant-fallback"];
                    checker.Model(*model, name + ' ' + object.key.rawcode);
                } else checker.Model(*model, name + ' ' + object.key.rawcode, variation, total);
            }
        }
    }

    std::cout << "[汇总]\n";
    for (const auto& [code, count] : checker.tally) std::cout << "  " << code << '\t' << count << '\n';
    return 0;
}
