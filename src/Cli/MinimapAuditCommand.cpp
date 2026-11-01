#include "MinimapAuditCommand.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"

#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <iostream>

int RunMinimapAudit(int argc, wchar_t** argv) {
    using namespace War3Map;
    using namespace War3Map::Render;
    if (argc != 4) { std::cerr << "usage: minimap-audit <map> <game-root>\n"; return 2; }
    const auto start = std::chrono::steady_clock::now();
    nlohmann::json report{{"schema", 1}, {"read_only", true}, {"saved", false},
        {"generated", false}, {"source_sha256", ""}, {"phase", "open"}};
    auto finish = [&](const std::vector<Diagnostic>& diagnostics) {
        report["diagnostics"] = nlohmann::json::array();
        for (const auto& diagnostic : diagnostics) {
            // 不输出来源路径或可能带私有对象名称的正文。
            report["diagnostics"].push_back({{"subsystem", diagnostic.subsystem}, {"code", diagnostic.code}});
        }
        report["elapsed_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << report.dump() << '\n';
        return report["generated"].get<bool>() ? 0 : 1;
    };
    auto source = MapArchiveSession::OpenMinimapAudit(argv[2]);
    if (!source.Succeeded()) return finish(source.diagnostics);
    report["source_sha256"] = source.value->SourceSha256();
    report["phase"] = "document";
    const auto document = MapDocument::FromMembers(source.value->Members(), source.value->SourcePath());
    MapData::MapDataEditSession edits(document);
    const std::array<std::string, 2> names{"war3map.w3e", "war3map.wpm"};
    auto terrainBytes = source.value->CaptureExactMembers(names);
    if (!terrainBytes.Succeeded()) return finish(terrainBytes.diagnostics);
    if (!terrainBytes.value->front().present) return finish(Status::Failure(
        "MinimapAudit", "TERRAIN_MEMBER_MISSING", "缺少地形成员。").diagnostics);
    auto attached = edits.AttachTerrain(terrainBytes.value->front().bytes);
    if (!attached.Succeeded()) return finish(attached.diagnostics);
    if (terrainBytes.value->at(1).present) {
        auto pathing = edits.AttachPathing(terrainBytes.value->at(1).bytes);
        if (!pathing.Succeeded()) return finish(pathing.diagnostics);
    }
    report["phase"] = "terrain_capture";
    auto terrain = CaptureTerrainMinimapAssets(*source.value, edits, argv[3], {}, TerrainMinimapRangePolicy::ClassicPlayable);
    if (!terrain.Succeeded()) return finish(terrain.diagnostics);
    report["phase"] = "objects_capture";
    auto objects = CaptureClassicMinimapObjectAssets(std::move(*terrain.value), *source.value, edits);
    if (!objects.Succeeded()) return finish(objects.diagnostics);
    report["phase"] = "generate";
    TerrainMinimapPostprocessSettings settings;
    settings.noiseSeed = 0; // 仅固定诊断重跑；不声称复现原版进程的随机流。
    auto candidate = BuildClassicMinimapCandidate(*objects.value, settings);
    if (!candidate.Succeeded()) return finish(candidate.diagnostics);
    const auto unchanged = source.value->VerifySourceUnchanged();
    if (!unchanged.Succeeded()) return finish(unchanged.diagnostics);
    report["generated"] = true;
    report["phase"] = "complete";
    report["raw_rgba_sha256"] = candidate.value->manifest.rawRgbaSha256;
    report["blp_sha256"] = candidate.value->manifest.blpSha256;
    report["source_rectangle"] = candidate.value->manifest.sourceRectangle;
    report["native_verified"] = false;
    return finish(candidate.diagnostics);
}
