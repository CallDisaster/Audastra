#pragma once

#include "WorldTerrainBuild.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "MdxTestModel.h"

#include <algorithm>
#include <array>

// 复用WorldModuleTests的原创地图/FlatGrid夹具，只补悬崖数据和断言；不新增运行器。
namespace War3Map::Tests::WorldCliffSourceFailure {

inline const std::string requested = "Doodads\\Terrain\\Cliffs\\CliffsAAAB1.mdx";
inline const std::string lower = "Doodads\\Terrain\\Cliffs\\CliffsAAAB0.mdx";

// 原创单层三角面；请求变体与低变体有不同真实Z，不以文件名或计数冒充选中几何。
inline std::vector<std::byte> Model(float z, std::uint32_t samplerFlags = 0) {
    MdxTest::Writer w;
    w.Tag("MDLX"); w.Tag("VERS"); w.U32(4); w.U32(800);
    w.Tag("TEXS"); w.U32(268); w.U32(0); w.Fixed("Terrain\\Test\\stone.blp", 260); w.U32(samplerFlags);
    w.Tag("MTLS"); {
        const auto chunk = w.Begin(), material = w.Begin();
        w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1);
        const auto layer = w.Begin();
        w.U32(0); w.U32(0); w.U32(0); w.U32(0xffffffffu); w.U32(0); w.F32(1);
        w.EndInclusive(layer); w.EndInclusive(material); w.EndChunk(chunk);
    }
    w.Tag("GEOS"); {
        const auto chunk = w.Begin(), geoset = w.Begin();
        w.Tag("VRTX"); w.U32(3);
        for (const auto& p : std::array<std::array<float, 3>, 3>{{{-96, 24, z}, {-32, 24, z}, {-96, 88, z}}})
            for (const auto axis : p) w.F32(axis);
        w.Tag("NRMS"); w.U32(3); for (int i = 0; i < 3; ++i) { w.F32(0); w.F32(0); w.F32(1); }
        w.Tag("PTYP"); w.U32(1); w.U32(4); w.Tag("PCNT"); w.U32(1); w.U32(3);
        w.Tag("PVTX"); w.U32(3); w.U16(0); w.U16(1); w.U16(2);
        w.Tag("GNDX"); w.U32(3); w.Raw("\0\0\0", 3);
        w.Tag("MTGC"); w.U32(1); w.U32(1); w.Tag("MATS"); w.U32(1); w.U32(0);
        w.U32(0); w.U32(0); w.U32(0); for (int i = 0; i < 7; ++i) w.F32(0); w.U32(0);
        w.Tag("UVAS"); w.U32(1); w.Tag("UVBS"); w.U32(3);
        for (int i = 0; i < 3; ++i) { w.F32(0); w.F32(0); }
        w.EndInclusive(geoset); w.EndChunk(chunk);
    }
    w.Tag("BONE"); {
        const auto chunk = w.Begin(), node = w.Begin();
        w.Fixed("OriginalCliff", 80); w.U32(0); w.U32(0xffffffffu); w.U32(0);
        w.EndInclusive(node); w.U32(0xffffffffu); w.U32(0xffffffffu); w.EndChunk(chunk);
    }
    w.Tag("PIVT"); w.U32(12); w.F32(0); w.F32(0); w.F32(0);
    return std::move(w.bytes);
}

inline std::vector<MapArchiveMember> Members(std::vector<MapArchiveMember> base, MapData::TerrainGridSnapshot grid) {
    if (base.size() < 2 || (grid.vertices.size() != 4 && grid.vertices.size() != 6)) return {};
    grid.header.mainTileset = 'A'; grid.header.groundTilesets = {"Ldrt"}; grid.header.cliffTilesets = {"CLdi"};
    grid.vertices[1].cliffAndLayerRaw = 3; grid.vertices[1].position[2] = 128; grid.maximum[2] = 128;
    for (auto& vertex : grid.vertices) vertex.variationRaw = 1 << 5;
    auto terrain = MapData::SerializeMapTerrain(grid); if (!terrain.Succeeded()) return {};
    base[1].data = std::move(*terrain.value);
    const std::string slk = "ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X2;Y1;K\"texDir\"\nC;X3;Y1;K\"texFile\"\n"
        "C;X4;Y1;K\"cliffModelDir\"\nC;X5;Y1;K\"rampModelDir\"\nC;X6;Y1;K\"groundTile\"\nC;X7;Y1;K\"cliffClass\"\n"
        "C;X1;Y2;K\"CLdi\"\nC;X2;Y2;K\"Terrain/Test\"\nC;X3;Y2;K\"stone\"\nC;X4;Y2;K\"Cliffs\"\n"
        "C;X5;Y2;K\"CliffTrans\"\nC;X6;Y2;K\"Ldrt\"\nC;X7;Y2;K\"1\"\nE\n";
    const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
    for (auto& member : base) if (member.name == "TerrainArt/CliffTypes.slk") member.data.assign(bytes.begin(), bytes.end());
    base.push_back({lower, Model(0)});
    return base;
}

inline bool SameDiagnostics(std::span<const Diagnostic> actual, std::span<const Diagnostic> expected) {
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i]; const auto& b = expected[i];
        if (a.severity != b.severity || a.subsystem != b.subsystem || a.code != b.code || a.message != b.message
            || a.origin.has_value() != b.origin.has_value()) return false;
        if (a.origin && (a.origin->source != b.origin->source || a.origin->offset != b.origin->offset
            || a.origin->length != b.origin->length || a.origin->fingerprint != b.origin->fingerprint)) return false;
    }
    return true;
}

inline bool RetainsFailure(const Modules::WorldTerrainLoaded& loaded, const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty() || loaded.cliffs || !loaded.ground || loaded.ground->hiddenTileCount != 0
        || loaded.ground->vertices.size() != 4 || loaded.cliffDiagnostics.size() != diagnostics.size() + 1
        || !loaded.cliffFailure || loaded.cliffFailure->requestedPath != requested
        || !loaded.materialNote.contains(QString::fromStdString(requested))) return false;
    if (!SameDiagnostics(std::span<const Diagnostic>(loaded.cliffDiagnostics).first(diagnostics.size()), diagnostics)) return false;
    const auto& context = loaded.cliffDiagnostics.back();
    if (context.code != Render::kTerrainCliffModelFailureContextCode || context.severity != DiagnosticSeverity::Info
        || context.subsystem != "Render.TerrainCliffs" || context.origin || context.message.find(requested) == std::string::npos) return false;
    return std::all_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return loaded.materialNote.contains(QString::fromStdString(diagnostic.code))
            && loaded.materialNote.contains(QString::fromUtf8(diagnostic.message));
    }) && !loaded.materialNote.contains(QStringLiteral("WORLD_CLIFF_MODEL"));
}

inline bool SelectsGeometry(const Modules::WorldTerrainLoaded& loaded, float z) {
    return loaded.cliffs && loaded.ground && loaded.ground->hiddenTileCount == 1 && loaded.cliffDiagnostics.empty() && !loaded.cliffFailure
        && loaded.cliffs->placedTiles == 1 && loaded.cliffs->coveredTiles == std::vector<std::uint8_t>{1}
        && loaded.cliffs->meshes.size() == 1 && loaded.cliffs->meshes.front().vertices.size() == 3
        && loaded.cliffs->meshes.front().vertices.front().z == z;
}

inline bool SameLocation(const Resources::ResourceLocation& actual, const Resources::ResourceLocation& expected) {
    return actual.canonicalPath == expected.canonicalPath && actual.layer == expected.layer && actual.priority == expected.priority
        && actual.sourceId == expected.sourceId && actual.sourcePath == expected.sourcePath;
}

inline bool CachesResource(const Modules::WorldTerrainCache& cache, const std::string& path,
    const std::vector<std::byte>& bytes, const Resources::ResourceLocation& location) {
    const auto found = cache.modelFiles.find(path);
    if (found == cache.modelFiles.end() || !found->second.Succeeded()) return false;
    const auto& resource = *found->second.value;
    return resource.bytes == bytes && SameLocation(resource.location, location);
}

} // namespace War3Map::Tests::WorldCliffSourceFailure
