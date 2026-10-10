#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <Windows.h>
#include <algorithm>
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
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& item) { return item.code == code; });
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-ScalarGuard-"
        + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument document;
    Fixture() {
        Require(std::filesystem::create_directory(root), "fixture requires a fresh directory");
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\n"
            "string=0,1,1,String,string\nunitcode=0,1,1,UnitId,integer\nscriptcode=0,0,0,Script,string\n");
        Write(root / "ui/base/action.txt", "[StoreInt]\n[[.args]]\ntype=integer\n[StoreReal]\n[[.args]]\ntype=real\n"
            "[StoreString]\n[[.args]]\ntype=string\n[StoreUnitCode]\n[[.args]]\ntype=unitcode\n"
            "[ScriptSlot]\n[[.args]]\ntype=scriptcode\n[CustomScriptCode]\n[[.args]]\ntype=string\n");
        Write(root / "ui/base/call.txt", "[ReadInt]\nreturns=integer\n[MappedDispatch]\nreturns=string\nscript_name=ExecuteFunc\n[[.args]]\ntype=string\n");
        const auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "load original catalog"); catalog = *loaded.value;
        const auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "create sealed fixture"); document = *empty.value;
        const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), "add category");
        const auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "OriginalTrigger"); Require(trigger.Succeeded(), "add trigger");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    StableId Add(std::string_view function, std::int32_t kind, std::string value, StableId triggerId = 0) {
        if (!triggerId) triggerId = document.triggers.front().id;
        const auto appended = TriggerDocumentEditor::AppendRootEca(document, catalog, triggerId, UiDefinitionKind::Action, function);
        Require(appended.Succeeded(), "append actual ECA");
        const auto* eca = TriggerDocumentEditor::FindEca(document, appended.value->rootEcaId);
        Require(eca && eca->arguments.size() == 1, "fixture direct scalar slot");
        const auto argumentId = document.argumentNodes[eca->arguments.front()].id;
        Require(TriggerDocumentEditor::SetScalarArgument(document, argumentId, kind, std::move(value)).Succeeded(), "set scalar");
        return argumentId;
    }
    TriggerGraphBinding Binding() const {
        const auto captured = CaptureTriggerGraphBinding(document, catalog); Require(captured.Succeeded(), "capture real binding"); return *captured.value;
    }
};
void TestScalarBatchMatchesExistingSingleGuards() {
    Fixture fixture;
    Require(TriggerDocumentEditor::AddVariable(fixture.document, "Counter", "integer").Succeeded(), "declared variable");
    std::vector<StableId> ids;
    for (std::size_t index = 0; index < 180; ++index)
        ids.push_back(fixture.Add("StoreInt", index % 2 ? 1 : 3, index % 2 ? "Counter" : std::to_string(index)));
    ids.push_back(fixture.Add("StoreString", 3, "TRIGSTR_7"));
    ids.push_back(fixture.Add("StoreUnitCode", 3, "hfoo"));
    const auto encodedText = fixture.Add("StoreString", 3, "中文原字节");
    auto* legacy = TriggerDocumentEditor::FindArgument(fixture.document, encodedText);
    Require(legacy != nullptr, "legacy scalar exists"); legacy->value.encoding = TextEncoding::Gb18030;
    const std::vector<std::byte> expectedGb{std::byte{0xd6}, std::byte{0xd0}, std::byte{0xce}, std::byte{0xc4},
        std::byte{0xd4}, std::byte{0xad}, std::byte{0xd7}, std::byte{0xd6}, std::byte{0xbd}, std::byte{0xda}};
    const auto encodedGb = legacy->value.ToBytes();
    Require(encodedGb.Succeeded() && *encodedGb.value == expectedGb, "actual GB18030 bytes encode exact original Chinese");
    const auto encoded = WtgWctCodec{}.Serialize(fixture.document, fixture.catalog);
    Require(encoded.Succeeded(), "encode original source fixture including GB18030");
    Require(std::search(encoded.value->wtg.begin(), encoded.value->wtg.end(), expectedGb.begin(), expectedGb.end()) != encoded.value->wtg.end(), "encoded WTG contains actual GB18030 source bytes");
    const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, fixture.catalog);
    Require(parsed.Succeeded(), "parse real source bytes before equality comparison"); fixture.document = *parsed.value;
    const auto parsedGb = std::find_if(fixture.document.argumentNodes.begin(), fixture.document.argumentNodes.end(),
        [](const auto& argument) { return argument.value.text == "中文原字节"; });
    Require(parsedGb != fixture.document.argumentNodes.end() && parsedGb->value.encoding == TextEncoding::Gb18030,
        "parsed source really retains GB18030 encoding");
    Require(!parsedGb->value.originalBytes.empty() && parsedGb->value.originalBytes == expectedGb,
        "parsed original bytes exactly equal first encoding and known correct Chinese bytes");
    ids.clear();
    for (const auto& argument : fixture.document.argumentNodes) ids.push_back(argument.id);
    const auto before = fixture.Binding();
    const auto batch = CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, ids);
    Require(batch.Succeeded() && batch.value->size() == ids.size(), "all requested rows retained in batch");
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto single = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, ids[index]);
        const auto& row = (*batch.value)[index];
        Require(single.Succeeded() && row.argumentId == ids[index] && row.guard && row.diagnostics.empty(), "batch scalar guard eligible");
        Require(*single.value == *row.guard, "entire old public guard and raw fingerprint byte identity preserved");
    }
    const auto after = fixture.Binding();
    Require(before.currentWtgSha256 == after.currentWtgSha256 && before.currentWctSha256 == after.currentWctSha256, "query never changes document");
}
void TestLocalFailuresAndComplexRootsRetained() {
    Fixture fixture;
    const auto good = fixture.Add("StoreInt", 3, "1");
    const auto script = fixture.Add("ScriptSlot", 3, "ExistingCode()");
    const auto dynamic = fixture.Add("CustomScriptCode", 3, "ExistingCode()");
    const auto complex = fixture.Add("StoreInt", 3, "1");
    Require(TriggerDocumentEditor::AppendArgumentCall(fixture.document, fixture.catalog, complex, "integer", "ReadInt").Succeeded(), "existing typed call");
    Require(CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, complex).Succeeded(), "old single closure API unchanged");
    const StableId missing = 0xfffffffffffffff0ULL;
    const std::vector<StableId> ids{good, script, dynamic, complex, missing};
    const auto batch = CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, ids);
    Require(batch.Succeeded() && batch.value->size() == ids.size(), "local rejections are not dropped");
    Require((*batch.value)[0].guard.has_value(), "valid peer retained");
    for (std::size_t index = 1; index < ids.size(); ++index)
        Require((*batch.value)[index].argumentId == ids[index] && !(*batch.value)[index].guard && !(*batch.value)[index].diagnostics.empty(), "each rejected selection has diagnostics");
    Require(HasCode((*batch.value)[1].diagnostics, "TRIGGER_GRAPH_CODE_UNSUPPORTED")
        && HasCode((*batch.value)[2].diagnostics, "TRIGGER_GRAPH_CODE_UNSUPPORTED"), "script slots and mapped dynamic source refused");
    Require(HasCode((*batch.value)[3].diagnostics, "TRIGGER_GRAPH_ARGUMENT_COMPLEX")
        && HasCode((*batch.value)[4].diagnostics, "TRIGGER_GRAPH_ARGUMENT_MISSING"), "complex and missing identities precise");
    const auto parent = fixture.Add("StoreString", 3, "literal");
    const auto call = TriggerDocumentEditor::AppendArgumentCall(fixture.document, fixture.catalog, parent, "string", "MappedDispatch");
    Require(call.Succeeded(), "mapped ancestor fixture");
    const auto* node = TriggerDocumentEditor::FindEca(fixture.document, call.value->rootEcaId);
    Require(node && node->arguments.size() == 1, "mapped ancestor direct parameter");
    const auto nested = fixture.document.argumentNodes[node->arguments.front()].id;
    Require(TriggerDocumentEditor::SetScalarArgument(fixture.document, nested, 3, "ExistingTrigger").Succeeded(), "mapped nested text");
    const std::vector<StableId> nestedIds{nested};
    const auto mapped = CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, nestedIds);
    Require(mapped.Succeeded() && !mapped.value->front().guard
        && HasCode(mapped.value->front().diagnostics, "TRIGGER_GRAPH_CODE_UNSUPPORTED"), "scalar inside mapped code ancestor refused");
    Require(TriggerDocumentEditor::AddVariable(fixture.document, "Array", "integer", true).Succeeded(), "declared array fixture");
    const auto oldGuard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, good);
    Require(oldGuard.Succeeded(), "before actual array source guard");
    GraphArgumentValue array; array.scalar = {1, "Array"};
    auto index = std::make_shared<GraphArgumentValue>(); index->scalar = {3, "0"}; array.index = index;
    TriggerGraphEditBatch arrayBatch{fixture.Binding(), {}};
    arrayBatch.operations.emplace_back(GraphReplaceArgument{{GraphRecordKind::Argument, good, {}}, *oldGuard.value, std::move(array)});
    Require(ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, arrayBatch).Succeeded(), "actual existing array root built by shared graph API");
    const std::vector<StableId> indexedIds{good};
    const auto indexed = CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, indexedIds);
    Require(indexed.Succeeded() && !indexed.value->front().guard
        && HasCode(indexed.value->front().diagnostics, "TRIGGER_GRAPH_ARGUMENT_COMPLEX"), "old indexed root never loses its index subtree");
}
void TestBatchShapeLimitsAndTypedVariableChangesStillAudited() {
    Fixture fixture;
    Require(TriggerDocumentEditor::AddVariable(fixture.document, "Counter", "integer").Succeeded(), "declared variable");
    const auto argument = fixture.Add("StoreInt", 1, "Counter");
    const std::vector<StableId> duplicate{argument, argument}, zero{0}, empty;
    Require(!CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, duplicate).Succeeded(), "duplicate selected IDs refused");
    Require(!CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, zero).Succeeded(), "zero ID refused");
    Require(!CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, empty).Succeeded(), "empty batch refused");
    const std::vector<StableId> tooMany(257, argument);
    Require(!CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, tooMany).Succeeded(), "operation cap checked before full scan");
    TriggerGraphEditLimits limit; limit.maxDepth = 0;
    const std::vector<StableId> one{argument};
    Require(!CaptureScalarArgumentReplacementGuards(fixture.document, fixture.catalog, one, limit).Succeeded(), "unsafe depth refused");
    const auto before = fixture.Binding();
    TriggerGraphEditBatch batch{before, {}};
    batch.operations.emplace_back(GraphSet{{GraphRecordKind::Variable, fixture.document.variables.front().id, {}},
        GraphField::VariableType, std::string("integer"), std::string("string")});
    const auto changed = ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch);
    Require(!changed.Succeeded(), "real typedVariables changes still audit existing references");
    const auto after = fixture.Binding();
    Require(before.currentWtgSha256 == after.currentWtgSha256 && before.currentWctSha256 == after.currentWctSha256, "typed-variable mismatch rolls whole candidate back");
    batch = {after, {}};
    batch.operations.emplace_back(GraphSet{{GraphRecordKind::Argument, argument, {}}, GraphField::ArgumentScalar,
        GraphScalarValue{1, "Counter"}, GraphScalarValue{3, "42"}});
    Require(ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch).Succeeded(), "scalar-only batch still typed and atomic");
}
} // namespace

int main() {
    try {
        TestScalarBatchMatchesExistingSingleGuards();
        TestLocalFailuresAndComplexRootsRetained();
        TestBatchShapeLimitsAndTypedVariableChangesStillAudited();
        std::cout << "scalar replacement guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
