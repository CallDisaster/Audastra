#pragma once
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

// 只读来源诊断；运行真实进程内流水线并输出七类物编原字/可读投影，不发布地图。
int AuditTerrainTemplateObjects(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& tool, const std::filesystem::path& gameRoot, const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "fresh template audit output required");
    const auto source = OpenOracleSource(path, expected);
    const auto layers = Take(Trigger::UiCatalog::ReadConfigLayers(tool));
    const auto catalog = Take(Trigger::UiCatalog::LoadLayers(tool, layers));
    const auto triggers = Take(Trigger::WtgWctCodec{}.Parse(Take(source.ReadMember("war3map.wtg")),
        Take(source.ReadMember("war3map.wct")), catalog));
    const auto game = Take(Game::GameDataSource::Mount(gameRoot));
    const auto standard = Take(ObjectData::StandardObjectCatalog::Load(game));
    const auto records = [&](ObjectType type) {
        const auto exact = Take(source.CaptureExactMembers(std::array<std::string, 1>{std::string(NativeFileName(type))}));
        if (!exact.front().present) return std::vector<ObjectRecord>{};
        const auto parsed = NativeObjectCodec(type).Parse(exact.front().bytes);
        const auto* native = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        Require(parsed.Succeeded() && native && native->IsStructurallySafe(), "template audit needs complete native objects");
        return std::vector<ObjectRecord>{native->Objects().begin(), native->Objects().end()};
    };
    std::filesystem::create_directories(output);
    Script::MapScriptBuildRequest request;
    request.archive = &source; request.catalog = &catalog; request.triggers = &triggers;
    request.game = &game; request.standard = &standard; request.layers = layers;
    request.units = records(ObjectType::Unit); request.items = records(ObjectType::Item); request.abilities = records(ObjectType::Ability);
    request.toolchain.toolRoot = tool; request.toolchain.workDir = output / "pipeline"; request.toolchain.inProcessOnly = true;
    const auto built = Script::BuildMapScript(request);
    Check({built.diagnostics}); Check({built.generated.diagnostics});
    if (built.pipeline) Check({built.pipeline->diagnostics});
    Require(built.Succeeded() && built.pipeline && built.pipeline->syntaxChecked, "template audit requires a complete checked pipeline");
    nlohmann::json members = nlohmann::json::array();
    for (const auto& member : built.pipeline->objectMembers) {
        std::optional<ObjectType> type;
        for (const auto candidate : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
            ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable})
            if (member.name == NativeFileName(candidate)) type = candidate;
        Require(type.has_value(), "template output must be one of the seven native members");
        const auto exact = Take(source.CaptureExactMembers(std::array<std::string, 1>{member.name}));
        const auto& before = exact.front();
        if (before.present) {
            Check(IO::WriteFileBytes(output / ("original-" + member.name), before.bytes));
            Check(IO::WriteFileBytes(output / ("original-" + member.name + ".ini"), Take(ConvertNativeObjectDataToLni(*type, before.bytes))));
        }
        Check(IO::WriteFileBytes(output / ("generated-" + member.name), member.data));
        Check(IO::WriteFileBytes(output / ("generated-" + member.name + ".ini"), Take(ConvertNativeObjectDataToLni(*type, member.data))));
        members.push_back({{"name", member.name}, {"source_present", before.present},
            {"original_sha256", before.present ? Take(Sha256(before.bytes)) : std::string{}},
            {"generated_sha256", Take(Sha256(member.data))}, {"changed", !before.present || before.bytes != member.data}});
    }
    Check(source.VerifySourceUnchanged());
    const auto report = nlohmann::json{{"source_sha256", expected}, {"source_unchanged", true},
        {"backend", "in_process_only"}, {"syntax_checked", true}, {"members", members}, {"map_written", false}}.dump(2);
    Check(IO::WriteFileBytes(output / "report.json", std::as_bytes(std::span(report))));
    std::cout << report << '\n'; return 0;
}
