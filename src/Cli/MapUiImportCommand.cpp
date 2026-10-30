#include "MapUiImportCommand.h"
#include "War3MapCore/ApplicationPaths.h"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <set>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Services;
using Json = nlohmann::json;

template<class T>
T Take(Result<T> result) {
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!result.Succeeded() || !result.value) throw std::runtime_error("MAP_UI_IMPORT_REJECTED: 输入或保存未通过，源图未改。");
    return std::move(*result.value);
}

void Check(Status status) {
    for (const auto& diagnostic : status.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!status.Succeeded()) throw std::runtime_error("MAP_UI_IMPORT_REJECTED");
}

std::string Utf8(const wchar_t* value) {
    const auto bytes = std::filesystem::path(value).u8string();
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

// 计划和输入只接受有界的 UTF-8 文本文件。
std::string ReadText(const std::filesystem::path& path, std::uint64_t budget) {
    if (path.extension() != L".json" && path.extension() != L".txt") throw std::runtime_error("MAP_UI_INPUT_EXTENSION_INVALID");
    const auto bytes = Take(IO::ReadFileBytes(path, budget));
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Take(Trigger::RawText{text, Trigger::TextEncoding::Utf8, {}}.ToBytes());
    return text;
}

// map-ui-import --inspect <源图> <源SHA> <UI根>：列出类别与触发器，供计划项明确写 category。
int Inspect(wchar_t** argv) {
    const std::filesystem::path path(argv[3]);
    const auto expected = Utf8(argv[4]);
    if (Take(Sha256File(path)) != expected) throw std::runtime_error("MAP_UI_SOURCE_CHANGED");
    auto archive = Take(MapArchiveSession::OpenScriptAudit(path));
    auto catalog = Take(Trigger::UiCatalog::Load(argv[5]));
    auto document = Take(Trigger::WtgWctCodec{}.Parse(
        Take(archive.ReadMember("war3map.wtg")), Take(archive.ReadMember("war3map.wct")), catalog));
    if (document.categories.size() > 256 || document.triggers.size() > 2048) throw std::runtime_error("MAP_UI_INSPECTION_BUDGET");
    Json categories = Json::array();
    Json triggers = Json::array();
    for (const auto& category : document.categories) categories.push_back({{"id", category.categoryId}, {"name", category.name.text}});
    for (const auto& trigger : document.triggers) triggers.push_back({{"name", trigger.name.text}, {"category", trigger.categoryId}});
    Check(archive.VerifySourceUnchanged());
    std::cout << Json{{"source_sha256", expected}, {"categories", categories}, {"triggers", triggers}}.dump(2) << '\n';
    return 0;
}

int Import(wchar_t** argv) {
    const std::filesystem::path sourcePath(argv[2]), target(argv[4]), ui(argv[6]), planPath(argv[8]);
    const auto expected = Utf8(argv[3]);
    const auto package = Utf8(argv[10]);
    if (std::filesystem::exists(target)) throw std::runtime_error("MAP_UI_TARGET_EXISTS");
    auto source = Take(MapArchiveSession::Open(sourcePath));
    if (source.SourceSha256() != expected) throw std::runtime_error("MAP_UI_SOURCE_CHANGED");
    auto catalog = Take(Trigger::UiCatalog::Load(ui));
    auto document = Take(Trigger::WtgWctCodec{}.Parse(
        Take(source.ReadMember("war3map.wtg")), Take(source.ReadMember("war3map.wct")), catalog));
    const auto plan = Json::parse(ReadText(planPath, 64 * 1024));
    if (!plan.is_object() || plan.size() != 2 || plan.at("schema") != 1 || !plan.at("items").is_array()
        || plan.at("items").empty() || plan.at("items").size() > 16)
        throw std::runtime_error("MAP_UI_PLAN_INVALID");
    // 输入文件只能是计划同目录下的文件名。
    const auto input = [&](const Json& item, const char* key, const char* extension) {
        const auto path = std::filesystem::u8path(item.at(key).get<std::string>());
        if (path != path.filename() || path.extension() != extension) throw std::runtime_error("MAP_UI_PLAN_PATH_INVALID");
        return ReadText(planPath.parent_path() / path, 1024 * 1024);
    };

    MapSaveTask task;
    Check(task.FreezeArchive(source));
    task.target = target;
    task.runtimePackageSources = Take(LoadInstalledMapRuntimeSources(CurrentInstallationRoot()));
    task.runtimeApprovedPackageSha256 = package;
    task.runtimePurpose = MapRuntimeSavePurpose::Normal;
    task.replacements.push_back(
        {std::string(kMapRuntimeSettingsMember), Take(SerializeMapRuntimeSettings({MapRuntimeMode::Automatic, package}))});
    auto imports = source.HasMember("war3map.imp")
        ? Take(Resources::ImportTable::Parse(Take(source.ReadMember("war3map.imp"))))
        : Resources::ImportTable::CreateEmpty();
    const auto import = [&](std::string name, std::string text) {
        task.replacements.push_back({name, Bytes(text)});
        Check(imports.Upsert(name, [&](std::string_view member) { return source.HasMember(member); }));
    };

    std::set<std::string> ids;
    std::size_t generatedCount = 0;
    for (const auto& item : plan.at("items")) {
        if (!item.is_object() || item.empty() || item.size() > 3 || (!item.contains("description") && !item.contains("triggers")))
            throw std::runtime_error("MAP_UI_PLAN_ITEM_INVALID");
        for (const auto& [key, value] : item.items()) {
            if (key != "description" && key != "triggers" && key != "category") throw std::runtime_error("MAP_UI_PLAN_ITEM_INVALID");
        }
        Trigger::TriggerGraphEditBatch batch{Take(Trigger::CaptureTriggerGraphBinding(document, catalog)), {}};
        std::string category = "导入的界面逻辑";
        if (item.contains("description")) {
            auto description = Take(GameUi::ParseUiDescription(input(item, "description", ".json"), "map-ui-import"));
            category = description.label.empty() ? description.id : description.label;
            if (!ids.insert(description.id).second) throw std::runtime_error("MAP_UI_DESCRIPTION_DUPLICATE");
            const auto generated = Take(GameUi::GenerateUiTriggers(description));
            generatedCount += generated.triggers.size();
            batch.operations.emplace_back(
                Trigger::GraphApplyFeatureText{std::string(GameUi::kUiTriggerCategory), generated.featureText, "ui", {}});
            import("AudastraUI/" + description.id + ".json", GameUi::SerializeUiDescription(description));
        }
        if (item.contains("category")) {
            category = item.at("category").get<std::string>();
            if (category.empty() || category.size() > 256) throw std::runtime_error("MAP_UI_CATEGORY_INVALID");
        }
        if (item.contains("triggers"))
            batch.operations.emplace_back(Trigger::GraphApplyFeatureText{category, input(item, "triggers", ".txt"), "triggers", {}});
        Take(Trigger::ApplyTriggerGraphEditBatch(document, catalog, batch));
    }

    // 与界面“生成触发器”一致更新这两个保留路径；内容只来自内置模板，不接客户端成员字节。
    Json templates = Json::array();
    for (const auto& [name, text] : {std::pair{GameUi::kUiTemplateTocMember, GameUi::UiTemplateTocText()},
             std::pair{GameUi::kUiTemplateFdfMember, GameUi::UiTemplateFdfText()}}) {
        const auto before = source.HasMember(name) ? Take(Sha256(Take(source.ReadMember(name)))) : std::string{};
        templates.push_back({{"member", name}, {"before_sha256", before}, {"after_sha256", Take(Sha256(Bytes(text)))}});
        import(std::string(name), std::string(text));
    }
    task.replacements.push_back({"war3map.imp", Take(imports.Serialize())});
    const auto encoded = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog));
    task.replacements.push_back({"war3map.wtg", encoded.wtg});
    task.replacements.push_back({"war3map.wct", encoded.wct});
    task.triggerChanges = true;

    ScriptSaveInputSources script;
    script.layers = catalog.Layers();
    script.expectedEffectiveLayers = script.layers;
    script.catalog = std::make_unique<Trigger::UiCatalog>(std::move(catalog));
    script.triggers = std::make_unique<Trigger::TriggerDocument>(std::move(document));
    script.toolRoot = argv[5];
    script.uiRoot = ui;
    script.gameRoot = argv[7];
    script.scriptMember = "war3map.j";
    script.documentRevision = 1;
    for (const auto type : {ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item, ObjectData::ObjectType::Ability}) {
        const auto name = std::string(ObjectData::NativeFileName(type));
        if (!source.HasMember(name)) continue;
        auto parsed = ObjectData::NativeObjectCodec(type).Parse(Take(source.ReadMember(name)));
        const auto* objects = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
        if (!objects || !parsed.Succeeded()) throw std::runtime_error("MAP_UI_OBJECT_INPUT_INVALID");
        auto& records = type == ObjectData::ObjectType::Unit ? script.units
            : type == ObjectData::ObjectType::Item           ? script.items
                                                             : script.abilities;
        records.assign(objects->Objects().begin(), objects->Objects().end());
    }
    Take(PrepareScriptSaveInputs(task, std::move(script)));
    MapSaveService::Run(task);
    for (const auto& diagnostic : task.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!task.succeeded) return 1;
    Check(source.VerifySourceUnchanged());

    auto saved = Take(MapArchiveSession::Open(target));
    for (const auto& id : ids) {
        if (!saved.HasMember("AudastraUI/" + id + ".json")) throw std::runtime_error("MAP_UI_READBACK_MISSING");
    }
    std::cout << Json{{"source_sha256", expected}, {"candidate_sha256", saved.SourceSha256()}, {"descriptions", ids},
                     {"generated_triggers", generatedCount}, {"templates", templates}, {"runtime_purpose", "normal"},
                     {"normal_map_save", true}, {"runtime_packaged", saved.HasMember(kMapRuntimeInstallationMember)},
                     {"runtime_validated", false}}
                     .dump(2)
              << '\n';
    FinalizeScriptWorkspace(task);
    return 0;
}

} // namespace

int RunMapUiImport(int argc, wchar_t** argv) {
    try {
        if (argc == 6 && std::wstring_view(argv[2]) == L"--inspect") return Inspect(argv);
        if (argc != 12 || std::wstring_view(argv[9]) != L"--runtime-package" || std::wstring_view(argv[11]) != L"--approve-native-runtime") {
            std::cerr << "usage: map-ui-import <source> <source-sha> <new-map> <tool-root> <ui-root> <game-root> <plan.json> "
                         "--runtime-package <sha> --approve-native-runtime\n";
            return 2;
        }
        return Import(argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
