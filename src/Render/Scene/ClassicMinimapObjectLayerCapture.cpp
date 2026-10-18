#include "War3MapCore/Render/ClassicMinimapObjectLayer.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace War3Map::Render {
namespace {
constexpr char kSubsystem[] = "ClassicMinimapObjectCapture";
struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, kSubsystem, code, std::move(message), {}}}};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
std::string Canonical(std::string_view name) { return Take(Resources::CanonicalImportPath(name)); }

struct Context {
    const MapArchiveSession& source;
    const ClassicMinimapObjectLayerLimits& limits;
    std::filesystem::path gameRoot;
    std::uint8_t tileset;
    AnimationSourceGuard guard;
    Mpq::MpqArchive map;
    std::optional<Game::GameDataSource> game;
    std::map<std::string, const MemberReplacement*, std::less<>> replacements;
    std::map<std::string, bool, std::less<>> removals;
    std::map<std::string, std::optional<Resources::ResolvedResource>, std::less<>> cache;
    std::vector<ClassicMinimapObjectLayer::Dependency> dependencies;
    std::size_t sourceBytes = 0;

    Context(const MapArchiveSession& input, const std::filesystem::path& root, std::uint8_t terrainSet,
        std::span<const MemberReplacement> output, std::span<const std::string> removed,
        const ClassicMinimapObjectLayerLimits& budget)
        : source(input), limits(budget), gameRoot(root), tileset(terrainSet), map(Take(Mpq::MpqArchive::OpenReadOnly(input.SourcePath()))) {
        Check(source.VerifySourceUnchanged()); Check(guard.Watch(source.SourcePath()));
        for (const auto& replacement : output)
            if (!replacements.emplace(Canonical(replacement.name), &replacement).second)
                Fail("CLASSIC_OBJECT_LAYER_DUPLICATE", "对象来源overlay重复替换成员。");
        for (const auto& removal : removed) {
            const auto key = Canonical(removal);
            if (!removals.emplace(key, true).second || replacements.contains(key))
                Fail("CLASSIC_OBJECT_LAYER_CONFLICT", "对象来源overlay重复删除或同时替换和删除成员。");
        }
    }

    void EnsureGame() {
        if (game || gameRoot.empty()) return;
        Check(guard.WatchGameRoot(gameRoot));
        game = Take(Game::GameDataSource::MountStrict(gameRoot, tileset)); Check(guard.Verify());
    }

    const std::optional<Resources::ResolvedResource>& Read(std::string_view input) {
        const auto path = Canonical(input);
        if (const auto found = cache.find(path); found != cache.end()) return found->second;
        if (cache.size() >= 8192) Fail("CLASSIC_OBJECT_LAYER_DEPENDENCY_BUDGET", "对象层依赖数量超过8192。");
        ClassicMinimapObjectLayer::Dependency dependency;
        dependency.winner.canonicalPath = dependency.winner.sourcePath = path;
        std::optional<Resources::ResolvedResource> resource;
        std::string container;
        if (const auto found = replacements.find(path); found != replacements.end()) {
            if (found->second->data.size() > limits.maxResourceBytes - std::min(sourceBytes, limits.maxResourceBytes))
                Fail("CLASSIC_OBJECT_LAYER_RESOURCE_BUDGET", "对象层overlay资源超过累计读取预算。");
            dependency.winner.layer = Resources::ResourceLayer::SessionOverride;
            dependency.winner.priority = Resources::ResourcePriority::SessionOverride;
            dependency.winner.sourceId = "classic-minimap-object-overlay";
            dependency.mapMemberPresent = true; container = source.SourceSha256();
            resource = Resources::ResolvedResource{dependency.winner, found->second->data};
        } else if (!removals.contains(path) && map.HasFile(path)) {
            dependency.winner.layer = Resources::ResourceLayer::MapImport;
            dependency.winner.priority = Resources::ResourcePriority::MapArchive;
            dependency.winner.sourceId = "classic-minimap-object-map";
            dependency.mapMemberPresent = true; container = source.SourceSha256();
            resource = Resources::ResolvedResource{dependency.winner,
                Take(map.ReadFileBounded(path, limits.maxResourceBytes - std::min(sourceBytes, limits.maxResourceBytes)))};
        } else {
            EnsureGame();
            if (game && game->HasFile(path)) {
                auto file = Take(game->ReadFileBoundedWithSource(path,
                    limits.maxResourceBytes - std::min(sourceBytes, limits.maxResourceBytes)));
                dependency.winner.layer = Resources::ResourceLayer::Game;
                dependency.winner.priority = Resources::ResourcePriority::GameBase;
                dependency.winner.sourceId = "classic-minimap-object-game";
                container = std::move(file.archiveName);
                resource = Resources::ResolvedResource{dependency.winner, std::move(file.bytes)};
            }
        }
        dependency.present = resource.has_value();
        if (resource) {
            dependency.encodedBytes = resource->bytes.size(); sourceBytes += resource->bytes.size();
            dependency.encodedSha256 = Take(Sha256(resource->bytes));
            Resources::ResourceReadProvenance provenance;
            provenance.origin = dependency.winner.layer == Resources::ResourceLayer::Game
                ? Resources::ResourceReadOrigin::GameArchive : dependency.winner.layer == Resources::ResourceLayer::SessionOverride
                ? Resources::ResourceReadOrigin::SessionOwned : Resources::ResourceReadOrigin::MapArchive;
            provenance.containerIdentity = container; provenance.memberPath = path;
            provenance.sourceContext = dependency.winner.layer == Resources::ResourceLayer::Game
                ? gameRoot.generic_string() : source.SourceSha256();
            dependency.winner.readProvenance = std::make_shared<const Resources::ResourceReadProvenance>(std::move(provenance));
            resource->location = dependency.winner;
        }
        dependencies.push_back(std::move(dependency));
        return cache.emplace(path, std::move(resource)).first->second;
    }

    Resources::ResourceResolver Resolver() {
        Resources::ResourceResolver resolver;
        Resources::ResourceSource resourceSource;
        resourceSource.id = "classic-minimap-object-capture"; resourceSource.layer = Resources::ResourceLayer::MapImport;
        resourceSource.priority = Resources::ResourcePriority::MapArchive;
        resourceSource.probe = [this](std::string_view name) {
            try { return Result<Resources::ResourceProbe>::Success(Read(name)
                ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(name)} : Resources::ResourceProbe{}); }
            catch (Failure& error) { return Result<Resources::ResourceProbe>{std::nullopt, std::move(error.diagnostics)}; }
        };
        resourceSource.readWithProvenance = [this](std::string_view name) {
            try {
                const auto& file = Read(name);
                if (!file || !file->location.readProvenance) return Result<Resources::ResourceReadResult>::Failure(
                    kSubsystem, "RESOURCE_MISSING", "对象层资源已确认缺失。");
                return Result<Resources::ResourceReadResult>::Success({file->bytes, *file->location.readProvenance});
            } catch (Failure& error) { return Result<Resources::ResourceReadResult>{std::nullopt, std::move(error.diagnostics)}; }
        };
        Check(resolver.AddSource(std::move(resourceSource))); return resolver;
    }

    void Verify() { Check(guard.Verify()); Check(source.VerifySourceUnchanged()); }
};

std::unique_ptr<CodecDocument> Native(Context& context, ObjectData::ObjectType type) {
    const auto& file = context.Read(ObjectData::NativeFileName(type));
    if (!file) return {};
    auto result = ObjectData::NativeObjectCodec(type).Parse(file->bytes);
    if (!result.Succeeded() || !result.document) throw Failure{std::move(result.diagnostics)};
    const auto* document = dynamic_cast<const ObjectData::NativeObjectDocument*>(result.document.get());
    if (!document || !document->IsStructurallySafe()) Fail("CLASSIC_OBJECT_LAYER_NATIVE", "对象原生覆盖表不具备安全读取结构。");
    return std::move(result.document);
}

} // namespace

Result<ClassicMinimapObjectLayer> CaptureClassicMinimapObjectLayer(
    std::span<const MapData::DoodadPlacement> placements, const MapArchiveSession& source,
    const std::filesystem::path& gameRoot, std::uint8_t mainTileset, std::array<float, 2> worldOrigin,
    std::array<std::uint32_t, 4> rectangle, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const ClassicMinimapObjectLayerLimits& limits) {
    try {
        Context context(source, gameRoot, mainTileset, replacements, removals, limits);
        if (placements.empty()) { context.Verify(); return Result<ClassicMinimapObjectLayer>::Success({}); }
        ObjectData::CatalogLoadOptions options;
        options.sources = {{ObjectData::ObjectType::Doodad, {"Doodads/Doodads.slk"}, {}},
            {ObjectData::ObjectType::Destructable, {"Units/DestructableData.slk"}, {}}};
        options.stringFiles.clear(); options.rejectInvalidPresentSources = true;
        auto catalog = Take(ObjectData::StandardObjectCatalog::Load([&](const std::string& name) {
            const auto& file = context.Read(name);
            return file ? Result<std::vector<std::byte>>::Success(file->bytes)
                : Result<std::vector<std::byte>>::Failure(kSubsystem, "OBJECT_CATALOG_SOURCE_MISSING", "对象数据表缺失。");
        }, options));
        auto doodads = Native(context, ObjectData::ObjectType::Doodad);
        auto destructables = Native(context, ObjectData::ObjectType::Destructable);
        ObjectData::ObjectFieldSchemaOptions schemaOptions;
        schemaOptions.sources = {{ObjectData::ObjectType::Doodad, "Doodads/DoodadMetaData.slk"},
            {ObjectData::ObjectType::Destructable, "Units/DestructableMetaData.slk"}};
        schemaOptions.stringFiles.clear();
        auto schema = Take(ObjectData::ObjectFieldSchema::Load([&](const std::string& name) {
            const auto& file = context.Read(name);
            return file ? Result<std::vector<std::byte>>::Success(file->bytes)
                : Result<std::vector<std::byte>>::Failure(kSubsystem, "RESOURCE_MISSING", "对象字段元数据缺失。");
        }, schemaOptions));
        auto resolver = context.Resolver();
        ClassicMinimapObjectLayerInput input;
        input.placements = placements; input.catalog = &catalog; input.schema = &schema;
        input.doodadOverrides = dynamic_cast<const ObjectData::NativeObjectDocument*>(doodads.get());
        input.destructableOverrides = dynamic_cast<const ObjectData::NativeObjectDocument*>(destructables.get());
        input.resources = &resolver; input.worldOrigin = worldOrigin; input.sourceRectangle = rectangle;
        auto output = Take(BuildClassicMinimapObjectLayer(input, limits));
        output.dependencies = std::move(context.dependencies); context.Verify();
        return Result<ClassicMinimapObjectLayer>::Success(std::move(output));
    } catch (Failure& failure) { return {std::nullopt, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<ClassicMinimapObjectLayer>::Failure(kSubsystem,
        "CLASSIC_OBJECT_LAYER_ALLOCATION", "对象层捕获内存不足。"); }
}

Status VerifyClassicMinimapObjectLayerDependencies(const ClassicMinimapObjectLayer& layer,
    const MapArchiveSession& source, const std::filesystem::path& gameRoot, std::uint8_t mainTileset,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    bool readback, const ClassicMinimapObjectLayerLimits& limits) {
    try {
        Context context(source, gameRoot, mainTileset, replacements, removals, limits);
        for (const auto& expected : layer.dependencies) {
            (void)context.Read(expected.winner.canonicalPath);
            const auto& actual = context.dependencies.back();
            if (actual.present != expected.present || actual.encodedBytes != expected.encodedBytes
                || actual.encodedSha256 != expected.encodedSha256 || actual.mapMemberPresent != expected.mapMemberPresent)
                Fail("CLASSIC_OBJECT_LAYER_STALE", "对象层依赖的存在性、正文或来源层已改变：" + expected.winner.canonicalPath);
            if (!expected.present) continue;
            const bool reboundMap = actual.mapMemberPresent && expected.mapMemberPresent;
            const auto& a = actual.winner.readProvenance; const auto& e = expected.winner.readProvenance;
            if (!a || !e || (!reboundMap && a->containerIdentity != e->containerIdentity)
                || (!reboundMap && actual.winner.layer != expected.winner.layer))
                Fail("CLASSIC_OBJECT_LAYER_WINNER", "对象层依赖的实际容器或winner已改变：" + expected.winner.canonicalPath);
        }
        context.Verify(); (void)readback; return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
}

} // namespace War3Map::Render
