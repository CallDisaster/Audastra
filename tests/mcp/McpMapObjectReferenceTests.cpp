#include "McpWorkspace.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
using namespace War3Map::Trigger;
using Bytes = std::vector<std::byte>;
constexpr std::string_view kUnit = "gg_unit_n00Q_0017";
constexpr std::string_view kItem = "gg_item_I001_0003";

void Require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(output.good(), "write isolated original fixture");
}
void Write(const std::filesystem::path& path, std::string_view text) {
    Write(path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}
Bytes Read(const std::filesystem::path& path) {
    const auto result = IO::ReadFileBytes(path, 1024U * 1024U);
    Require(result.Succeeded() && result.value, Codes(result)); return *result.value;
}
Bytes Native(std::string_view child, std::string_view parent, bool extra = false) {
    MapDataFixtures::Fixture writer;
    writer.Int(2); writer.Int(0); writer.Int(extra ? 2 : 1);
    writer.Raw(parent); writer.Raw(child); writer.Int(0);
    if (extra) { writer.Raw(parent); writer.Raw("n00R"); writer.Int(0); }
    return std::move(writer.bytes);
}
Bytes Placements(float x = 0) {
    MapData::UnitPlacements units;
    MapData::UnitPlacement unit; unit.rawcode = "n00Q"; unit.creationId = 17;
    unit.position = {x, 0, 0}; unit.scale = {1, 1, 1}; unit.flags = 2;
    units.units.push_back(unit);
    unit.rawcode = "I001"; unit.creationId = 3; units.units.push_back(unit);
    const auto encoded = units.Serialize(); Require(encoded.Succeeded(), Codes(encoded)); return *encoded.value;
}
GraphRecordRef Ref(GraphRecordKind kind, StableId id) { return {kind, id, {}}; }
GraphArgumentValue Variable(std::string value) {
    GraphArgumentValue result; result.scalar = {1, std::move(value)}; return result;
}
TriggerGraphEditBatch Batch(const McpWorkspace& workspace, const MapObjectReferenceSnapshot* references = nullptr) {
    const auto binding = CaptureTriggerGraphBinding(workspace.VisibleDocument(), workspace.Catalog(), references);
    Require(binding.Succeeded(), Codes(binding)); return {*binding.value, {}};
}
GraphInsertSubtree Assignment(const McpWorkspace& workspace, std::string rhs) {
    const auto& document = workspace.VisibleDocument();
    const auto& trigger = document.triggers.front();
    GraphParentExpectation parent{GraphParentKind::TriggerEcas, Ref(GraphRecordKind::Trigger, trigger.id), {}, {}};
    for (const auto index : trigger.ecas) parent.orderedChildren.push_back(Ref(GraphRecordKind::Eca, document.ecaNodes[index].id));
    auto node = std::make_shared<GraphEcaValue>(); node->function = "SetVariable";
    node->arguments = {Variable("Target"), Variable(std::move(rhs))};
    return {std::move(parent), {}, std::move(node)};
}
EncodedTriggerDocument Pair(const McpWorkspace& workspace) {
    const auto encoded = WtgWctCodec{}.Serialize(workspace.VisibleDocument(), workspace.Catalog());
    Require(encoded.Succeeded(), Codes(encoded)); return *encoded.value;
}
void SamePair(const McpWorkspace& workspace, const EncodedTriggerDocument& expected) {
    const auto actual = Pair(workspace);
    Require(actual.wtg == expected.wtg && actual.wct == expected.wct, "rejected host operation must not publish partial trigger changes");
}
StableId Argument(const McpWorkspace& workspace, std::string_view function, std::size_t position) {
    const auto& document = workspace.VisibleDocument();
    const auto eca = std::find_if(document.ecaNodes.begin(), document.ecaNodes.end(),
        [function](const auto& node) { return node.name.text == function; });
    Require(eca != document.ecaNodes.end() && position < eca->arguments.size(), "original typed argument exists");
    return document.argumentNodes[eca->arguments[position]].id;
}
std::string ArgumentValue(const McpWorkspace& workspace, StableId id) {
    const auto& arguments = workspace.VisibleDocument().argumentNodes;
    const auto found = std::find_if(arguments.begin(), arguments.end(), [id](const auto& node) { return node.id == id; });
    Require(found != arguments.end(), "stable argument remains present"); return found->value.text;
}

struct Fixture {
    std::filesystem::path root, source, ui;
    EncodedTriggerDocument original;
    explicit Fixture(bool withMapObjects = true) {
        static unsigned sequence = 0;
        root = std::filesystem::temp_directory_path() / ("Forge-McpMapObjects-" + std::to_string(GetCurrentProcessId())
            + '-' + std::to_string(GetTickCount64()) + '-' + std::to_string(++sequence));
        source = root / "source"; ui = root / "ui";
        Write(ui / "config", "base\n");
        Write(ui / "base/define.txt", "[TriggerTypes]\nunit=0,1,0,Unit,unit\nitem=0,1,0,Item,item\n"
            "abilcode=0,1,1,Ability,integer\nscriptcode=1,0,0,Script,string\n");
        Write(ui / "base/action.txt", "[SetVariable]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n"
            "[UseItem]\n[[.args]]\ntype=item\n"
            "[LiteralProbe]\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=unit\n");
        const auto catalog = UiCatalog::Load(ui); Require(catalog.Succeeded(), Codes(catalog));
        auto created = WtgWctCodec{}.CreateEmpty(*catalog.value); Require(created.Succeeded(), Codes(created));
        auto& document = *created.value;
        const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), Codes(category));
        const auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "Original"); Require(trigger.Succeeded(), Codes(trigger));
        for (const auto* name : {"Target", "Other"})
            Require(TriggerDocumentEditor::AddVariable(document, name, "unit").Succeeded(), "declare original unit variable");
        Require(TriggerDocumentEditor::AddVariable(document, "ItemTarget", "item").Succeeded(), "declare original item variable");
        Require(TriggerDocumentEditor::AddVariable(document, "Spell", "abilcode").Succeeded(), "declare original ability-code variable");
        const auto assignment = TriggerDocumentEditor::AppendRootEca(document, *catalog.value, *trigger.value,
            UiDefinitionKind::Action, "SetVariable");
        Require(assignment.Succeeded() && assignment.value->appendedArguments.size() == 2, Codes(assignment));
        Require(TriggerDocumentEditor::SetScalarArgument(document, assignment.value->appendedArguments[0].id, 1, "Target").Succeeded()
            && TriggerDocumentEditor::SetScalarArgument(document, assignment.value->appendedArguments[1].id, 1, "Other").Succeeded(),
            "original natural-language assignment is well typed");
        const auto item = TriggerDocumentEditor::AppendRootEca(document, *catalog.value, *trigger.value, UiDefinitionKind::Action, "UseItem");
        Require(item.Succeeded() && item.value->appendedArguments.size() == 1, Codes(item));
        Require(TriggerDocumentEditor::SetScalarArgument(document, item.value->appendedArguments[0].id, 1, "ItemTarget").Succeeded(), "original item reference");
        const auto encoded = WtgWctCodec{}.Serialize(document, *catalog.value); Require(encoded.Succeeded(), Codes(encoded)); original = *encoded.value;
        Write(source / "map/war3map.wtg", original.wtg); Write(source / "map/war3map.wct", original.wct);
        Write(source / "sentinel.txt", "original source outside trigger pair\n");
        if (withMapObjects) {
            MapData::MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.mapName = "Original host fixture";
            const auto w3i = settings.Serialize(); Require(w3i.Succeeded(), Codes(w3i));
            Write(source / "map/war3map.w3i", *w3i.value);
            Write(source / "map/war3mapUnits.doo", Placements());
            Write(source / "map/war3map.w3u", Native("n00Q", "hfoo"));
            Write(source / "map/war3map.w3t", Native("I001", "ratf"));
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::unique_ptr<McpWorkspace> Open() const {
        auto opened = McpWorkspace::Open(source, ui); Require(opened.Succeeded() && opened.value, Codes(opened)); return std::move(*opened.value);
    }
    void SourcePairUnchanged() const {
        Require(Read(source / "map/war3map.wtg") == original.wtg && Read(source / "map/war3map.wct") == original.wct,
            "host must preserve source WTG/WCT");
    }
};

void RealCaptureGraphPreviewAndRollback() {
    Fixture fixture; auto workspace = fixture.Open();
    const auto objects = workspace->MapObjectReferences();
    Require(objects.Succeeded() && objects.value && (*objects.value)->Find(kUnit) && (*objects.value)->Find(kItem), Codes(objects));
    const auto& snapshot = **objects.value;
    Require(snapshot.Find(kUnit)->type == "unit" && snapshot.Find(kUnit)->creationId == 17
        && snapshot.Find(kItem)->type == "item" && snapshot.Identity().size() == 64, "query owns real native category and placement identities");
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Require(transaction.Succeeded(), Codes(transaction));
    auto batch = Batch(*workspace, &snapshot); batch.operations.emplace_back(Assignment(*workspace, std::string(kUnit)));
    const auto changed = workspace->ApplyGraphEdit(*transaction.value, revision, batch);
    Require(changed.Succeeded() && changed.value->before.mapObjectReferencesSha256 == snapshot.Identity()
        && changed.value->after.mapObjectReferencesSha256 == snapshot.Identity(), Codes(changed));
    const auto& document = workspace->VisibleDocument(); const auto& added = document.ecaNodes[document.triggers.front().ecas.back()];
    Require(added.name.text == "SetVariable" && document.argumentNodes[added.arguments[1]].value.text == kUnit,
        "public host publishes real preplaced reference as editable GUI T");
    const auto preview = workspace->PreviewEdit(*transaction.value, revision);
    Require(preview.Succeeded() && !preview.value->Empty(), Codes(preview));
    const auto validation = workspace->ValidateEdit(*transaction.value, revision);
    Require(validation.satisfied, Codes(validation));
    fixture.SourcePairUnchanged();
    Require(workspace->Rollback(*transaction.value, revision).Succeeded() && !workspace->HasActiveTransaction(), "public rollback releases transaction");
    SamePair(*workspace, fixture.original); fixture.SourcePairUnchanged();
}

void LegacyBindingAndWtgOnlyRemainUsable() {
    for (const bool withObjects : {true, false}) {
        Fixture fixture(withObjects); auto workspace = fixture.Open();
        const auto query = workspace->MapObjectReferences();
        Require(query.Succeeded() && query.value, Codes(query));
        Require((*query.value)->References().empty() == !withObjects, "WTG-only query exposes no fabricated external variables");
        const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
        Require(transaction.Succeeded(), Codes(transaction));
        auto ordinary = Batch(*workspace); Require(ordinary.expected.mapObjectReferencesSha256.empty(), "legacy request omits new identity");
        ordinary.operations.emplace_back(Assignment(*workspace, "Other"));
        const auto accepted = workspace->ApplyGraphEdit(*transaction.value, revision, ordinary);
        Require(accepted.Succeeded(), Codes(accepted));
        const auto before = Pair(*workspace);
        auto noQualification = Batch(*workspace); noQualification.operations.emplace_back(Assignment(*workspace, std::string(kUnit)));
        const auto refused = workspace->ApplyGraphEdit(*transaction.value, revision, noQualification);
        Require(!refused.Succeeded() && HasCode(refused, "TRIGGER_GRAPH_VARIABLE_UNKNOWN"), "omitting snapshot cannot authorize a preplaced name");
        SamePair(*workspace, before);
        auto forgedIdentity = Batch(*workspace); forgedIdentity.expected.mapObjectReferencesSha256.assign(64, '0');
        forgedIdentity.operations.emplace_back(Assignment(*workspace, "Other"));
        const auto stale = workspace->ApplyGraphEdit(*transaction.value, revision, forgedIdentity);
        Require(!stale.Succeeded() && HasCode(stale, "TRIGGER_GRAPH_SOURCE_STALE"), "host rejects caller-invented source identity");
        SamePair(*workspace, before);
        Require(workspace->PreviewEdit(*transaction.value, revision).Succeeded()
            && workspace->ValidateEdit(*transaction.value, revision).satisfied, "legacy ordinary T can preview and validate without map data");
        Require(workspace->Rollback(*transaction.value, revision).Succeeded(), "legacy transaction remains rollbackable");
        SamePair(*workspace, fixture.original); fixture.SourcePairUnchanged();
    }
}

void CaptureDiagnosticsSurviveCacheHits() {
    Fixture fixture;
    auto placements = MapData::ParseUnitPlacements(Placements());
    Require(placements.Succeeded(), Codes(placements));
    auto start = placements.value->units.front();
    start.rawcode = "sloc"; start.creationId = 99;
    placements.value->units.push_back(start);
    const auto bytes = placements.value->Serialize(); Require(bytes.Succeeded(), Codes(bytes));
    Write(fixture.source / "map/war3mapUnits.doo", *bytes.value);
    auto workspace = fixture.Open();
    const auto first = workspace->MapObjectReferences();
    const auto cached = workspace->MapObjectReferences();
    Require(first.Succeeded() && cached.Succeeded() && first.value && cached.value,
        "one unsupported record must not discard independently qualified references");
    Require((*first.value)->Find(kUnit) && (*first.value)->Find(kItem)
        && (*first.value)->References().size() == 2, "start location has no generated unit reference");
    Require(HasCode(first, "MAP_OBJECT_REFERENCE_PLACEMENT_UNSUPPORTED")
        && HasCode(cached, "MAP_OBJECT_REFERENCE_PLACEMENT_UNSUPPORTED")
        && Codes(first) == Codes(cached) && *first.value == *cached.value,
        "first and cached queries must both explain omitted map object references");
    Require(Read(fixture.source / "map/war3mapUnits.doo") == *bytes.value,
        "read-only qualification must preserve all original placement bytes");
    fixture.SourcePairUnchanged();
}

void LegacyScalarEntryUsesTheSameQualification() {
    Fixture fixture; auto workspace = fixture.Open();
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Require(transaction.Succeeded(), Codes(transaction));
    const auto rhs = Argument(*workspace, "SetVariable", 1), lhs = Argument(*workspace, "SetVariable", 0);
    const auto unit = workspace->SetScalarArgument(*transaction.value, revision, rhs, 1, std::string(kUnit));
    Require(unit.Succeeded() && ArgumentValue(*workspace, rhs) == kUnit, Codes(unit));
    const auto before = Pair(*workspace);
    for (const auto& [name, code] : {std::pair{"gg_unit_n00Q_9999", "TRIGGER_GRAPH_VARIABLE_UNKNOWN"},
            std::pair{"gg_item_I001_0003", "TRIGGER_GRAPH_TYPE_MISMATCH"}}) {
        const auto rejected = workspace->SetScalarArgument(*transaction.value, revision, rhs, 1, name);
        Require(!rejected.Succeeded() && HasCode(rejected, code), "legacy scalar cannot bypass shared existence/type rules");
        SamePair(*workspace, before);
    }
    const auto readonly = workspace->SetScalarArgument(*transaction.value, revision, lhs, 1, std::string(kUnit));
    Require(!readonly.Succeeded() && HasCode(readonly, "TRIGGER_WRITABLE_VARIABLE_UNKNOWN"), "legacy scalar cannot assign to a preplaced global");
    SamePair(*workspace, before);
    const auto item = Argument(*workspace, "UseItem", 0);
    const auto itemEdit = workspace->SetScalarArgument(*transaction.value, revision, item, 1, std::string(kItem));
    Require(itemEdit.Succeeded(), Codes(itemEdit));
    Require(workspace->SetScalarArgument(*transaction.value, revision, rhs, 1, "Other").Succeeded()
        && ArgumentValue(*workspace, rhs) == "Other", "editing an old preplaced value back to a declaration uses the same guarded path");
    Require(workspace->PreviewEdit(*transaction.value, revision).Succeeded()
        && workspace->ValidateEdit(*transaction.value, revision).satisfied, "legacy scalar result remains a valid paired transaction");
    Require(workspace->Rollback(*transaction.value, revision).Succeeded(), "legacy scalar transaction rolls back");
    SamePair(*workspace, fixture.original); fixture.SourcePairUnchanged();
}

// 直接值与界面共用字面量规则：物编对象编号与自定义代码可写；句柄类型没有固定值写法。
void DirectLiteralsUseTheSharedRules() {
    Fixture fixture; auto workspace = fixture.Open();
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
    Require(transaction.Succeeded(), Codes(transaction));
    const auto appended = workspace->AppendRootEca(*transaction.value, revision, workspace->VisibleDocument().triggers.front().id,
        UiDefinitionKind::Action, "LiteralProbe");
    Require(appended.Succeeded() && appended.value->appendedArguments.size() == 3, Codes(appended));
    const auto ability = appended.value->appendedArguments[0].id, script = appended.value->appendedArguments[1].id,
        unit = appended.value->appendedArguments[2].id;
    const auto code = workspace->SetScalarArgument(*transaction.value, revision, ability, 3, "A04W");
    Require(code.Succeeded() && ArgumentValue(*workspace, ability) == "A04W", Codes(code));
    const auto shortCode = workspace->SetScalarArgument(*transaction.value, revision, ability, 3, "A04");
    Require(!shortCode.Succeeded() && HasCode(shortCode, "TRIGGER_LITERAL_INVALID"), "object codes are exactly four bytes");
    const auto preset = workspace->SetScalarArgument(*transaction.value, revision, ability, 0, "A04W");
    Require(!preset.Succeeded() && HasCode(preset, "MCP_PRESET_NOT_ALLOWED"), "a rawcode is not a preset name");
    const auto variable = workspace->SetScalarArgument(*transaction.value, revision, ability, 1, "udg_Spell");
    Require(variable.Succeeded(), Codes(variable));
    const auto wrongType = workspace->SetScalarArgument(*transaction.value, revision, ability, 1, "udg_ItemTarget");
    Require(!wrongType.Succeeded() && HasCode(wrongType, "MCP_VARIABLE_TYPE_MISMATCH"), "an item variable is not an ability code");
    const auto missing = workspace->SetScalarArgument(*transaction.value, revision, ability, 1, "udg_Missing");
    Require(!missing.Succeeded() && HasCode(missing, "MCP_VARIABLE_UNKNOWN"), "an undeclared variable is reported as unknown");
    const auto text = workspace->SetScalarArgument(*transaction.value, revision, script, 3, "call DoNothing()");
    Require(text.Succeeded() && ArgumentValue(*workspace, script) == "call DoNothing()", Codes(text));
    const auto handle = workspace->SetScalarArgument(*transaction.value, revision, unit, 3, "hfoo");
    Require(!handle.Succeeded() && HasCode(handle, "MCP_DIRECT_TYPE_UNSUPPORTED"), "handle types have no literal form");
    Require(workspace->SetScalarArgument(*transaction.value, revision, unit, 1, "udg_Target").Succeeded(), "handle argument takes a variable");
    Require(workspace->PreviewEdit(*transaction.value, revision).Succeeded(), "literal edits preview");
    const auto literalValidation = workspace->ValidateEdit(*transaction.value, revision);
    Require(literalValidation.satisfied, "literal edits remain a valid paired transaction: " + Codes(literalValidation));
    Require(workspace->Rollback(*transaction.value, revision).Succeeded(), "literal transaction rolls back");
    SamePair(*workspace, fixture.original); fixture.SourcePairUnchanged();
}

void ExternalSourceChangesRejectActiveWork() {
    for (const bool mutateClassification : {false, true}) {
        Fixture fixture; auto workspace = fixture.Open(); const auto objects = workspace->MapObjectReferences();
        Require(objects.Succeeded() && objects.value, Codes(objects));
        const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision);
        Require(transaction.Succeeded(), Codes(transaction));
        auto batch = Batch(*workspace, objects.value->get()); batch.operations.emplace_back(Assignment(*workspace, std::string(kUnit)));
        const auto applied = workspace->ApplyGraphEdit(*transaction.value, revision, batch); Require(applied.Succeeded(), Codes(applied));
        const auto before = Pair(*workspace);
        auto pending = Batch(*workspace, objects.value->get()); pending.operations.emplace_back(Assignment(*workspace, std::string(kUnit)));
        if (mutateClassification) Write(fixture.source / "map/war3map.w3u", Native("n00Q", "hfoo", true));
        else Write(fixture.source / "map/war3mapUnits.doo", Placements(128));
        Require(!workspace->MapObjectReferences().Succeeded(), "query cannot return cached authority after source modification");
        Require(!workspace->ApplyGraphEdit(*transaction.value, revision, pending).Succeeded(), "graph edit rejects externally changed member/category bytes");
        Require(!workspace->SetScalarArgument(*transaction.value, revision, Argument(*workspace, "SetVariable", 1), 1, std::string(kUnit)).Succeeded(),
            "legacy scalar rejects changed source too");
        Require(!workspace->PreviewEdit(*transaction.value, revision).Succeeded()
            && !workspace->ValidateEdit(*transaction.value, revision).satisfied, "preview and validation do not bless stale source authority");
        SamePair(*workspace, before); fixture.SourcePairUnchanged();
        Require(workspace->Rollback(*transaction.value, revision).Succeeded() && !workspace->HasActiveTransaction(), "source conflict retains explicit rollback");
        SamePair(*workspace, fixture.original);
        Require(!workspace->BeginEdit(revision).Succeeded(), "new writer lease requires reopening externally changed source");
    }
}
} // namespace

int main() {
    try {
        RealCaptureGraphPreviewAndRollback();
        LegacyBindingAndWtgOnlyRemainUsable();
        CaptureDiagnosticsSurviveCacheHits();
        LegacyScalarEntryUsesTheSameQualification();
        DirectLiteralsUseTheSharedRules();
        ExternalSourceChangesRejectActiveWork();
        std::cout << "MCP real map object references passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
