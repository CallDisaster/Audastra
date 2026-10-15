#include "War3MapCore/Resources/ClassicObjectTexture.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "AnimationTestModel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Resources;
using namespace War3Map::Render;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T> T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "empty failure" :
        result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}

std::string Key(std::string_view path) {
    std::string key(path);
    for (auto& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

std::vector<std::byte> Blp(std::uint8_t red) {
    Image::DecodedImage image{2, 2, std::vector<std::byte>(16)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = static_cast<std::byte>(red); image.rgba[i + 3] = std::byte{255};
    }
    return Good(Image::EncodeBlpPaletted(image));
}

std::vector<std::byte> Tga(std::uint8_t red) {
    std::vector<std::byte> bytes(18 + 16);
    bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{2};
    bytes[16] = std::byte{32}; bytes[17] = std::byte{0x28};
    for (std::size_t i = 18; i < bytes.size(); i += 4) {
        bytes[i + 2] = static_cast<std::byte>(red); bytes[i + 3] = std::byte{255};
    }
    return bytes;
}

struct Layer {
    std::map<std::string, std::vector<std::byte>> files;
    std::string failure;
    std::vector<std::string> reads;
};

ResourceSource Source(const std::shared_ptr<Layer>& layer, std::string id, int priority) {
    return {std::move(id), ResourceLayer::Game, priority,
        [layer](std::string_view path) {
            const auto found = layer->files.find(std::string(path));
            return Result<ResourceProbe>::Success({found == layer->files.end() ? ResourceProbeState::Missing :
                ResourceProbeState::Found, std::string(path)});
        },
        [layer](std::string_view path) {
            layer->reads.emplace_back(path);
            if (!layer->failure.empty()) return Result<std::vector<std::byte>>::Failure("Test", layer->failure, "原创来源拒绝读取");
            return Result<std::vector<std::byte>>::Success(layer->files.at(std::string(path)));
        }};
}

void TestCandidateSourceOrder() {
    const auto paths = Good(ClassicObjectTextureCandidates("Textures.v1/Surface.original.BLP"));
    Require(paths == std::array<std::string, 2>{"Textures.v1\\Surface.original.tga", "Textures.v1\\Surface.original.blp"},
        "only the filename extension must be removed");
    Require(Good(ClassicObjectTextureCandidates("Textures.v1/Surface"))[0] == "Textures.v1\\Surface.tga",
        "directory dots must not remove the filename");
    for (const auto path : {"../Surface.tga", "Textures/../Surface.blp", "C:/Surface.blp", "Textures/.tga", ".blp"}) {
        int reads = 0;
        const auto rejected = ReadClassicObjectTexture<int>(path, [&](auto) { ++reads; return Result<int>::Success(1); });
        Require(!rejected.Succeeded() && reads == 0, "unsafe names must be rejected before any source access");
    }
    auto high = std::make_shared<Layer>(), low = std::make_shared<Layer>();
    high->files["TEXTURES\\SURFACE.BLP"] = {std::byte{17}};
    low->files["TEXTURES\\SURFACE.BLP"] = {std::byte{23}};
    ResourceResolver resolver;
    Require(resolver.AddSource(Source(high, "original-patch", 20)).Succeeded(), "patch source registration");
    Require(resolver.AddSource(Source(low, "original-base", 10)).Succeeded(), "base source registration");
    const auto read = [&](std::string_view path) { return resolver.Read(path); };
    auto selected = Good(ReadClassicObjectTexture<ResolvedResource>("Textures\\Surface.tga", read));
    Require(selected.path == "Textures\\Surface.blp" && selected.resource.location.sourceId == "original-patch"
        && selected.resource.bytes == std::vector{std::byte{17}} && low->reads.empty(),
        "missing TGA must use the actual BLP and its highest source identity");
    low->files["TEXTURES\\SURFACE.TGA"] = {std::byte{31}};
    selected = Good(ReadClassicObjectTexture<ResolvedResource>("Textures\\Surface.blp", read));
    Require(selected.path == "Textures\\Surface.tga" && selected.resource.location.sourceId == "original-base"
        && selected.resource.bytes == std::vector{std::byte{31}},
        "each candidate must finish its full source search before trying the next extension");
    high->files["TEXTURES\\SURFACE.TGA"] = {std::byte{47}};
    selected = Good(ReadClassicObjectTexture<ResolvedResource>("Textures\\Surface.blp", read));
    Require(selected.path.ends_with(".tga") && selected.resource.location.sourceId == "original-patch"
        && selected.resource.bytes == std::vector{std::byte{47}}, "existing TGA must win even for a declared BLP name");
    high->failure = "RESOURCE_ACCESS_DENIED"; high->reads.clear(); low->reads.clear();
    const auto failed = ReadClassicObjectTexture<ResolvedResource>("Textures\\Surface.blp", read);
    Require(!failed.Succeeded() && high->reads == std::vector<std::string>{"TEXTURES\\SURFACE.TGA"}
        && low->reads.empty(), "a broken higher TGA must not fall through to lower sources or BLP");
}

void TestFailureBoundaries() {
    for (const auto code : {"RESOURCE_READ_FAILED", "RESOURCE_CONFLICT", "RESOURCE_PROBE_FAILED",
             "RESOURCE_SOURCE_CHANGED", "ANIMATION_RESOURCE_BUDGET", "file-not-found", "WORLD_ASSET_MISSING", ""}) {
        std::vector<std::string> reads;
        const auto result = ReadClassicObjectTexture<int>("Textures\\Surface.blp", [&](std::string_view path) {
            reads.emplace_back(path);
            if (reads.size() > 1) return Result<int>::Success(17);
            if (!*code) return Result<int>{};
            return Result<int>::Failure("Test", code, "原创拒绝原因");
        });
        Require(!result.Succeeded() && reads == std::vector<std::string>{"Textures\\Surface.tga"},
            "only exact RESOURCE_NOT_FOUND may select a second extension");
        if (*code) Require(result.diagnostics.front().code == code, "failure reason must remain intact");
    }
    int reads = 0;
    const auto missing = ReadClassicObjectTexture<int>("Surface.bmp", [&](std::string_view path) {
        ++reads; return Result<int>::Failure("Test", "RESOURCE_NOT_FOUND", std::string(path));
    });
    Require(!missing.Succeeded() && reads == 2 && missing.diagnostics.size() == 2
        && missing.diagnostics[0].message == "Surface.tga" && missing.diagnostics[1].message == "Surface.blp",
        "both missing candidates must retain their own reason and path");
}

// 两个材质层分别使用物编替换入口和普通 TEXS；所有字节由原创夹具生成。
std::vector<std::byte> Model(std::string_view directPath, bool particles = false, bool replaceGeometry = true) {
    auto bytes = AnimationTest::Bytes();
    const auto word = [&](std::size_t at) { std::uint32_t value; std::memcpy(&value, bytes.data() + at, 4); return value; };
    const auto write = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    bool changed = false;
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        auto size = word(at + 4);
        Require(size <= bytes.size() - at - 8, "original model chunk bound");
        if (!std::memcmp(bytes.data() + at, "TEXS", 4)) {
            Require(size == 536 && directPath.size() < 260, "original texture layout");
            std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(at + 12), 260, std::byte{});
            std::memcpy(bytes.data() + at + 12, directPath.data(), directPath.size());
            write(at + 8 + 268, replaceGeometry ? 11 : 1); changed = true;
        }
        if (particles && !std::memcmp(bytes.data() + at, "PIVT", 4)) {
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + size), 12, std::byte{});
            size += 12; write(at + 4, size);
        }
        at += 8 + size;
    }
    Require(changed, "original texture chunk missing");
    if (!particles) return bytes;
    MdxTest::Writer w; w.bytes = std::move(bytes);
    w.Tag("PRE2"); const auto chunk = w.Begin(), emitter = w.Begin(), node = w.Begin();
    w.Fixed("Original replacement emitter", 80); w.U32(2); w.U32(0); w.U32(0x1000); w.EndInclusive(node);
    for (const float value : {0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 100.0f, 0.0f, 0.0f}) w.F32(value);
    w.U32(1); w.U32(1); w.U32(1); w.U32(0); w.F32(0); w.F32(0.5f);
    for (int i = 0; i < 9; ++i) w.F32(1);
    const std::array<unsigned char, 3> alpha{255, 255, 255}; w.Raw(alpha.data(), alpha.size());
    for (int i = 0; i < 3; ++i) w.F32(1);
    for (int i = 0; i < 4; ++i) { w.U32(0); w.U32(0); w.U32(1); }
    w.U32(0); w.U32(0); w.U32(0); w.U32(11);
    w.EndInclusive(emitter); w.EndChunk(chunk);
    return std::move(w.bytes);
}

struct Files {
    std::map<std::string, AnimationResource> values;
    std::map<std::string, unsigned> reads;
    void Add(std::string_view path, std::vector<std::byte> bytes, std::string source = "original-object-textures") {
        const auto digest = Good(Sha256(bytes));
        values[Key(path)] = {std::move(bytes), std::move(source), digest};
    }
    AnimationResourceReader Reader() {
        return [this](std::string_view path, std::uint64_t maximum) {
            ++reads[Key(path)];
            const auto found = values.find(Key(path));
            if (found == values.end()) return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", std::string(path));
            if (found->second.bytes.size() > maximum)
                return Result<AnimationResource>::Failure("Test", "ANIMATION_RESOURCE_BUDGET", "原创资源超过预算");
            return Result<AnimationResource>::Success(found->second);
        };
    }
    ModelFileReader ByteReader() {
        return [read = Reader()](std::string_view path) {
            auto resource = read(path, UINT64_MAX);
            if (!resource.Succeeded()) return Result<std::vector<std::byte>>{std::nullopt, std::move(resource.diagnostics)};
            return Result<std::vector<std::byte>>::Success(std::move(resource.value->bytes));
        };
    }
};

ModelInstanceRequest Request(std::string path = "original.mdx") {
    ModelInstanceRequest request; request.modelPath = std::move(path); request.replaceableTexture = "Textures\\Surface.blp";
    return request;
}

void TestSceneEntryAndCache() {
    Files files; files.Add("original.mdx", Model("Textures\\Surface.blp"));
    files.Add("Textures\\Surface.tga", Tga(200)); files.Add("Textures\\Surface.blp", Blp(40));
    const auto request = Request();
    auto scene = Good(BuildWorldModelScene(std::span(&request, 1), files.ByteReader()));
    Require(scene.textures.size() == 2 && scene.meshes.size() > 0, "two distinct successful source textures expected");
    auto layer = scene.meshes.front().firstLayer;
    Require(scene.textures[scene.layers[layer].textureIndex].rgba[0] == 200
        && scene.textures[scene.layers[layer + 1].textureIndex].rgba[0] == 40,
        "object BLP request and exact BLP request must not share the wrong texture cache entry");

    files.Add("original.mdx", Model("Textures\\Surface.tga")); files.values.erase(Key("Textures\\Surface.tga"));
    scene = Good(BuildWorldModelScene(std::span(&request, 1), files.ByteReader()));
    layer = scene.meshes.front().firstLayer;
    Require(scene.textures.size() == 1 && Key(scene.textures.front().path) == "textures\\surface.blp"
        && scene.layers[layer].textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE
        && scene.layers[layer + 1].textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE,
        "only object replacement may use BLP when ordinary TEXS names a missing TGA");

    files.Add("Textures\\Surface.tga", Tga(200));
    WorldModelLimits limits; limits.maxTextures = 1;
    scene = Good(BuildWorldModelScene(std::span(&request, 1), files.ByteReader(), limits));
    layer = scene.meshes.front().firstLayer;
    Require(scene.textures.size() == 1 && scene.layers[layer].textureIndex == scene.layers[layer + 1].textureIndex
        && scene.layers[layer].textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE,
        "different request rules resolving the same TGA must share the actual texture budget");

    files.Add("original.mdx", Model("Textures\\Other.blp")); files.Add("Textures\\Other.blp", Blp(80));
    files.Add("Textures\\Surface.tga", {std::byte{0}}); files.reads.clear();
    scene = Good(BuildWorldModelScene(std::span(&request, 1), files.ByteReader()));
    layer = scene.meshes.front().firstLayer;
    Require(scene.layers[layer].textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE && files.reads["textures\\surface.blp"] == 0,
        "a found but corrupt TGA must not be replaced with a BLP after decoding fails");
}

AnimationInstanceSpec Spec(std::string path, std::uint64_t id) {
    AnimationInstanceSpec spec; spec.identity = {id, 7}; spec.hostKey = "original:" + std::to_string(id);
    spec.model = Request(std::move(path)); spec.sequenceIndex = 0; spec.elapsedMs = 300; spec.globalMs = 300;
    return spec;
}

void CheckBlpSource(const AnimatedModelScene& scene, const AnimationResource& expected) {
    Require(std::any_of(scene.sources.begin(), scene.sources.end(), [&](const auto& source) {
        return Key(source.virtualPath) == "textures\\surface.blp" && source.sourceLayer == expected.sourceLayer
            && source.contentDigest == expected.digest;
    }), "source identity must name actual selected BLP and preserve its layer and digest");
    Require(std::none_of(scene.sources.begin(), scene.sources.end(), [](const auto& source) {
        return Key(source.virtualPath) == "textures\\surface.tga";
    }), "missing TGA must not be reported as the successful BLP source");
}

void TestAnimationPreparedSources() {
    Files files; files.Add("seed.mdx", Model("Textures\\Other.blp"));
    files.Add("new.mdx", Model("Textures\\Other.blp"));
    files.Add("Textures\\Other.blp", Blp(80)); files.Add("Textures\\Seed.blp", Blp(100));
    files.Add("Textures\\Surface.blp", Blp(40), "original-selected-layer");
    auto seed = Spec("seed.mdx", 1); seed.model.replaceableTexture = "Textures\\Seed.blp";
    const auto added = Spec("new.mdx", 2); auto limits = InteractiveAnimationPreviewLimits(); limits.retainUnsupportedStatic = true;
    auto resident = Good(ModelAnimationService::Create({7, 1, "original-replacement-session"}, std::span(&seed, 1), files.Reader(), limits));
    auto preview = Good(ModelAnimationService::Create({7, 1, "original-replacement-preview"}, std::span(&added, 1), files.Reader(), limits));
    CheckBlpSource(*preview->Scene(), files.values.at("textures\\surface.blp"));
    const auto snapshot = Good(preview->CaptureAssetSnapshot());
    const auto catalog = Good(resident->CaptureInstanceCatalog());
    const auto prepared = Good(catalog->PrepareStaticAssets(added.model, files.Reader(), snapshot));
    const auto before = files.reads;
    AnimationInstanceDelta delta; delta.session = {7, 2, "original-replacement-session"}; delta.additions = {added};
    AnimationInstanceDeltaTimings timings;
    const auto plan = Good(catalog->PrepareInstanceCatalog(delta, files.Reader(), {}, &timings, prepared));
    Require(timings.preparedAssetHit && files.reads["textures\\surface.tga"] == before.at("textures\\surface.tga") + 1
        && files.reads["textures\\surface.blp"] == before.at("textures\\surface.blp") + 1,
        "prepared asset must verify both missing TGA and actual BLP through the frozen reader");
    const auto frame = Good(resident->Prepare(nullptr));
    const auto applied = Good(resident->PrepareInstanceDelta(*frame, *plan));
    CheckBlpSource(*applied.service->Scene(), files.values.at("textures\\surface.blp"));
    const auto original = files.values.at("textures\\surface.blp");
    for (const bool bytesChanged : {false, true}) {
        if (bytesChanged) files.Add("Textures\\Surface.blp", Blp(60), original.sourceLayer);
        else files.values.at("textures\\surface.blp").sourceLayer = "original-new-source-layer";
        const auto stale = catalog->PrepareInstanceCatalog(delta, files.Reader(), {}, &timings, prepared);
        Require(!stale.Succeeded() && stale.diagnostics.front().code == "ANIMATION_PREPARED_SOURCE_CHANGED"
            && !timings.preparedAssetHit, "changed selected bytes or source must reject the prepared packet");
        files.values.at("textures\\surface.blp") = original;
    }
    files.Add("Textures\\Surface.tga", Tga(210));
    const auto appeared = catalog->PrepareInstanceCatalog(delta, files.Reader(), {}, &timings, prepared);
    Require(!appeared.Succeeded() && appeared.diagnostics.front().code == "ANIMATION_PREPARED_SOURCE_CHANGED",
        "new preferred TGA must invalidate the recorded missing candidate");
    const auto refreshed = Good(catalog->PrepareStaticAssets(added.model, files.Reader(), snapshot));
    const auto refreshedPlan = Good(catalog->PrepareInstanceCatalog(delta, files.Reader(), {}, &timings, refreshed));
    const auto refreshedScene = Good(resident->PrepareInstanceDelta(*frame, *refreshedPlan)).service->Scene();
    const auto texture = std::find_if(refreshedScene->geometry.textures.begin(), refreshedScene->geometry.textures.end(), [](const auto& t) {
        return Key(t.path) == "textures\\surface.tga";
    });
    Require(texture != refreshedScene->geometry.textures.end() && texture->rgba[0] == 210,
        "old BLP decode snapshot must not replace a newly selected TGA");
}

void TestParticleReplacementSource() {
    for (const bool geometryReplacement : {false, true}) {
        Files files; files.Add("particle.mdx", Model("Textures\\Other.blp", true, geometryReplacement));
        files.Add("Textures\\Other.blp", Blp(80)); files.Add(TeamColorTexture(0), Blp(120));
        files.Add("Textures\\Surface.blp", Blp(40), "original-particle-layer");
        const auto spec = Spec("particle.mdx", 1);
        auto limits = InteractiveAnimationPreviewLimits(); limits.enableParticles = true; limits.retainUnsupportedStatic = true;
        const auto service = Good(ModelAnimationService::Create({7, 1, "original-particle-session"},
            std::span(&spec, 1), files.Reader(), limits));
        const auto scene = service->Scene();
        Require(scene->particleInstanceCount == 1 && scene->particleTextures.size() == 1
            && Key(scene->particleTextures.front().path) == "textures\\surface.blp"
            && scene->particleTextures.front().rgba[0] == 40,
            "particle replacement must resolve actual BLP both with and without a geometry texture cache");
        CheckBlpSource(*scene, files.values.at("textures\\surface.blp"));
    }
}
} // namespace

int main() {
    try {
        TestCandidateSourceOrder(); TestFailureBoundaries(); TestSceneEntryAndCache();
        TestAnimationPreparedSources(); TestParticleReplacementSource();
        std::cout << "classic_object_texture_sources=1 exact_texs_unchanged=1 prepared_source_identity=1 particles=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
