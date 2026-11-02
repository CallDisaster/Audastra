#pragma once
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerTransactionSnapshot.h"

namespace {
// 只为独立通行验收追加可编辑T；不改源代码槽，也不直接拼接游戏脚本。
class CorpusMovementT {
public:
    War3Map::Trigger::TriggerDocument& document;
    const War3Map::Trigger::UiCatalog& catalog;
    using Id = War3Map::StableId;
    using Edit = War3Map::Trigger::TriggerDocumentEditor;
    using Kind = War3Map::Trigger::UiDefinitionKind;

    Id Root(Id trigger, Kind kind, std::string_view name, std::size_t count) {
        const auto result = Take(Edit::AppendRootEca(document, catalog, trigger, kind, name));
        Require(Edit::FindEca(document, result.rootEcaId)->arguments.size() == count,
            "movement fixture actual public definition signature differs");
        return result.rootEcaId;
    }
    Id Arg(Id eca, std::size_t slot) {
        const auto* node = Edit::FindEca(document, eca);
        Require(node && slot < node->arguments.size(), "movement fixture argument missing");
        return document.argumentNodes.at(node->arguments[slot]).id;
    }
    void Reset(Id argument) {
        const auto* node = Edit::FindArgument(document, argument);
        Require(node && !node->indexArgument, "movement fixture scalar is unexpectedly indexed");
        if (node->callEca) (void)Take(Edit::DetachArgumentCall(document, argument));
    }
    void Value(Id argument, std::int32_t kind, std::string value) {
        Reset(argument); Check(Edit::SetScalarArgument(document, argument, kind, std::move(value)));
    }
    Id Call(Id argument, std::string_view type, std::string_view name, std::size_t count) {
        Reset(argument);
        const auto result = Take(Edit::AppendArgumentCall(document, catalog, argument, type, name));
        Require(Edit::FindEca(document, result.rootEcaId)->arguments.size() == count,
            "movement fixture actual nested definition signature differs");
        return result.rootEcaId;
    }
    static std::string Number(double value) {
        Require(std::isfinite(value), "movement fixture coordinate must be finite");
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        Require(result.ec == std::errc{}, "movement fixture coordinate formatting failed");
        return {buffer.data(), result.ptr};
    }
    void Point(Id argument, double x, double y) {
        const auto point = Call(argument, "location", "Location", 2);
        Value(Arg(point, 0), 3, Number(x)); Value(Arg(point, 1), 3, Number(y));
    }
    Id Timed(std::int32_t category, std::string_view name, std::string seconds) {
        const auto trigger = Take(Edit::AddTrigger(document, category, std::string(name)));
        const auto timer = Root(trigger, Kind::Event, "TriggerRegisterTimerEventSingle", 1);
        Value(Arg(timer, 0), 3, std::move(seconds)); return trigger;
    }
    Id Text(Id trigger) {
        const auto action = Root(trigger, Kind::Action, "DisplayTimedTextToForce", 3);
        // force默认来源由真实Catalog构建；只改本夹具需要的时长与文本。
        Value(Arg(action, 1), 3, "30"); return Arg(action, 2);
    }
    void Add(double x, double y) {
        constexpr auto variable = "AudastraTerrainWalker";
        Check(Edit::AddVariable(document, variable, "unit"));
        const auto category = Take(War3Map::Trigger::TriggerDirectoryEditor::AddCategory(document, "TerrainAcceptance"));
        const auto start = Timed(category, "TerrainAcceptanceStart", "1");
        const auto create = Root(start, Kind::Action, "CreateNUnitsAtLoc", 5);
        Value(Arg(create, 0), 3, "1"); Value(Arg(create, 1), 3, "hfoo");
        Value(Arg(create, 2), 0, "Player00"); Point(Arg(create, 3), x - 384, y);
        Value(Arg(create, 4), 3, "0");
        const auto store = Root(start, Kind::Action, "SetVariable", 2);
        Value(Arg(store, 0), 1, variable); (void)Call(Arg(store, 1), "unit", "GetLastCreatedUnit", 0);
        const auto select = Root(start, Kind::Action, "SelectUnitForPlayerSingle", 2);
        Value(Arg(select, 0), 1, variable); Value(Arg(select, 1), 0, "Player00");
        const auto camera = Root(start, Kind::Action, "PanCameraToTimedLocForPlayer", 3);
        Value(Arg(camera, 0), 0, "Player00"); Point(Arg(camera, 1), x, y); Value(Arg(camera, 2), 3, "0");
        const auto order = Root(start, Kind::Action, "IssuePointOrderLoc", 3);
        Value(Arg(order, 0), 1, variable); Point(Arg(order, 2), x, y);
        Value(Text(start), 3, "TERRAIN_ACCEPTANCE: move to the edited point");
        const auto report = Timed(category, "TerrainAcceptancePosition", "8");
        Value(Text(report), 3, "TERRAIN_ACCEPTANCE: X then Y");
        for (const auto name : {"GetUnitX", "GetUnitY"}) {
            const auto convert = Call(Text(report), "string", "R2S", 1);
            const auto coordinate = Call(Arg(convert, 0), "real", name, 1);
            Value(Arg(coordinate, 0), 1, variable);
        }
    }
};

int PrepareCorpusMovement(const std::filesystem::path& source, const std::string& expected,
    const std::filesystem::path& tool, const std::filesystem::path& game,
    const std::filesystem::path& target, double x, double y) {
    using namespace War3Map;
    Require(expected.size() == 64 && Take(Sha256File(source)) == expected, "movement source identity differs");
    Require(std::filesystem::file_size(source) <= kSaveBytes && !std::filesystem::exists(target),
        "movement fixture requires a bounded source and fresh target");
    const auto metadata = Take(Mpq::MpqArchive::OpenReadOnly(source));
    for (const auto& member : Take(metadata.ListFilesStrict()))
        Require(!Text::LowerAscii(member.name).ends_with(".ai"), "movement fixture excludes AI members");
    const auto archive = Take(MapArchiveSession::Open(source));
    const auto wtg = Take(archive.ReadMember("war3map.wtg")), wct = Take(archive.ReadMember("war3map.wct"));
    const auto terrain = Take(archive.ReadMember("war3map.w3e")), pathing = Take(archive.ReadMember("war3map.wpm"));
    const auto catalog = Take(Trigger::UiCatalog::Load(tool / "ui"));
    auto document = Take(Trigger::WtgWctCodec{}.Parse(wtg, wct, catalog));
    CorpusMovementT{document, catalog}.Add(x, y);
    const auto frozen = Take(Trigger::TriggerTransactionSnapshot::Freeze(document, catalog, wtg, wct,
        {expected, 1, {0, expected, catalog.Sha256()}, 1}));
    MapEditSaveRequest request;
    request.sourceMap = source; request.expectedSourceSha256 = expected; request.targetCandidate = target;
    request.toolRoot = tool; request.gameRoot = game; request.uiRoot = tool / "ui";
    request.triggerTransaction = frozen; request.effectiveLayers = catalog.Layers(); request.inProcessOnly = true;
    const auto saved = Take(SaveMapEditCandidate(request));
    Require(saved.inProcessOnly, "movement fixture must use the in-process backend policy");
    const auto readback = Take(MapArchiveSession::Open(target));
    Require(Take(readback.ReadMember("war3map.w3e")) == terrain && Take(readback.ReadMember("war3map.wpm")) == pathing,
        "movement fixture must not change actual terrain or pathing");
    const auto rebuilt = Take(Trigger::WtgWctCodec{}.Parse(Take(readback.ReadMember("war3map.wtg")),
        Take(readback.ReadMember("war3map.wct")), catalog));
    Require(rebuilt.triggers.size() == document.triggers.size() && rebuilt.variables.size() == document.variables.size(),
        "movement T readback differs");
    Check(archive.VerifySourceUnchanged()); Require(Take(Sha256File(source)) == expected, "movement source changed");
    std::cout << nlohmann::json{{"source_sha256", expected}, {"candidate_sha256", saved.candidateSha256},
        {"script_backend_policy", "in_process_only"}, {"terrain_pathing_unchanged", true},
        {"target_x", x}, {"target_y", y}, {"start_x", x - 384}, {"start_y", y},
        {"report_at_seconds", 8}, {"traditional_t", true}}.dump() << '\n';
    return 0;
}
} // namespace
