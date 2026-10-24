#include "War3MapCore/MapData/TerrainShadowCaster.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Vec3 = std::array<float, 3>;
constexpr std::array<float, 12> Identity{1,0,0,0, 0,1,0,0, 0,0,1,0};

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Refused(const Result<TerrainShadowCasterProjection>& result, const char* code = nullptr) {
    Require(!result.Succeeded() && !result.value, "failure cannot publish a partially traced image");
    if (code) Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.code == code; }), "failure must identify its actual boundary");
}

// 原创最小MDX v800，无外部资源；包含真实BONE/PIVT/MTGC/MATS静态资格。
struct Writer {
    std::vector<std::byte> bytes;
    void Raw(const void* data, std::size_t count) {
        const auto* first = static_cast<const std::byte*>(data); bytes.insert(bytes.end(), first, first + count);
    }
    void Tag(const char* value) { Raw(value, 4); }
    void U32(std::uint32_t value) { Raw(&value, 4); }
    void U16(std::uint16_t value) { Raw(&value, 2); }
    void F32(float value) { Raw(&value, 4); }
    void Fixed(const char* value, std::size_t count) {
        std::vector<char> text(count, 0); std::memcpy(text.data(), value, std::min(count, std::strlen(value))); Raw(text.data(), count);
    }
    std::size_t Begin() { const auto at = bytes.size(); U32(0); return at; }
    void End(std::size_t at, bool inclusive = false) {
        const auto size = static_cast<std::uint32_t>(bytes.size() - at - (inclusive ? 0 : 4));
        std::memcpy(bytes.data() + at, &size, 4);
    }
};
struct ModelOptions {
    std::vector<float> planes{4};
    bool reverse = false, animated = false, unknownChunk = false, nonfinitePivot = false;
    std::uint32_t selectionFlags = 0;
    float alpha = 1;
    float radius = 256, sequenceRadius = 0;
    Vec3 minimum{0,0,-8}, maximum{128,128,8};
    Vec3 sequenceMinimum{}, sequenceMaximum{};
    std::array<float,2> vertexOffset{};
};
std::vector<std::byte> Model(const ModelOptions& options = {}) {
    Writer w;
    w.Tag("MDLX"); w.Tag("VERS"); w.U32(4); w.U32(800);
    w.Tag("MODL"); { const auto chunk = w.Begin(); w.Fixed("OriginalShadowCaster", 80); w.Fixed("", 260);
        w.F32(options.radius); for (const auto v : options.minimum) w.F32(v); for (const auto v : options.maximum) w.F32(v);
        w.U32(0); w.End(chunk); }
    w.Tag("SEQS"); { const auto chunk = w.Begin(); w.Fixed("Stand", 80); w.U32(0); w.U32(1000);
        w.F32(0); w.U32(0); w.F32(0); w.U32(0); w.F32(options.sequenceRadius);
        for (const auto v : options.sequenceMinimum) w.F32(v); for (const auto v : options.sequenceMaximum) w.F32(v); w.End(chunk); }
    w.Tag("TEXS"); w.U32(268); w.U32(0); w.Fixed("Textures\\OriginalShadow.blp", 260); w.U32(0);
    w.Tag("MTLS"); { const auto chunk = w.Begin(), material = w.Begin();
        w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1); const auto layer = w.Begin();
        w.U32(0); w.U32(0); w.U32(0); w.U32(UINT32_MAX); w.U32(0); w.F32(1);
        w.End(layer, true); w.End(material, true); w.End(chunk); }
    w.Tag("GEOS"); { const auto chunk = w.Begin(), geoset = w.Begin();
        const auto vertices = static_cast<std::uint32_t>(options.planes.size() * 3);
        w.Tag("VRTX"); w.U32(vertices);
        for (const auto z : options.planes) for (const auto& p : std::array<Vec3, 3>{{
            {options.vertexOffset[0],options.vertexOffset[1],z},
            {128+options.vertexOffset[0],options.vertexOffset[1],z},
            {options.vertexOffset[0],128+options.vertexOffset[1],z}}})
            for (const auto value : p) w.F32(value);
        w.Tag("NRMS"); w.U32(vertices); for (std::uint32_t i = 0; i < vertices; ++i) { w.F32(0); w.F32(0); w.F32(1); }
        w.Tag("PTYP"); w.U32(1); w.U32(4); w.Tag("PCNT"); w.U32(1); w.U32(vertices);
        w.Tag("PVTX"); w.U32(vertices);
        for (std::uint32_t i = 0; i < vertices; i += 3) {
            w.U16(static_cast<std::uint16_t>(i));
            w.U16(static_cast<std::uint16_t>(i + (options.reverse ? 2 : 1)));
            w.U16(static_cast<std::uint16_t>(i + (options.reverse ? 1 : 2)));
        }
        w.Tag("GNDX"); w.U32(vertices); for (std::uint32_t i = 0; i < vertices; ++i) { const char zero = 0; w.Raw(&zero, 1); }
        w.Tag("MTGC"); w.U32(1); w.U32(1); w.Tag("MATS"); w.U32(1); w.U32(0);
        w.U32(0); w.U32(0); w.U32(options.selectionFlags); for (int i = 0; i < 7; ++i) w.F32(0); w.U32(0);
        w.Tag("UVAS"); w.U32(1); w.Tag("UVBS"); w.U32(vertices);
        for (std::uint32_t i = 0; i < vertices; ++i) { w.F32(0); w.F32(0); }
        w.End(geoset, true); w.End(chunk); }
    w.Tag("GEOA"); { const auto chunk = w.Begin(), item = w.Begin();
        w.F32(options.alpha); w.U32(0); w.F32(1); w.F32(1); w.F32(1); w.U32(0); w.End(item, true); w.End(chunk); }
    w.Tag("BONE"); { const auto chunk = w.Begin(), node = w.Begin();
        w.Fixed("Caster", 80); w.U32(0); w.U32(UINT32_MAX); w.U32(0);
        if (options.animated) {
            w.Tag("KGTR"); w.U32(1); w.U32(0); w.U32(UINT32_MAX); w.U32(0); w.F32(0); w.F32(0); w.F32(0);
        }
        w.End(node, true); w.U32(UINT32_MAX); w.U32(UINT32_MAX); w.End(chunk); }
    w.Tag("PIVT"); w.U32(12); w.U32(options.nonfinitePivot ? 0x7FC00031u : 0u); w.U32(0); w.U32(0x80000000u);
    if (options.unknownChunk) { w.Tag("UNKN"); w.U32(0); }
    return std::move(w.bytes);
}
struct Fixture {
    std::array<float, 16> heights{};
    std::array<float, 3> light{0,0,1};
    std::vector<std::byte> bytes = Model();
    TerrainShadowCasterInstanceInput instance{0, Identity};
    TerrainShadowHeightSamples Samples() const { return {4,4,heights,light}; }
    Result<TerrainShadowCasterProjection> Run(const TerrainShadowCasterLimits& limits = {}, std::stop_token cancellation = {}) const {
        const TerrainShadowCasterModelInput model{"OriginalCaster.mdx", bytes};
        return ProjectStaticTerrainShadowCasters(Samples(), {0,0}, std::span(&model, 1), std::span(&instance, 1), limits, cancellation);
    }
};
const std::vector<std::uint8_t> TriangleMask{1,1,1,1, 1,1,1,0, 1,1,0,0, 1,0,0,0};

void SignedMinimumAndDoubleSidedEdges() {
    Fixture fixture;
    const auto before = fixture.bytes;
    const auto positive = Take(fixture.Run());
    Require(positive.casterShadowBits == TriangleMask && positive.triangles == 1,
        "literal mask includes triangle boundary and positive intersections");
    Require(fixture.bytes == before && fixture.heights == std::array<float,16>{}, "CPU kernel cannot mutate borrowed source bytes or receiver grid");
    fixture.bytes = Model({.reverse = true});
    Require(Take(fixture.Run()).casterShadowBits == TriangleMask, "triangle winding cannot change double-sided native ray result");
    fixture.bytes = Model({.planes = {-4}});
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16, 0), "negative-only signed distance does not shadow");
    fixture.bytes = Model({.planes = {4,-4}});
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16, 0),
        "a rear triangle of the same model suppresses an earlier positive hit through signed minimum");
    fixture.bytes = Model({.planes = {-4,4}});
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16, 0), "signed minimum must be independent of triangle order");
    fixture.bytes = Model({.planes = {0,4}});
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16, 0), "zero intersection suppresses positive intersection of same model");
    fixture.bytes = Model({.nonfinitePivot = true});
    Require(Take(fixture.Run()).casterShadowBits == TriangleMask, "unused static pivot NaN bits cannot poison or invalidate qualified identity skinning");
}
void MatrixLayoutAndIndependentModels() {
    Fixture fixture;
    fixture.instance.nativeMatrix[3] = 32; // 行主序第一行末为X平移。
    const std::vector<std::uint8_t> shifted{0,1,1,1, 0,1,1,1, 0,1,1,0, 0,1,0,0};
    Require(Take(fixture.Run()).casterShadowBits == shifted, "row-major translation must use index3 rather than native column-major byte offset");
    fixture.instance.nativeMatrix = Identity; fixture.instance.nativeMatrix[0] = fixture.instance.nativeMatrix[5] = 0.5f;
    const std::vector<std::uint8_t> scaled{1,1,0,0, 1,0,0,0, 0,0,0,0, 0,0,0,0};
    Require(Take(fixture.Run()).casterShadowBits == scaled, "actual instance scale applies to geometry once without an invented 1.5 multiplier");
    const auto positive = Model(), negative = Model({.planes = {-4}});
    const std::array<TerrainShadowCasterModelInput,2> models{{{"front.mdx", positive}, {"rear.mdx", negative}}};
    const std::array<TerrainShadowCasterInstanceInput,2> instances{{{1,Identity}, {0,Identity}}};
    Require(Take(ProjectStaticTerrainShadowCasters(fixture.Samples(), {0,0}, models, instances)).casterShadowBits == TriangleMask,
        "negative distance suppresses only its own model, not a different caster instance");
}
void QualificationAndVisibilityAreFailClosed() {
    Fixture fixture;
    fixture.bytes = Model({.animated = true}); Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_DYNAMIC");
    fixture.bytes = Model({.unknownChunk = true}); Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_DYNAMIC");
    fixture.bytes = Model({.selectionFlags = 4}); Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_VISIBILITY");
    fixture.bytes = Model({.alpha = 0.5f}); Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_VISIBILITY");
    fixture.bytes = Model({.alpha = 0});
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "static zero-alpha GEOA is hidden without relying on UI Stand visibility");
    fixture.bytes = Model(); fixture.bytes.pop_back(); Refused(fixture.Run());
    fixture.bytes = Model(); fixture.instance.modelIndex = 1; Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_INSTANCE");
    fixture.instance.modelIndex = 0; fixture.instance.nativeMatrix[2] = std::numeric_limits<float>::infinity();
    Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_MATRIX");
}
void BudgetsAndCancellationDiscardEntireCandidate() {
    Fixture fixture;
    const auto complete = Take(fixture.Run());
    auto limits = TerrainShadowCasterLimits{};
    limits.maximumWork = complete.work;
    Require(Take(fixture.Run(limits)).casterShadowBits == TriangleMask, "exact total work budget succeeds");
    --limits.maximumWork; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; ++limits.maximumWork; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_LIMITS");
    limits = {}; limits.maximumTriangles = 0; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumModels = 0; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumInstances = 0; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumModelBytes = fixture.bytes.size() - 1; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumGeometryBytes = 1; Refused(fixture.Run(limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumCompiledBytes = 1; Refused(fixture.Run(limits));
    std::stop_source stop; stop.request_stop(); Refused(fixture.Run({}, stop.get_token()), "TERRAIN_SHADOW_CASTER_CANCELLED");
    fixture.heights[7] = std::numeric_limits<float>::quiet_NaN(); Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_HEIGHT");
    fixture.heights[7] = 0; fixture.light = {0,0,0}; Refused(fixture.Run(), "TERRAIN_SHADOW_CASTER_LIGHT");
    fixture.light = {1,0,0};
    Require(Take(fixture.Run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "parallel triangle is rejected by native determinant threshold");
    const auto empty = Take(ProjectStaticTerrainShadowCasters(fixture.Samples(), {0,0}, {}, {}));
    Require(empty.triangles == 0 && empty.casterShadowBits == std::vector<std::uint8_t>(16,0), "complete empty caster set produces explicit zero mask");
}

void NativeSphereAndBlockFiltersAreRequired() {
    Fixture f;
    const TerrainShadowCasterFilterInputs profile{2048,-512};
    const auto run = [&](const TerrainShadowCasterLimits& limits = {}) {
        const TerrainShadowCasterModelInput model{"actual.mdx",f.bytes};
        return ProjectFilteredStaticTerrainShadowCasters(f.Samples(),{0,0},std::span(&model,1),std::span(&f.instance,1),profile,limits);
    };
    Require(Take(run()).casterShadowBits == TriangleMask, "actual MODL center/radius retains the ordinary static caster");
    // 几何仍覆盖receiver，但实际sphere远离有限segment，原版不会进三角形。
    f.bytes = Model({.radius=1,.minimum={1000,1000,0},.maximum={1000,1000,0}});
    Require(Take(f.Run()).casterShadowBits == TriangleMask, "unfiltered reference still intersects displaced extent geometry");
    Require(Take(run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "MODL sphere miss must suppress infinite-line triangle hit");
    f.bytes = Model({.radius=0,.sequenceRadius=256,.sequenceMinimum={0,0,-8},.sequenceMaximum={128,128,8}});
    Require(Take(run()).casterShadowBits == TriangleMask, "near-zero MODL radius uses first original SEQS extent");
    f.bytes = Model({.radius=0,.sequenceRadius=1,.sequenceMinimum={1000,1000,0},.sequenceMaximum={1000,1000,0}});
    Require(Take(run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "first SEQS sphere also performs finite-segment rejection");
    f.bytes = Model({.radius=1,.minimum={16,16,0},.maximum={16,16,0}});
    f.instance.nativeMatrix[10]=100; f.instance.actualScale={1,1,100};
    Require(Take(run()).casterShadowBits == TriangleMask, "native sphere radius uses maximum actual axis scale including Z");
    f.instance.nativeMatrix=Identity; f.instance.actualScale={1,1,1};
    f.bytes = Model({.planes={1300000},.radius=1,.minimum={16,16,1300000},.maximum={16,16,1300000}});
    Require(Take(f.Run()).casterShadowBits[0] == 1, "unfiltered line reaches geometry beyond native 1280000 endpoint");
    Require(Take(run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "finite segment sphere gate rejects beyond-end caster");
    f.bytes = Model({.minimum={-640,0,-8},.maximum={-512,128,8},.vertexOffset={-640,0}});
    f.instance.nativeMatrix[3] = 640;
    Require(Take(run()).casterShadowBits == TriangleMask, "block0 expanded anchor maximum 640 is inclusive");
    f.bytes = Model({.minimum={-641,0,-8},.maximum={-513,128,8},.vertexOffset={-641,0}});
    f.instance.nativeMatrix[3] = 641;
    Require(Take(f.Run()).casterShadowBits == TriangleMask, "geometry remains fixed while actual anchor crosses halo");
    Require(Take(run()).casterShadowBits == std::vector<std::uint8_t>(16,0), "anchor beyond 128 halo is not a native block candidate");
    f = Fixture{};
    const auto complete = Take(run());
    TerrainShadowCasterLimits exact; exact.maximumWork=complete.work;
    Require(Take(run(exact)).casterShadowBits == TriangleMask, "filtered actual byte reread and all filters have exact aggregate work");
    --exact.maximumWork; Refused(run(exact),"TERRAIN_SHADOW_CASTER_BUDGET");
    f.instance.actualScale[1]=0; Refused(run(),"TERRAIN_SHADOW_CASTER_SCALE");
    f.instance.actualScale={1,1,1}; f.light={1,0,0}; Refused(run(),"TERRAIN_SHADOW_CASTER_FILTER");
}

void NativeBlockYEndpointUsesReceiverBlockX() {
    const auto bytes=Model({.minimum={0,-1000,-8},.maximum={128,-872,8},.vertexOffset={0,-1000}});
    const TerrainShadowCasterModelInput model{"block-y.mdx",bytes};
    TerrainShadowCasterInstanceInput instance{0,Identity};
    instance.nativeMatrix[3]=512; instance.nativeMatrix[7]=1000;
    std::vector<float> heights(32*48,0);
    const TerrainShadowHeightSamples samples{32,48,heights,{0,0,1}};
    const auto result=Take(ProjectFilteredStaticTerrainShadowCasters(samples,{0,0},std::span(&model,1),
        std::span(&instance,1),{0,0}));
    Require(result.casterShadowBits[16] == 1,
        "receiver block(1,0) includes anchor in blockY1 because native endY is blockX+directionY");
    Require(result.casterShadowBits[0] == 0, "receiver blockX0 retains its own X candidate restriction");
}
} // namespace

int main() {
    try {
        SignedMinimumAndDoubleSidedEdges(); MatrixLayoutAndIndependentModels();
        QualificationAndVisibilityAreFailClosed(); BudgetsAndCancellationDiscardEntireCandidate();
        NativeSphereAndBlockFiltersAreRequired(); NativeBlockYEndpointUsesReceiverBlockX();
        std::cout << "Static terrain shadow caster CPU tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
