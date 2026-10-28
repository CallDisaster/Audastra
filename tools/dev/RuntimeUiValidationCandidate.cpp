#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
// 内部原创界面验收工具：只调用正常生成器、图事务与共用保存服务，不安装、不启动游戏。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;
using namespace War3Map::Services;
template<class T> T Take(Result<T> result) {
    for (const auto& d : result.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    if (!result.Succeeded() || !result.value) throw std::runtime_error("验收步骤失败，保留已有独立产物。");
    return std::move(*result.value);
}
void Require(Status result) {
    for (const auto& d : result.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    if (!result.Succeeded()) throw std::runtime_error("验收事务失败。");
}
std::vector<std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
void Write(const std::filesystem::path& path, const std::string& text) {
    if (std::filesystem::exists(path)) throw std::runtime_error("拒绝覆盖已有证据。");
    std::ofstream file(path, std::ios::binary); file << text;
    if (!file) throw std::runtime_error("写出证据失败。");
}
UiDescription Description() {
    UiDescription d; d.id = "coexist"; d.label = "CoexistTest";
    d.values = {UiValue{"clicks", "Clicks", UiValueType::Integer, 0, "0", {}, {}}};
    UiFrame panel; panel.id = "panel"; panel.type = UiFrameType::Panel;
    panel.width = 0.24; panel.height = 0.11;
    panel.anchors = {{FramePoint::Top, UiAnchorTarget::Screen, {}, FramePoint::Top, 0, -0.06}};
    UiFrame background; background.id = "background"; background.type = UiFrameType::Backdrop;
    background.width = 0.24; background.height = 0.11;
    background.anchors = {{FramePoint::Center, UiAnchorTarget::Parent, {}, FramePoint::Center, 0, 0}};
    background.texture = "UI\\Widgets\\ToolTips\\Human\\human-tooltip-background.blp";
    UiFrame text; text.id = "label"; text.type = UiFrameType::Text; text.width = 0.23; text.height = 0.025;
    text.anchors = {{FramePoint::Top, UiAnchorTarget::Parent, {}, FramePoint::Top, 0, -0.01}};
    text.text = "Audastra ready - clicks: {clicks}";
    UiFrame button; button.id = "button"; button.type = UiFrameType::Button; button.width = 0.16; button.height = 0.035;
    button.anchors = {{FramePoint::Bottom, UiAnchorTarget::Parent, {}, FramePoint::Bottom, 0, 0.015}};
    button.texture = "UI\\Widgets\\Console\\Human\\human-inventory-slotfiller.blp";
    button.pushed = button.texture; button.text = "Click to increment";
    panel.children = {background, text, button}; d.frames = {panel};
    d.events = {UiEvent{"press", "button", UiEventKind::Click}};
    return d;
}
int VerifyReadback(const std::filesystem::path& sourcePath, const std::filesystem::path& candidatePath, const std::filesystem::path& data) {
    auto source = Take(MapArchiveSession::Open(sourcePath)), candidate = Take(MapArchiveSession::Open(candidatePath));
    const auto catalog = Take(UiCatalog::LoadLayers(data, Take(UiCatalog::ReadConfigLayers(data))));
    const auto beforeWtg = Take(source.ReadMember("war3map.wtg")), beforeWct = Take(source.ReadMember("war3map.wct"));
    const auto afterWtg = Take(candidate.ReadMember("war3map.wtg")), afterWct = Take(candidate.ReadMember("war3map.wct"));
    const auto before = Take(WtgWctCodec{}.Parse(beforeWtg, beforeWct, catalog));
    const auto after = Take(WtgWctCodec{}.Parse(afterWtg, afterWct, catalog));
    const auto equal = [](const auto& a, const OriginSpan& x, const auto& b, const OriginSpan& y) {
        if (!x.length || x.length != y.length || x.offset > a.size() || x.length > a.size() - x.offset
            || y.offset > b.size() || y.length > b.size() - y.offset) return false;
        return std::equal(a.begin() + x.offset, a.begin() + x.offset + x.length, b.begin() + y.offset);
    };
    if (after.triggers.size() != before.triggers.size() + 3 || after.variables.size() <= before.variables.size())
        throw std::runtime_error("新增触发器/变量数量与原创计划不符。");
    for (std::size_t i = 0; i < before.triggers.size(); ++i) {
        if (!equal(beforeWtg, before.triggers[i].origin, afterWtg, after.triggers[i].origin)
            || Take(before.triggers[i].customText.ToBytes()) != Take(after.triggers[i].customText.ToBytes()))
            throw std::runtime_error("原有触发器成员或代码字节变化，拒绝验收。");
    }
    for (std::size_t i = 0; i < before.variables.size(); ++i)
        if (!equal(beforeWtg, before.variables[i].origin, afterWtg, after.variables[i].origin))
            throw std::runtime_error("原有变量记录字节变化，拒绝验收。");
    if (Take(before.wct.customCode.ToBytes()) != Take(after.wct.customCode.ToBytes())
        || Take(before.wct.customComment.ToBytes()) != Take(after.wct.customComment.ToBytes()))
        throw std::runtime_error("原有全局代码变化。");
    Require(source.VerifySourceUnchanged()); Require(candidate.VerifySourceUnchanged());
    std::cout << nlohmann::json{{"source_sha256", source.SourceSha256()}, {"candidate_sha256", candidate.SourceSha256()},
        {"original_trigger_count", before.triggers.size()}, {"original_variable_count", before.variables.size()},
        {"added_triggers", after.triggers.size() - before.triggers.size()}, {"added_variables", after.variables.size() - before.variables.size()},
        {"original_trigger_bytes_unchanged", true}, {"original_variable_bytes_unchanged", true}, {"original_code_bytes_unchanged", true}}.dump(2) << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--verify") return VerifyReadback(argv[2], argv[3], argv[4]);
        if (argc != 8) { std::cerr << "source source-sha data-root game-root installed-root package-sha new-evidence-directory\n"; return 2; }
        const std::filesystem::path sourcePath = argv[1], data = argv[3], game = argv[4], install = argv[5], out = argv[7];
        const auto text = [](const wchar_t* arg) { const auto u8 = std::filesystem::path(arg).u8string(); return std::string(u8.begin(), u8.end()); };
        const auto expected = text(argv[2]), package = text(argv[6]);
        auto source = Take(MapArchiveSession::Open(sourcePath));
        if (source.SourceSha256() != expected || std::filesystem::exists(out)) throw std::runtime_error("来源身份变化或输出目录已存在。");
        std::filesystem::create_directory(out);
        auto layers = Take(UiCatalog::ReadConfigLayers(data));
        auto catalog = Take(UiCatalog::LoadLayers(data, layers));
        auto wtg = Take(source.ReadMember("war3map.wtg")), wct = Take(source.ReadMember("war3map.wct"));
        auto document = Take(WtgWctCodec{}.Parse(wtg, wct, catalog));
        const auto description = Description(); UiTriggerOptions options; options.defaultFont = "Fonts\\dfst-m3u.ttf";
        const auto plan = Take(GenerateUiTriggers(description, options));
        const auto update = "gg_trg_" + TriggerDirectoryEditor::ScriptName(UiTriggerName(description, "更新", "Update"));
        const auto callback = "\n触发器 \"CoexistTest ClickCallback\":\n    events:\n        AudastraUITriggerRegisterEvent(\"coexist.press\")\n    actions:\n        SetVariable(udg_AUIV_coexist_clicks, OperatorInt(udg_AUIV_coexist_clicks, OperatorAdd, 1))\n        ConditionalTriggerExecute(" + update + ")\n";
        auto binding = Take(CaptureTriggerGraphBinding(document, catalog));
        TriggerGraphEditBatch batch{binding, {GraphApplyFeatureText{std::string(kUiTriggerCategory), plan.featureText + callback, {}, {}}}};
        Take(ApplyTriggerGraphEditBatch(document, catalog, batch));
        Write(out / "description.audastra.ui", SerializeUiDescription(description));
        Write(out / "generated-triggers.t", plan.featureText + callback);
        // 模板导入是单独的资源保存检查点；原WTG/WCT/脚本完全保留，随后才消费GUI事务。
        MapSaveTask resources; Require(resources.FreezeArchive(source)); resources.target = out / "ui-resources-source.w3x";
        auto imports = source.HasMember("war3map.imp") ? Take(Resources::ImportTable::Parse(Take(source.ReadMember("war3map.imp"))))
            : Resources::ImportTable::CreateEmpty();
        for (const auto& [member, content] : {std::pair{kUiTemplateTocMember, UiTemplateTocText()}, std::pair{kUiTemplateFdfMember, UiTemplateFdfText()}}) {
            if (source.HasMember(member)) throw std::runtime_error("验收源已存在本方模板，拒绝覆盖未知所有者。");
            resources.replacements.push_back({std::string(member), Bytes(content)});
            Require(imports.Upsert(member, [&](std::string_view name) { return source.HasMember(name); }));
        }
        resources.replacements.push_back({"war3map.imp", Take(imports.Serialize())});
        MapSaveService::Run(resources);
        for (const auto& d : resources.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        if (!resources.succeeded) throw std::runtime_error("模板资源保存失败。");
        const auto resourceSha = Take(Sha256File(resources.target));
        TriggerTransactionBinding transaction{resourceSha, 1, {0, expected, catalog.Sha256()}, 1};
        MapEditSaveRequest request; request.sourceMap = resources.target; request.expectedSourceSha256 = resourceSha;
        request.targetCandidate = out / "coexist-ui-candidate.w3x";
        request.toolRoot = data; request.uiRoot = data / "ui"; request.gameRoot = game; request.effectiveLayers = layers;
        request.triggerTransaction = Take(TriggerTransactionSnapshot::Freeze(document, catalog, wtg, wct, transaction));
        request.runtime.purpose = MapRuntimeSavePurpose::InternalValidation;
        request.runtime.decision = MapRuntimeDecision{{MapRuntimeMode::Enabled, package}, true};
        request.runtimePackageSources = Take(LoadInstalledMapRuntimeSources(install));
        const auto receipt = Take(SaveMapEditCandidate(request));
        nlohmann::json json{{"source_sha256", expected}, {"resource_checkpoint_sha256", resourceSha},
            {"candidate_sha256", receipt.candidateSha256}, {"member_count", receipt.memberCount},
            {"runtime_packaged", receipt.runtimePackaged}, {"ui_catalog_sha256", catalog.Sha256()},
            {"script_seconds", receipt.scriptSeconds}, {"triggers", plan.triggers}, {"callback", "CoexistTest ClickCallback"},
            {"original_editor_validated", false}, {"runtime_validated", false}};
        Write(out / "save-receipt.json", json.dump(2)); std::cout << json.dump(2) << '\n';
        Require(source.VerifySourceUnchanged()); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
