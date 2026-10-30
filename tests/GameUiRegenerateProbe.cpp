// 手动验收入口：只读脚本白名单与明确的界面描述，复用生产生成/事务/编译服务。
// 输出诊断工件而非可分发包；不打开通用全成员保存会话。
#include "../src/Cli/ScriptBuildCommand.cpp"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include <stdexcept>
#include <nlohmann/json.hpp>

template<class T> T Take(War3Map::Result<T> r) {
    Print(r.diagnostics);
    if (!r.Succeeded()) throw std::runtime_error("production service refused input");
    return std::move(*r.value);
}
int wmain(int argc, wchar_t** argv) {
    if ((argc == 4 || argc == 5) && std::wstring_view(argv[1]) == L"--ability-evidence") {
        try {
            const std::filesystem::path source(argv[2]);
            const auto expected = std::filesystem::path(argv[3]).string();
            if (Take(War3Map::Sha256File(source)) != expected) throw std::runtime_error("source identity mismatch");
            auto archive = Take(MapArchiveSession::OpenScriptAudit(source));
            if (argc == 5) {
                const std::filesystem::path output(argv[4]);
                if (output.extension()!=L".w3a" || std::filesystem::exists(output)) throw std::runtime_error("new ability-only fixture output required");
                const auto bytes=Take(archive.ReadMember("war3map.w3a"));
                if (bytes.size()>16*1024*1024) throw std::runtime_error("ability export budget");
                const auto written=IO::WriteFileBytes(output,bytes);
                if(!written.Succeeded()) throw std::runtime_error("ability export failed");
            }
            nlohmann::json rows = nlohmann::json::array();
            for (const auto& object : CustomObjects(archive, ObjectData::ObjectType::Ability)) {
                if (rows.size() >= 64) throw std::runtime_error("ability evidence budget");
                nlohmann::json fields = nlohmann::json::array();
                for (const auto& change : object.modifications) {
                    if (fields.size() >= 256) throw std::runtime_error("ability field evidence budget");
                    const auto value = ObjectData::ValueToDisplayString(change.value);
                    if (value.size() > 4096) throw std::runtime_error("ability text evidence budget");
                    fields.push_back({{"field",change.key.fieldId},{"level",change.key.level.value_or(0)},
                        {"dataIndex",change.key.dataIndex.value_or(0)},{"value",value}});
                }
                rows.push_back({{"id",object.key.rawcode},{"parent",object.parentRawcode.value_or("")},{"fields",fields}});
            }
            if (Take(War3Map::Sha256File(source)) != expected) throw std::runtime_error("source changed");
            std::cout << rows.dump(2) << '\n'; return 0;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    if (argc != 9 && argc != 10) return 2;
    try {
        const std::filesystem::path source(argv[1]), ui(argv[2]), tool(argv[3]), gameRoot(argv[4]), out(argv[5]);
        if (std::filesystem::exists(out)) throw std::runtime_error("output must be new");
        std::filesystem::create_directories(out);
        auto archive = Take(MapArchiveSession::OpenScriptAudit(source));
        auto catalog = Take(Trigger::UiCatalog::Load(ui));
        auto doc = Take(Trigger::WtgWctCodec{}.Parse(Take(archive.ReadMember("war3map.wtg")),
            Take(archive.ReadMember("war3map.wct")), catalog));
        Trigger::TriggerGraphEditBatch observations{Take(Trigger::CaptureTriggerGraphBinding(doc, catalog)), {}};
        const auto oldReceipt = std::filesystem::path(argv[7]).string();
        const auto newReceipt = std::filesystem::path(argv[8]).string();
        if (oldReceipt.empty() || newReceipt.empty()) throw std::runtime_error("explicit receipt relocation required");
        for (const auto& arg : doc.argumentNodes) {
            auto value = arg.value.text;
            const auto at = value.find(oldReceipt);
            if (at == std::string::npos) continue;
            value.replace(at, oldReceipt.size(), newReceipt);
            observations.operations.emplace_back(Trigger::GraphSet{{Trigger::GraphRecordKind::Argument, arg.id, {}},
                Trigger::GraphField::ArgumentScalar, Trigger::GraphScalarValue{arg.type, arg.value.text},
                Trigger::GraphScalarValue{arg.type, value}});
        }
        if (!observations.operations.empty()) Take(Trigger::ApplyTriggerGraphEditBatch(doc, catalog, observations));
        auto named = Take(Mpq::MpqArchive::OpenReadOnly(source));
        std::vector<GameUi::UiDescription> descriptions;
        for (const auto* id : {"castbar", "party"}) {
            const auto bytes = Take(named.ReadFile(std::string("AudastraUI/") + id + ".json"));
            const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            descriptions.push_back(Take(GameUi::ParseUiDescription(text, id)));
        }
        std::vector<std::string> features;
        if (argc == 10) {
            const std::filesystem::path extra(argv[9]);
            const auto read = [&](const std::string& name, std::string_view extension) {
                const auto path = std::filesystem::path(name);
                if (path.filename() != path || path.extension().string() != extension
                    || !std::filesystem::is_regular_file(extra / path) || std::filesystem::file_size(extra / path) > 1024 * 1024)
                    throw std::runtime_error("explicit bounded JSON/T files required");
                std::ifstream stream(extra / path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(stream), {});
            };
            const auto plan = nlohmann::json::parse(read("plan.json", ".json"));
            if (!plan.is_array() || plan.size() > 16) throw std::runtime_error("bounded probe plan required");
            for (const auto& item : plan) {
                const auto id = item.at("id").get<std::string>();
                if (item.value("fresh", false)) {
                    doc = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog));
                    descriptions.clear();
                }
                if (item.contains("description")) {
                    auto d = Take(GameUi::ParseUiDescription(read(item.at("description").get<std::string>(), ".json"), id));
                    std::erase_if(descriptions, [&](const auto& old) { return old.id == d.id; });
                    descriptions.push_back(std::move(d));
                }
                if (item.contains("features")) features.push_back(read(item.at("features").get<std::string>(), ".txt"));
            }
        }
        for (const auto& description : descriptions) {
            auto generated = Take(GameUi::GenerateUiTriggers(description));
            std::ofstream(out / (description.id + ".t"), std::ios::binary) << generated.featureText;
            Trigger::TriggerGraphEditBatch batch{Take(Trigger::CaptureTriggerGraphBinding(doc, catalog)), {}};
            batch.operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(GameUi::kUiTriggerCategory), generated.featureText, {}, {}});
            Take(Trigger::ApplyTriggerGraphEditBatch(doc, catalog, batch));
        }
        for (const auto& text : features) {
            Trigger::TriggerGraphEditBatch batch{Take(Trigger::CaptureTriggerGraphBinding(doc, catalog)), {}};
            batch.operations.emplace_back(Trigger::GraphApplyFeatureText{"验收", text, {}, {}});
            Take(Trigger::ApplyTriggerGraphEditBatch(doc, catalog, batch));
        }
        auto encoded = Take(Trigger::WtgWctCodec{}.Serialize(doc, catalog));
        // 同一次生产事务的原生触发器输出随验收工件保存，不能手改生成文字。
        std::ofstream(out / "war3map.wtg", std::ios::binary).write(reinterpret_cast<const char*>(encoded.wtg.data()), encoded.wtg.size());
        std::ofstream(out / "war3map.wct", std::ios::binary).write(reinterpret_cast<const char*>(encoded.wct.data()), encoded.wct.size());
        const auto game = MountGame(gameRoot);
        const auto standard = LoadStandard(game);
        Script::MapScriptBuildRequest request;
        request.archive = &archive; request.triggers = &doc; request.catalog = &catalog;
        request.game = game ? &*game : nullptr; request.standard = standard ? &*standard : nullptr;
        request.units = CustomObjects(archive, ObjectData::ObjectType::Unit);
        request.items = CustomObjects(archive, ObjectData::ObjectType::Item);
        request.abilities = CustomObjects(archive, ObjectData::ObjectType::Ability);
        request.layers = catalog.Layers(); request.toolchain.toolRoot = tool; request.toolchain.uiRoot = ui;
        request.toolchain.workDir = out / "compile";
        request.toolchain.jassDirs.push_back(ui / "ui/AudastraUI/jass");
        for (const auto& layer : catalog.Layers()) {
            const auto directory = ui / "ui" / layer / "jass";
            if (layer != "AudastraUI" && std::filesystem::is_directory(directory)) request.toolchain.jassDirs.push_back(directory);
        }
        for (const auto* layer : {"japi", "ydwe", "ydtrigger"})
            request.toolchain.jassDirs.push_back(tool / "ui" / layer / "jass");
        Script::MapRuntimeBootPlan boot; boot.payloadSha256 = std::filesystem::path(argv[6]).string();
        boot.testRunDiagnostics = true; request.toolchain.mapRuntimeBoot = boot;
        auto result = Script::BuildMapScript(request);
        Print(result.diagnostics); Print(result.generated.diagnostics);
        std::ofstream(out / "generated.j", std::ios::binary) << result.generated.text;
        if (result.pipeline) {
            Print(result.pipeline->diagnostics);
            std::ofstream(out / "compiled.j", std::ios::binary) << result.pipeline->script;
        }
        if (!result.Succeeded()) return 1;
        std::cout << "production_regeneration=true\nscript_compile=true\nnormal_map_save=false\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
