#pragma once

#include "TerrainHistorySourceShadowCases.h"
#include "War3MapCore/Hash.h"

namespace AudastraTest::TerrainHistoryShadowReadBudgetCases {
using namespace AudastraTest::TerrainResize;
using namespace War3Map::Services;
namespace SourceShadow = AudastraTest::TerrainHistorySourceShadowCases;

inline bool ShadowReadBudgetFailure(const Diagnostic& diagnostic) {
    return diagnostic.code == "FILE_READ_BUDGET_EXCEEDED"
        && (diagnostic.message.find("war3map.shd") != std::string::npos
            || diagnostic.message.find("WAR3MAP.SHD") != std::string::npos);
}

inline void PhysicalShadowIsBoundedBeforeReadingAfterTheTenMembers() {
    Fixture fixture; SourceShadow::Raise(fixture);
    const auto lease = Take(TerrainHistorySave::BindSourceShadow(*fixture.archive,
        Take(fixture.edits->PrepareTerrainHistoryForSave())));
    SourceShadow::Save(fixture, lease, "shadow-read-budget-station.w3x");
    const auto physical = MapDocument::FromArchive(*fixture.archive);
    const auto terrain = Take(fixture.archive->ReadMember("war3map.w3e"));
    const auto pathing = Take(fixture.archive->ReadMember("war3map.wpm"));
    const auto shadow = Take(fixture.archive->ReadMember("war3map.shd"));
    const auto wire = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))));
    const auto journal = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'I'} && journal.history && journal.history->baselineShadow
        && journal.history->baselineShadow->bytes == shadow, "real ordinary save freezes the original SHD in I history");
    MapDataEditSession raw(physical); Check(raw.AttachTerrain(terrain)); Check(raw.AttachPathing(pathing));
    const auto revision = raw.Revision(); const auto sourceSha = fixture.archive->SourceSha256();

    std::size_t manifestBytes = 0;
    for (const auto& member : journal.members) manifestBytes += member.bytes;
    Require(!shadow.empty() && manifestBytes > shadow.size(),
        "the ten-member physical read is larger than SHD, so its old shared cap cannot reject SHD");
    const TerrainReplayLimits hard;
    Require(shadow.size() <= std::min(hard.maximumCandidateBytes, hard.maximumWork) / 64,
        "small original fixture leaves room for the bounded public-budget search");
    for (const bool candidateBytes : {false, true}) {
        bool manifestReadRefused = false, shadowReadRefused = false;
        std::string lastFailure;
        // 用实际成员大小递增公共预算，观察读取层的拒绝位置，不复制内部预收常数。
        // 只需找到十成员已读完、SHD尚未读入的窗口；不重放大图或依赖分配器容量。
        for (std::size_t multiple = 1; multiple <= 64; ++multiple) {
            TerrainReplayLimits limits;
            if (candidateBytes) limits.maximumCandidateBytes = shadow.size() * multiple;
            else limits.maximumWork = shadow.size() * multiple;
            const auto restored = TerrainPathingSave::RestoreJournal(*fixture.archive, physical, terrain,
                pathing, journal, raw, fixture.gameRoot, limits);
            if (restored.Succeeded()) break;
            Require(!restored.value, "budget failure does not publish a partially restored typed session");
            lastFailure.clear();
            for (const auto& diagnostic : restored.diagnostics) {
                lastFailure += diagnostic.code + ": " + diagnostic.message + "\n";
                if (ShadowReadBudgetFailure(diagnostic)) shadowReadRefused = true;
                else if (diagnostic.code == "FILE_READ_BUDGET_EXCEEDED") manifestReadRefused = true;
            }
            if (shadowReadRefused) break;
        }
        if (!manifestReadRefused || !shadowReadRefused)
            throw std::runtime_error(std::string(candidateBytes ? "candidate" : "work")
                + " budget must first bound the ten members, then reject SHD before allocation; last refusal: " + lastFailure);
        Require(raw.Revision() == revision && raw.MatchesTerrainJournalDraft(*fixture.edits),
            "failed budget probes leave the complete raw endpoint and revision unchanged");
    }
    Require(Take(Sha256File(fixture.archive->SourcePath())) == sourceSha,
        "budget probes leave the real original archive unchanged");
}

inline void RunTerrainHistoryShadowReadBudgetCases() {
    PhysicalShadowIsBoundedBeforeReadingAfterTheTenMembers();
}
} // namespace AudastraTest::TerrainHistoryShadowReadBudgetCases
