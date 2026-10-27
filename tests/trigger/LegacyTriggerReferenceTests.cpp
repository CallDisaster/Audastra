#include "War3MapCore/Trigger/LegacyTriggerReferences.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << body;
    Require(stream.good(), "write original catalog fixture");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-LegacyTriggerRef-"
        + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument document;
    StableId owner = 0, target = 0;
    Fixture() {
        Require(std::filesystem::create_directory(root), "fixture requires a fresh directory");
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ntrigger=0,1,1,Trigger,trigger\nstring=0,1,1,String,string\n");
        Write(root / "ui/base/action.txt", "[RunTrigger]\n[[.args]]\ntype=trigger\n[ShowText]\n[[.args]]\ntype=string\n");
        const auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "load original catalog"); catalog = *loaded.value;
        const auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "create sealed fixture"); document = *empty.value;
        const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), "add category");
        const auto first = TriggerDocumentEditor::AddTrigger(document, *category.value, "OriginalTrigger"); Require(first.Succeeded(), "add owner");
        const auto second = TriggerDocumentEditor::AddTrigger(document, *category.value, "Target Trigger"); Require(second.Succeeded(), "add target");
        owner = document.triggers.front().id; target = document.triggers.back().id;
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    StableId Add(std::string_view function, std::int32_t kind, std::string value) {
        const auto appended = TriggerDocumentEditor::AppendRootEca(document, catalog, owner, UiDefinitionKind::Action, function);
        Require(appended.Succeeded(), "append actual ECA");
        const auto* eca = TriggerDocumentEditor::FindEca(document, appended.value->rootEcaId);
        Require(eca && eca->arguments.size() == 1, "fixture direct scalar slot");
        auto& argument = document.argumentNodes[eca->arguments.front()];
        // 旧写法按读进来的原样放进 IR，不经编辑器校验。
        argument.type = kind; argument.value = RawText{std::move(value)};
        return argument.id;
    }
    TriggerGraphBinding Binding() const {
        const auto captured = CaptureTriggerGraphBinding(document, catalog); Require(captured.Succeeded(), "capture real binding"); return *captured.value;
    }
};

// 旧写法认得出、只改声明变量参数，WE 写法与字符串原样不动；改完再查为空，旧批次拒绝重放。
void TestFindAndFixLegacyReferences() {
    Fixture fixture;
    const auto legacy = fixture.Add("RunTrigger", 1, "gg_trg_Target Trigger");
    const auto current = fixture.Add("RunTrigger", 1, "gg_trg_Target_Trigger");
    const auto text = fixture.Add("ShowText", 3, "gg_trg_Target Trigger");
    const auto symbols = LegacyTriggerSymbols(fixture.document);
    Require(symbols.contains("gg_trg_Target Trigger") && symbols.at("gg_trg_Target Trigger") == std::vector<StableId>{fixture.target},
        "legacy spelling maps to its trigger");
    Require(!symbols.contains("gg_trg_Target_Trigger") && !symbols.contains("gg_trg_OriginalTrigger"), "WE spellings are not legacy");

    const auto found = FindLegacyTriggerReferences(fixture.document);
    Require(found.size() == 1, "only the legacy variable argument is listed");
    Require(found.front().argumentId == legacy && found.front().ownerTriggerId == fixture.owner
        && found.front().targetTriggerId == fixture.target && found.front().symbol == "gg_trg_Target_Trigger", "reference details");
    Require(FindLegacyTriggerReferences(fixture.document, fixture.target).empty(), "owner filter");

    TriggerGraphEditBatch batch{fixture.Binding(), LegacyTriggerReferenceFixes(found)};
    const auto applied = ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch);
    Require(applied.Succeeded(), "fix applies through the shared graph edit");
    const auto value = [&](StableId id) { return TriggerDocumentEditor::FindArgument(fixture.document, id)->value.text; };
    Require(value(legacy) == "gg_trg_Target_Trigger" && value(current) == "gg_trg_Target_Trigger"
        && value(text) == "gg_trg_Target Trigger", "only the legacy reference changed");
    Require(FindLegacyTriggerReferences(fixture.document).empty(), "nothing left after the fix");

    TriggerGraphEditBatch stale{fixture.Binding(), LegacyTriggerReferenceFixes(found)};
    Require(!ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, stale).Succeeded(), "stale fix refused");
    Require(value(legacy) == "gg_trg_Target_Trigger", "refused batch changes nothing");
}
} // namespace

int main() {
    try {
        TestFindAndFixLegacyReferences();
        std::cout << "legacy trigger reference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
