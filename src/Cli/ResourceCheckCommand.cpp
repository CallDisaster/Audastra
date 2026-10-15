#include "ResourceCheckCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/PlacementInitializationRules.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Workspace.h"

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
    const Mpq::MpqArchive& archive;
    std::set<std::string, std::less<>> listed;
    Resources::ResourceResolver resolver;
    std::map<std::string, int, std::less<>> tally;
    std::set<std::string, std::less<>> checkedTextures;
    std::set<std::string, std::less<>> checkedModels;

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
        if (!checkedModels.insert(Text::LowerAscii(path)).second) return;
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
    auto opened = MpqWorkspace::OpenReadOnly(mapPath);
    if (!opened.Succeeded()) { std::cerr << Reason(opened.diagnostics) << '\n'; return 2; }
    const auto& archive = opened.value->Archive();
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

    // 同一有效目录覆盖地图SLK/profile、原生覆盖和实际摆放；不能仅扫描显式修改的模型字段。
    Services::ObjectCatalogSources catalogSources;
    catalogSources.readOnlyArchive = &*opened.value; catalogSources.gameRoot = gameRoot;
    const auto effective = Services::LoadEffectiveObjectCatalog(catalogSources);
    const auto schema = ObjectData::ObjectFieldSchema::Load(*game.value);
    if (!effective.Succeeded() || !schema.Succeeded()) {
        checker.Fail("OBJECT_CATALOG_UNAVAILABLE", "物编资源覆盖不完整：" + Reason(
            effective.Succeeded() ? schema.diagnostics : effective.diagnostics));
        return 2;
    }
    const auto* standard = &effective.value->Catalog();
    const std::pair<ObjectData::ObjectType, const char*> fields[]{
        {ObjectData::ObjectType::Doodad, "dfil"}, {ObjectData::ObjectType::Destructable, "bfil"},
        {ObjectData::ObjectType::Unit, "umdl"}, {ObjectData::ObjectType::Item, "ifil"}};
    using ObjectId = std::pair<ObjectData::ObjectType, std::string>;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, 7> documents;
    std::array<bool, 7> unavailable{};
    for (const auto& spec : ObjectData::StandardObjectCatalog::DefaultSources()) {
        if (spec.slkFiles.empty()) continue;
        const auto* input = effective.value->Input(spec.slkFiles.front());
        if (!input || !input->present) {
            unavailable[static_cast<std::size_t>(spec.type)] = true;
            checker.Fail("OBJECT_CLASSIFICATION_SOURCE_MISSING", std::string(ObjectData::ToString(spec.type)) + " 主表缺失，分类覆盖不完整");
        }
    }
    std::set<ObjectId> candidates;
    std::size_t mapDefinitions = 0, placed = 0, classified = 0;
    for (const auto& entry : standard->Entries()) {
        if (entry.nativeOverridesSupported || std::none_of(std::begin(fields), std::end(fields),
            [&](const auto& field) { return field.first == entry.key.type; })) continue;
        candidates.emplace(entry.key.type, entry.key.rawcode); ++mapDefinitions;
    }
    for (const auto& [type, fieldId] : fields) {
        (void)fieldId;
        const auto name = std::string(ObjectData::NativeFileName(type));
        if (!archive.HasFile(name)) continue;
        const auto bytes = archive.ReadFileBounded(name, 64 * 1024 * 1024);
        const auto index = static_cast<std::size_t>(type);
        if (!bytes.Succeeded()) { unavailable[index] = true; checker.Fail("OBJECT_READ", name + ' ' + Reason(bytes.diagnostics)); continue; }
        const auto converted = ObjectData::ConvertNativeObjectDataToLni(type, *bytes.value);
        if (!converted.Succeeded()) { unavailable[index] = true; checker.Fail("OBJECT_PARSE", name + ' ' + Reason(converted.diagnostics)); continue; }
        auto parsed = ObjectData::LniObjectCodec(type).Parse(*converted.value);
        const auto* document = parsed.Succeeded() ? dynamic_cast<const ObjectData::LniObjectDocument*>(parsed.document.get()) : nullptr;
        if (!document || !document->IsStructurallySafe()) { unavailable[index] = true; checker.Fail("OBJECT_PARSE", name + ' ' + Reason(parsed.diagnostics)); continue; }
        documents[index] = std::make_unique<ObjectData::LniObjectDocument>(*document);
        for (const auto& object : document->Objects()) candidates.emplace(type, object.key.rawcode);
    }
    const auto map = MapDocument::FromReadOnlyArchive(*opened.value);
    const auto classify = [&](const std::string& rawcode, ObjectData::ObjectType first, ObjectData::ObjectType second) {
        if (first == ObjectData::ObjectType::Unit && rawcode == "sloc") { ++checker.tally["placement-start-location"]; return; }
        ++placed;
        if (first == ObjectData::ObjectType::Unit && MapData::IsRandomPlacementMarker(rawcode)) {
            checker.Fail("PLACEMENT_RANDOM_UNSUPPORTED",
                rawcode + " 随机摆放占位；当前资源检查未展开候选，资源支持仍未验证");
            return;
        }
        const auto exists = [&](ObjectData::ObjectType type) {
            const auto* document = documents[static_cast<std::size_t>(type)].get();
            return (document && document->FindObject({type, rawcode})) || standard->Find(type, rawcode);
        };
        if (unavailable[static_cast<std::size_t>(first)] || unavailable[static_cast<std::size_t>(second)]) {
            checker.Fail("PLACEMENT_CLASSIFICATION_UNAVAILABLE", rawcode + " 对象类别表未完整读取"); return;
        }
        const bool a = exists(first), b = exists(second);
        if (a == b) { checker.Fail(a ? "PLACEMENT_CLASSIFICATION_AMBIGUOUS" : "PLACEMENT_OBJECT_UNDEFINED", rawcode); return; }
        ++classified; candidates.emplace(a ? first : second, rawcode);
    };
    if (!map.Succeeded()) checker.Fail("PLACEMENT_READ", Reason(map.diagnostics));
    else {
        for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads}) {
            const auto& member = map.value->Member(kind);
            if (member.state != MapDataState::Ready && member.state != MapDataState::Missing)
                checker.Fail("PLACEMENT_PARSE", member.memberName + ' ' + Reason(member.diagnostics));
        }
        if (map.value->Units()) for (const auto& unit : map.value->Units()->units)
            classify(unit.rawcode, ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item);
        if (map.value->Doodads()) {
            for (const auto& doodad : map.value->Doodads()->doodads)
                classify(doodad.rawcode, ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable);
            for (const auto& doodad : map.value->Doodads()->specialDoodads)
                classify(doodad.rawcode, ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable);
        }
    }
    std::cout << "[物编覆盖] 地图基础表对象 " << mapDefinitions << "；摆放 " << placed << "，已分类 " << classified
        << "；待查对象 " << candidates.size() << "（含原生继承、地图SLK和实际摆放；不含脚本动态字符串与技能特效）\n";
    for (const auto& [type, rawcode] : candidates) {
        const auto field = std::find_if(std::begin(fields), std::end(fields), [&](const auto& spec) { return spec.first == type; });
        if (field == std::end(fields) || unavailable[static_cast<std::size_t>(type)]) continue;
        Services::ObjectCommandContext context;
        context.type = type; context.document = documents[static_cast<std::size_t>(type)].get();
        context.standard = standard; context.schema = &*schema.value;
        for (std::size_t i = 0; i < documents.size(); ++i) context.documents[i] = documents[i].get();
        const auto values = Services::ReadObjectFields(context, rawcode);
        const auto owner = std::string(ObjectData::ToString(type)) + ' ' + rawcode;
        if (!values.Succeeded()) { checker.Fail("OBJECT_INHERITANCE", owner + ' ' + Reason(values.diagnostics)); continue; }
        const bool variants = type == ObjectData::ObjectType::Doodad || type == ObjectData::ObjectType::Destructable;
        const auto* countField = type == ObjectData::ObjectType::Doodad ? "dvar" : "bvar";
        const std::string* model = nullptr;
        std::optional<std::int32_t> count;
        bool badField = false;
        for (const auto& row : *values.value) {
            if (row.key.fieldId == field->second) {
                const auto* value = row.value ? std::get_if<std::string>(&*row.value) : nullptr;
                if (!value || model) badField = true;
                else model = value;
            }
            if (variants && row.key.fieldId == countField) {
                const auto* value = row.value ? std::get_if<std::int32_t>(&*row.value) : nullptr;
                if (!value || count) badField = true;
                else count = *value;
            }
        }
        if (!model || badField) { checker.Fail("OBJECT_MODEL_FIELD_UNAVAILABLE", owner + " 模型或变体字段缺失、歧义或不透明"); continue; }
            // 与世界页相同：空、"_"、"-" 或只有扩展名（".mdl"）表示不显示模型。
            const auto stem = Text::LowerAscii(Text::TrimCWhitespace(*model));
            if (stem.empty() || stem == "_" || stem == "-" || stem == ".mdl" || stem == ".mdx") { ++checker.tally["model-empty"]; continue; }
            if (count && (*count < 0 || *count > 64)) { checker.Fail("OBJECT_VARIATION_BUDGET", owner + " 变体数不在当前检查预算0..64内，未截断冒充检查完整"); continue; }
            const auto total = static_cast<std::uint32_t>(std::max(count.value_or(1), 1));
            for (std::uint32_t variation = 0; variation < total; ++variation) {
                // 与世界页相同：变体文件确实不存在、无编号文件存在时改用无编号模型。
                const auto variant = Render::MdxModelFile(*model, variation, total);
                const auto located = checker.resolver.Locate(variant);
                const bool missing = !located.Succeeded() && !located.diagnostics.empty()
                    && located.diagnostics.front().code == "RESOURCE_NOT_FOUND";
                if (total > 1 && missing && checker.resolver.Locate(Render::MdxModelFile(*model, 0, 1)).Succeeded()) {
                    ++checker.tally["model-variant-fallback"];
                    checker.Model(*model, owner);
                } else checker.Model(*model, owner, variation, total);
            }
    }
    const auto verified = Services::VerifyEffectiveObjectCatalog(*effective.value, catalogSources);
    if (!verified.Succeeded()) checker.Fail("OBJECT_SOURCE_CHANGED", Reason(verified.diagnostics));
    if (!opened.value->VerifyUnchanged().Succeeded()) checker.Fail("MAP_SOURCE_CHANGED", "地图来源在检查期间发生变化");

    std::cout << "[汇总]\n";
    for (const auto& [code, count] : checker.tally) std::cout << "  " << code << '\t' << count << '\n';
    return 0;
}
