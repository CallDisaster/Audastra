#include "ScriptBuildCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Script/ScriptQueries.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

// script-build：从地图（只读）生成整图脚本（WE + YDTrigger 等价），可选继续走外部工具链做语法检查。
namespace {

using namespace War3Map;

void Print(const std::vector<Diagnostic>& diagnostics, std::size_t limit = 40) {
    std::size_t shown = 0;
    for (const auto& diagnostic : diagnostics) {
        if (shown++ >= limit) {
            std::cerr << "... " << diagnostics.size() - limit << " more\n";
            break;
        }
        std::cerr << (diagnostic.severity == DiagnosticSeverity::Error ? "error " : "warning ")
                  << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}

std::vector<ObjectData::ObjectRecord> CustomObjects(const MapArchiveSession& archive, ObjectData::ObjectType type) {
    const auto bytes = archive.ReadMember(std::string(ObjectData::NativeFileName(type)));
    if (!bytes.Succeeded()) return {};
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
    const auto* document = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
    if (document == nullptr) return {};
    return {document->Objects().begin(), document->Objects().end()};
}

std::optional<ObjectData::StandardObjectCatalog> LoadStandard(const std::optional<Game::GameDataSource>& game) {
    if (!game) return std::nullopt;
    if (auto loaded = ObjectData::StandardObjectCatalog::Load(*game); loaded.Succeeded()) return std::move(*loaded.value);
    return std::nullopt;
}

std::optional<Game::GameDataSource> MountGame(const std::filesystem::path& gameRoot) {
    if (auto game = Game::GameDataSource::Mount(gameRoot); game.Succeeded()) return std::move(*game.value);
    return std::nullopt;
}

} // namespace

int RunScriptBuild(const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    const std::filesystem::path& mapPath, const std::filesystem::path& outputPath, const std::optional<std::filesystem::path>& compileDir,
    std::wstring_view engines) {
    const auto started = std::chrono::steady_clock::now();
    auto archive = MapArchiveSession::Open(mapPath);
    if (!archive.Succeeded()) {
        Print(archive.diagnostics);
        return 1;
    }
    auto packages = UiLibrary::DiscoverPackages(toolRoot);
    if (!packages.value) {
        Print(packages.diagnostics);
        return 1;
    }
    const auto layers = UiLibrary::EffectiveLayers(UiLibrary::ReconcileGlobal({}, *packages.value), {}, *packages.value);
    auto catalog = Trigger::UiCatalog::LoadLayers(toolRoot, layers);
    if (!catalog.Succeeded()) {
        Print(catalog.diagnostics);
        return 1;
    }
    const auto& session = *archive.value;
    const auto wtg = session.ReadMember("war3map.wtg");
    const auto wct = session.ReadMember("war3map.wct");
    if (!wtg.Succeeded() || !wct.Succeeded()) {
        std::cerr << "error: 地图缺少 war3map.wtg / war3map.wct\n";
        return 1;
    }
    auto triggers = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *catalog.value);
    if (!triggers.Succeeded()) {
        Print(triggers.diagnostics);
        return 1;
    }
    const auto game = MountGame(gameRoot);
    const auto standard = LoadStandard(game);

    Script::MapScriptBuildRequest request;
    request.game = game ? &*game : nullptr;
    request.archive = &session;
    request.triggers = &*triggers.value;
    request.catalog = &*catalog.value;
    request.standard = standard ? &*standard : nullptr;
    request.units = CustomObjects(session, ObjectData::ObjectType::Unit);
    request.items = CustomObjects(session, ObjectData::ObjectType::Item);
    request.abilities = CustomObjects(session, ObjectData::ObjectType::Ability);
    request.layers = layers;
    request.toolchain.toolRoot = toolRoot;
    request.toolchain.workDir = compileDir.value_or(std::filesystem::path{});
    request.compile = compileDir.has_value();
    if (engines == L"--external") {
        request.toolchain.preprocessor = Script::ScriptEngine::External;
        request.toolchain.vjass = Script::ScriptEngine::External;
        request.toolchain.checker = Script::ScriptEngine::External;
    }
    request.toolchain.crossCheck = engines == L"--cross-check";
    const auto result = Script::BuildMapScript(request);
    Print(result.diagnostics);
    Print(result.generated.diagnostics);
    std::ofstream output(outputPath, std::ios::binary);
    output.write(result.generated.text.data(), static_cast<std::streamsize>(result.generated.text.size()));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "triggers=" << result.generated.triggerCount << " functions=" << result.generated.functionCount
              << " bytes=" << result.generated.text.size() << " diagnostics=" << result.generated.diagnostics.size() << '\n';
    if (const auto& pipeline = result.pipeline) {
        for (const auto& stage : pipeline->stages) {
            std::cout << "stage " << stage.name << " exit=" << stage.exitCode << " elapsedMs=" << stage.elapsed.count();
            if (stage.name == "inject" || stage.name == "preprocess" || stage.name == "template" || stage.name == "vjass" || stage.name == "check") {
                std::cout << " (" << stage.log << ')';
            }
            std::cout << '\n';
        }
        Print(pipeline->diagnostics);
        std::cout << "syntaxChecked=" << (pipeline->syntaxChecked ? "true" : "false") << " checkedBy=" << pipeline->checkedBy
                  << " finalBytes=" << pipeline->script.size() << '\n';
    }
    std::cout << "elapsedMs=" << elapsed << '\n';
    if (!output.good()) return 1;
    return result.Succeeded() ? 0 : 2;
}

int RunScriptTemplate(const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    const std::filesystem::path& mapPath, const std::filesystem::path& inputPath, const std::filesystem::path& outputPath) {
    auto archive = MapArchiveSession::Open(mapPath);
    if (!archive.Succeeded()) {
        Print(archive.diagnostics);
        return 1;
    }
    const auto standard = LoadStandard(MountGame(gameRoot));
    const auto templates = Script::BuildTemplateContext(toolRoot, standard ? &*standard : nullptr,
        CustomObjects(*archive.value, ObjectData::ObjectType::Ability));
    std::ifstream input(inputPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto started = std::chrono::steady_clock::now();
    const auto result = Script::EvaluateTemplates(text, templates);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Print(result.diagnostics);
    std::ofstream output(outputPath, std::ios::binary);
    output.write(result.text.data(), static_cast<std::streamsize>(result.text.size()));
    std::cout << "expressions=" << result.expressions << " skipped=" << result.skippedBlocks << " elapsedMs=" << elapsed << '\n';
    return result.Succeeded() ? 0 : 1;
}

int RunObjectField(const std::filesystem::path& gameRoot, const std::string& type, const std::string& rawcode) {
    auto game = Game::GameDataSource::Mount(gameRoot);
    if (!game.Succeeded()) return 1;
    auto loaded = ObjectData::StandardObjectCatalog::Load(*game.value);
    if (!loaded.Succeeded()) return 1;
    const auto objectType = type == "ability" ? ObjectData::ObjectType::Ability
        : type == "item" ? ObjectData::ObjectType::Item : ObjectData::ObjectType::Unit;
    const auto* entry = loaded.value->Find(objectType, rawcode);
    if (entry == nullptr) {
        std::cout << "not found\n";
        return 1;
    }
    for (const auto& [key, value] : entry->fields) std::cout << key << '=' << value << '\n';
    return 0;
}
