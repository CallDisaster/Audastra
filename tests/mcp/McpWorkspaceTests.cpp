#include "McpWorkspace.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ByteVector = std::vector<std::byte>;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void Expect(bool condition, std::string_view message) {
    if (!condition) throw TestFailure(std::string(message));
}

class BinaryWriter {
public:
    void I32(std::int32_t value) {
        const std::uint32_t raw = static_cast<std::uint32_t>(value);
        data_.push_back(static_cast<std::byte>(raw));
        data_.push_back(static_cast<std::byte>(raw >> 8U));
        data_.push_back(static_cast<std::byte>(raw >> 16U));
        data_.push_back(static_cast<std::byte>(raw >> 24U));
    }

    void Ascii(std::string_view value) {
        data_.insert(
            data_.end(),
            reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size()));
    }

    void CString(std::string_view value) {
        Ascii(value);
        data_.push_back(std::byte{0});
    }

    void SizedText(std::string_view value) {
        if (value.empty()) {
            I32(0);
            return;
        }
        I32(static_cast<std::int32_t>(value.size() + 1));
        Ascii(value);
        data_.push_back(std::byte{0});
    }

    ByteVector Take() { return std::move(data_); }

private:
    ByteVector data_;
};

void WriteBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw TestFailure("cannot create fixture file");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) throw TestFailure("cannot write fixture file");
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    WriteBytes(path, std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

ByteVector ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw TestFailure("cannot read fixture file");
    const std::vector<char> raw{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    ByteVector bytes(raw.size());
    if (!raw.empty()) {
        std::memcpy(bytes.data(), raw.data(), raw.size());
    }
    return bytes;
}

ByteVector MakeWtg() {
    BinaryWriter writer;
    writer.Ascii("WTG!");
    writer.I32(7);
    writer.I32(1);
    writer.I32(1);
    writer.CString("Category");
    writer.I32(0);
    writer.I32(2);
    writer.I32(3);
    const auto writeVariable = [&](std::string_view name, std::string_view type, bool array) {
        writer.CString(name);
        writer.CString(type);
        writer.I32(1);
        writer.I32(array ? 1 : 0);
        writer.I32(1);
        writer.I32(0);
        writer.CString("");
    };
    writeVariable("UnitVar", "unit", false);
    writeVariable("RealVar", "real", false);
    writeVariable("UnitArray", "unit", true);
    writer.I32(2);

    const auto writeTrigger = [&](std::string_view name, std::string_view action) {
        writer.CString(name);
        writer.CString("");
        writer.I32(0);
        writer.I32(1);
        writer.I32(1);
        writer.I32(0);
        writer.I32(0);
        writer.I32(1);
        writer.I32(1);
        writer.I32(2);
        writer.CString(action);
        writer.I32(1);
        if (action == "LibraryAction") {
            writer.I32(3);
            writer.CString("hfoo");
            writer.I32(0);
            writer.I32(0);
        }
        writer.I32(0);
    };
    writeTrigger("Normal", "DoNothing");
    writeTrigger("Legacy", "LibraryAction");
    return writer.Take();
}

ByteVector MakeWct() {
    BinaryWriter writer;
    writer.I32(1);
    writer.CString("mcp synthetic");
    writer.SizedText("");
    writer.I32(2);
    writer.SizedText("slot-normal");
    writer.SizedText("slot-legacy");
    return writer.Take();
}

std::filesystem::path CreateFixture() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("War3MapEditor64-McpTests-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto ui = root / "ui";
    WriteText(ui / "config", "base\r\n9527/ext\r\n");
    WriteText(ui / "base" / "define.txt",
        "[TriggerParams]\r\n"
        "PresetFootman=0,unit,hfoo,Footman\r\n"
        "PresetReal=0,real,1.0,One\r\n");
    WriteText(ui / "base" / "action.txt",
        "[DoNothing]\r\ntitle=Do nothing\r\n"
        "[SetLife]\r\ntitle=Set life\r\n"
        "[[.args]]\r\ntype=unit\r\n"
        "[[.args]]\r\ntype=real\r\n"
        "[SetCount]\r\ntitle=Set count\r\n"
        "[[.args]]\r\ntype=integer\r\n");
    WriteText(ui / "base" / "call.txt",
        "[GetLastCreatedUnit]\r\ntitle=Last unit\r\nreturns=unit\r\n"
        "[IdentityUnit]\r\ntitle=Identity unit\r\nreturns=unit\r\n"
        "[[.args]]\r\ntype=unit\r\n"
        "[GetInteger]\r\ntitle=Integer\r\nreturns=integer\r\n");
    WriteText(ui / "9527" / "ext" / "action.txt",
        "[LibraryAction]\r\ntitle=Library\r\n"
        "[[.args]]\r\ntype=unit\r\n");

    const auto workspace = root / "source";
    WriteBytes(workspace / "map" / "war3map.wtg", MakeWtg());
    WriteBytes(workspace / "map" / "war3map.wct", MakeWct());
    WriteText(workspace / "sentinel.txt", "source member must stay exact\n");
    std::filesystem::create_directories(root / "out");
    return root;
}

template <typename T>
bool HasDiagnostic(const T& value, std::string_view code) {
    return std::any_of(
        value.diagnostics.begin(), value.diagnostics.end(),
        [code](const War3Map::Diagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

void TestSafeWorkspace() {
    const std::filesystem::path fixture = CreateFixture();
    const std::filesystem::path source = fixture / "source";
    const ByteVector originalWtg = ReadBytes(source / "map" / "war3map.wtg");
    const ByteVector originalWct = ReadBytes(source / "map" / "war3map.wct");
    const ByteVector originalSentinel = ReadBytes(source / "sentinel.txt");
    const std::filesystem::path actionCatalog = fixture / "ui" / "base" / "action.txt";
    const ByteVector originalActionCatalog = ReadBytes(actionCatalog);

    {
        auto opened = War3Map::Mcp::McpWorkspace::Open(source, fixture / "ui");
        Expect(opened.Succeeded(), "safe MCP workspace did not open");
        auto& workspace = **opened.value;
        const War3Map::ProjectRevision revision = workspace.Revision();

        War3Map::ProjectRevision stale = revision;
        ++stale.generation;
        Expect(!workspace.BeginEdit(stale).Succeeded(),
            "stale revision unexpectedly acquired a writer lease");

        auto transaction = workspace.BeginEdit(revision);
        Expect(transaction.Succeeded(), "valid revision did not acquire writer lease");
        Expect(workspace.HasActiveTransaction(), "active transaction was not recorded");

        War3Map::Mcp::VerifiedWarVKModule forgedWarVK{
            "920872221b3836a5eff69d3ec721915b21e0c4b5399c0f09f05b028cf46d27bf",
            "e222e2044ecd3763292c576df34926a0cc6fe954618cc0e3f2fab7bdf31c2606",
            "// Generated from the WarVK v1 manifest.\ncall BJDebugMsg(\"forged\")\n"};
        auto forgedInstall = workspace.InstallGeneratedWarVKModule(
            *transaction.value, revision, 1, std::move(forgedWarVK));
        Expect(!forgedInstall.Succeeded() &&
               HasDiagnostic(forgedInstall, "MCP_WARVK_WRAPPER_HASH_MISMATCH") &&
               workspace.VisibleDocument().triggers.size() == 2,
            "manifest-labelled arbitrary JASS was not rejected before mutation");

        auto trigger = workspace.CreateTrigger(
            *transaction.value, revision, 1, "WarVK AutoTest");
        Expect(trigger.Succeeded(), "normal trigger creation failed");
        auto missingDefinition = workspace.AppendRootEca(
            *transaction.value,
            revision,
            *trigger.value,
            War3Map::Trigger::UiDefinitionKind::Action,
            "MissingAction");
        Expect(!missingDefinition.Succeeded() &&
               HasDiagnostic(missingDefinition, "MCP_ROOT_ECA_NOT_CREATABLE"),
            "an action missing from the Catalog was created in a normal trigger");
        Expect(!workspace.AppendRootEca(
            *transaction.value,
            revision,
            *trigger.value,
            War3Map::Trigger::UiDefinitionKind::Action,
            "UnknownAction").Succeeded(),
            "unknown Catalog root action was accepted");

        auto root = workspace.AppendRootEca(
            *transaction.value,
            revision,
            *trigger.value,
            War3Map::Trigger::UiDefinitionKind::Action,
            "SetLife");
        Expect(root.Succeeded() && root.value->appendedArguments.size() == 2,
            "catalog-backed root ECA did not create two arguments");

        const War3Map::StableId unitArgument = root.value->appendedArguments[0].id;
        const War3Map::StableId realArgument = root.value->appendedArguments[1].id;
        auto wrongCall = workspace.AppendArgumentCall(
            *transaction.value, revision, unitArgument, "GetInteger");
        Expect(!wrongCall.Succeeded() &&
               HasDiagnostic(wrongCall, "TRIGGER_CALL_TYPE_MISMATCH"),
            "Catalog-derived argument type accepted a mismatched call");
        Expect(!workspace.AppendArgumentCall(
            *transaction.value, revision, unitArgument, "UnknownCall").Succeeded(),
            "unknown Catalog call was accepted");
        Expect(!workspace.AppendArgumentCall(
            *transaction.value, revision, realArgument, "GetLastCreatedUnit").Succeeded(),
            "real slot accepted a unit-returning call");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 3, "hfoo").Succeeded(),
            "handle parameter accepted an unproven direct scalar");
        const War3Map::Status wrongPreset = workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 0, "PresetReal");
        Expect(!wrongPreset.Succeeded() &&
               HasDiagnostic(wrongPreset, "MCP_PRESET_TYPE_MISMATCH"),
            "mismatched TriggerParams preset was accepted");
        Expect(workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 0, "PresetFootman").Succeeded(),
            "matching TriggerParams preset was rejected");
        const auto precision = workspace.SetScalarArgument(*transaction.value, revision, realArgument, 3, "0.13799999999999998");
        Expect(precision.Succeeded() && HasDiagnostic(precision, "TRIGGER_REAL_LITERAL_PRECISION_RISK"),
            "legacy scalar publishing retains the common real warning");
        Expect(workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 1, "udg_UnitVar").Succeeded(),
            "matching declared variable was rejected");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 1, "UnitVar").Succeeded(),
            "non-udg variable reference form was accepted");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 1, "udg_RealVar").Succeeded(),
            "wrong-type variable was accepted");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, unitArgument, 1, "udg_UnitArray").Succeeded(),
            "array variable was accepted without an index argument");
        auto nested = workspace.AppendArgumentCall(
            *transaction.value,
            revision,
            unitArgument,
            "IdentityUnit");
        Expect(nested.Succeeded() && nested.value->appendedArguments.size() == 1,
            "typed nested call append failed");
        const War3Map::StableId nestedUnitArgument =
            nested.value->appendedArguments.front().id;
        Expect(workspace.AppendArgumentCall(
            *transaction.value,
            revision,
            nestedUnitArgument,
            "GetLastCreatedUnit").Succeeded(),
            "parent call Catalog slot did not derive its nested argument type");
        Expect(workspace.SetScalarArgument(
            *transaction.value, revision, realArgument, 3, "420").Succeeded(),
            "scalar argument edit failed");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, realArgument, 3, "NaN").Succeeded(),
            "non-finite real text was accepted");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, realArgument, 3, "Inf").Succeeded(),
            "infinite real text was accepted");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, realArgument, 3, "1e9999").Succeeded(),
            "overflowing real text was accepted");
        auto countRoot = workspace.AppendRootEca(
            *transaction.value,
            revision,
            *trigger.value,
            War3Map::Trigger::UiDefinitionKind::Action,
            "SetCount");
        Expect(countRoot.Succeeded() && countRoot.value->appendedArguments.size() == 1,
            "integer fixture action did not append");
        const War3Map::StableId integerArgument =
            countRoot.value->appendedArguments.front().id;
        Expect(workspace.SetScalarArgument(
            *transaction.value, revision, integerArgument, 3, "2147483647").Succeeded(),
            "valid int32 direct value was rejected");
        Expect(!workspace.SetScalarArgument(
            *transaction.value, revision, integerArgument, 3, "2147483648").Succeeded(),
            "overflowing int32 direct value was accepted");

        const auto& document = workspace.VisibleDocument();
        const auto legacy = std::find_if(
            document.triggers.begin(), document.triggers.end(),
            [](const auto& value) { return value.name.text == "Legacy"; });
        Expect(legacy != document.triggers.end() && !legacy->ecas.empty(),
            "numeric-layer fixture trigger missing");
        // 数字命名层（原 9527 特判）按普通函数库编辑。
        const auto& libraryEca = document.ecaNodes[legacy->ecas.front()];
        const War3Map::StableId libraryArgument =
            document.argumentNodes[libraryEca.arguments.front()].id;
        const War3Map::Status libraryWrite = workspace.SetScalarArgument(
            *transaction.value, revision, libraryArgument, 0, "PresetFootman");
        Expect(libraryWrite.Succeeded(), "numeric-layer argument edit was rejected");

        const War3Map::Status noPreviewExport = workspace.ExportCandidate(
            *transaction.value, revision, fixture / "out" / "no-preview");
        Expect(!noPreviewExport.Succeeded() &&
               HasDiagnostic(noPreviewExport, "MCP_CURRENT_GENERATION_NOT_PROVEN"),
            "export succeeded without explicit preview and validation");
        const War3Map::ValidationReport beforePreview =
            workspace.ValidateEdit(*transaction.value, revision);
        Expect(!beforePreview.satisfied &&
               HasDiagnostic(beforePreview, "MCP_PREVIEW_REQUIRED"),
            "validate_edit did not require an explicit current preview");
        auto preview = workspace.PreviewEdit(*transaction.value, revision);
        Expect(preview.Succeeded() && preview.value->changes.size() == 2,
            "preview did not contain the sealed WTG/WCT pair");
        Expect(workspace.SetScalarArgument(
            *transaction.value, revision, realArgument, 3, "421").Succeeded(),
            "post-preview mutation failed");
        const War3Map::Status staleProof = workspace.ExportCandidate(
            *transaction.value, revision, fixture / "out" / "stale-proof");
        Expect(!staleProof.Succeeded() &&
               HasDiagnostic(staleProof, "MCP_CURRENT_GENERATION_NOT_PROVEN"),
            "mutation did not invalidate previous preview/validation proofs");
        Expect(!workspace.ValidateEdit(*transaction.value, revision).satisfied,
            "validation accepted a stale preview generation");
        preview = workspace.PreviewEdit(*transaction.value, revision);
        Expect(preview.Succeeded(), "current generation preview failed");
        const War3Map::Status noValidationExport = workspace.ExportCandidate(
            *transaction.value, revision, fixture / "out" / "no-validation");
        Expect(!noValidationExport.Succeeded() &&
               HasDiagnostic(noValidationExport, "MCP_CURRENT_GENERATION_NOT_PROVEN"),
            "export succeeded without current explicit validation");
        const War3Map::ValidationReport validation =
            workspace.ValidateEdit(*transaction.value, revision);
        Expect(validation.satisfied, "safe transaction did not pass L0 validation");

        const War3Map::Status archive = workspace.ExportCandidate(
            *transaction.value, revision, fixture / "out" / "forbidden.w3x");
        Expect(!archive.Succeeded() &&
               HasDiagnostic(archive, "MCP_ARCHIVE_EXPORT_UNAVAILABLE"),
            "unfinished archive writer did not fail closed");
        Expect(workspace.HasActiveTransaction(),
            "failed archive export incorrectly consumed the transaction");

        const std::filesystem::path existing = fixture / "out" / "existing-candidate";
        WriteText(existing, "must not overwrite\n");
        const ByteVector existingBytes = ReadBytes(existing);
        const War3Map::Status existingStatus = workspace.ExportCandidate(
            *transaction.value, revision, existing);
        Expect(!existingStatus.Succeeded() &&
               HasDiagnostic(existingStatus, "CANDIDATE_ALREADY_EXISTS") &&
               ReadBytes(existing) == existingBytes,
            "existing candidate was not rejected byte-preservingly");

        const std::filesystem::path sidecarCandidate = fixture / "out" / "sidecar-blocked";
        const std::filesystem::path existingSidecar = fixture / "out" /
            "sidecar-blocked.war3me-manifest.json";
        WriteText(existingSidecar, "existing sidecar\n");
        const ByteVector existingSidecarBytes = ReadBytes(existingSidecar);
        const War3Map::Status sidecarStatus = workspace.ExportCandidate(
            *transaction.value, revision, sidecarCandidate);
        Expect(!sidecarStatus.Succeeded() &&
               HasDiagnostic(sidecarStatus, "CANDIDATE_MANIFEST_EXISTS") &&
               !std::filesystem::exists(sidecarCandidate) &&
               ReadBytes(existingSidecar) == existingSidecarBytes,
            "existing manifest sidecar did not fail closed before candidate creation");

        const std::filesystem::path candidate = fixture / "out" / "candidate-expanded";
        Expect(workspace.ExportCandidate(
            *transaction.value, revision, candidate).Succeeded(),
            "expanded candidate export failed");
        Expect(!workspace.HasActiveTransaction(),
            "successful candidate export did not close the transaction");
        Expect(workspace.Revision().generation == revision.generation + 1,
            "successful candidate export did not advance the revision");

        Expect(ReadBytes(source / "map" / "war3map.wtg") == originalWtg &&
               ReadBytes(source / "map" / "war3map.wct") == originalWct,
            "source WTG/WCT changed during overlay export");
        Expect(ReadBytes(candidate / "map" / "war3map.wtg") != originalWtg,
            "candidate WTG did not contain the edits");
        Expect(ReadBytes(candidate / "sentinel.txt") == ReadBytes(source / "sentinel.txt"),
            "unmodified source member changed in the candidate");

        auto reopened = War3Map::Mcp::McpWorkspace::Open(candidate, fixture / "ui");
        Expect(reopened.Succeeded() &&
               (*reopened.value)->VisibleDocument().triggers.size() == 3,
            "candidate WTG/WCT did not re-open with aligned trigger slots");

        const auto revision2 = workspace.Revision();
        auto transaction2 = workspace.BeginEdit(revision2);
        Expect(transaction2.Succeeded(), "second transaction did not start");
        Expect(workspace.CreateTrigger(
            *transaction2.value, revision2, 1, "Source identity probe").Succeeded(),
            "second transaction edit failed");
        Expect(workspace.PreviewEdit(*transaction2.value, revision2).Succeeded(),
            "second transaction preview failed");
        War3Map::ProjectRevision wrongRollbackRevision = revision2;
        ++wrongRollbackRevision.generation;
        Expect(!workspace.Rollback(
            *transaction2.value, wrongRollbackRevision).Succeeded() &&
               workspace.HasActiveTransaction(),
            "rollback accepted the wrong transaction revision identity");
        WriteText(source / "sentinel.txt", "external source mutation\n");
        Expect(!workspace.CreateTrigger(
            *transaction2.value, revision2, 1, "Must not apply").Succeeded(),
            "external source mutation did not immediately block mutation");
        Expect(!workspace.PreviewEdit(*transaction2.value, revision2).Succeeded(),
            "external source mutation did not immediately block preview");
        Expect(!workspace.ValidateEdit(*transaction2.value, revision2).satisfied,
            "external source mutation did not invalidate the transaction");
        Expect(!workspace.ExportCandidate(
            *transaction2.value, revision2, fixture / "out" / "source-changed").Succeeded(),
            "external source mutation did not immediately block export");
        Expect(workspace.Rollback(*transaction2.value, revision2).Succeeded(),
            "rollback did not remain available after source mutation");
        Expect(!workspace.BeginEdit(revision2).Succeeded(),
            "external source mutation did not block begin_edit");
        WriteBytes(source / "sentinel.txt", originalSentinel);

        WriteText(actionCatalog, "external Catalog mutation\n");
        Expect(!workspace.BeginEdit(revision2).Succeeded(),
            "external Catalog mutation did not block begin_edit");
        WriteBytes(actionCatalog, originalActionCatalog);

        auto transaction3 = workspace.BeginEdit(revision2);
        Expect(transaction3.Succeeded(), "Catalog mutation transaction did not start");
        Expect(workspace.CreateTrigger(
            *transaction3.value, revision2, 1, "Catalog identity probe").Succeeded(),
            "Catalog mutation fixture edit failed");
        Expect(workspace.PreviewEdit(*transaction3.value, revision2).Succeeded(),
            "Catalog mutation fixture preview failed");
        Expect(workspace.ValidateEdit(*transaction3.value, revision2).satisfied,
            "Catalog mutation fixture validation failed");
        WriteText(actionCatalog, "external Catalog mutation\n");
        Expect(!workspace.CreateTrigger(
            *transaction3.value, revision2, 1, "Must not apply").Succeeded(),
            "Catalog mutation did not immediately block mutation");
        Expect(!workspace.PreviewEdit(*transaction3.value, revision2).Succeeded(),
            "Catalog mutation did not immediately block preview");
        Expect(!workspace.ValidateEdit(*transaction3.value, revision2).satisfied,
            "Catalog mutation did not immediately block validation");
        Expect(!workspace.ExportCandidate(
            *transaction3.value, revision2, fixture / "out" / "catalog-changed").Succeeded(),
            "Catalog mutation did not immediately block export");
        Expect(workspace.Rollback(*transaction3.value, revision2).Succeeded(),
            "rollback did not remain available after Catalog mutation");
        WriteBytes(actionCatalog, originalActionCatalog);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture, cleanupError);
    Expect(!cleanupError, "test fixture cleanup failed");
}

void TestArchiveTransactionBinding() {
    const auto fixture = CreateFixture();
    const auto source = fixture / "source";
    const auto originalWtg = ReadBytes(source / "map" / "war3map.wtg");
    const auto originalWct = ReadBytes(source / "map" / "war3map.wct");
    auto opened = War3Map::Mcp::McpWorkspace::Open(source, fixture / "ui");
    Expect(opened.Succeeded(), "archive transaction fixture did not open");
    auto& workspace = **opened.value;
    const auto revision = workspace.Revision();
    const auto transaction = workspace.BeginEdit(revision);
    Expect(transaction.Succeeded(), "archive transaction did not start");
    Expect(workspace.CreateTrigger(*transaction.value, revision, 1, "GUI archive edit").Succeeded(),
        "archive transaction GUI edit failed");
    War3Map::Services::MapEditSaveRequest request;
    request.sourceMap = fixture / "missing-source.w3x";
    request.targetCandidate = fixture / "out" / "archive-candidate.w3x";
    request.expectedSourceSha256 = std::string(64, 'a');
    request.toolRoot = fixture / "ui";
    request.gameRoot = fixture;
    const auto exportRequest = [&]() {
        return workspace.ExportArchiveCandidate(*transaction.value, revision, request);
    };
    Expect(HasDiagnostic(exportRequest(), "MCP_CURRENT_GENERATION_NOT_PROVEN"),
        "archive export accepted an unproven generation");
    Expect(workspace.PreviewEdit(*transaction.value, revision).Succeeded(), "archive preview failed");
    Expect(HasDiagnostic(exportRequest(), "MCP_CURRENT_GENERATION_NOT_PROVEN"),
        "archive export accepted an unvalidated generation");
    Expect(workspace.ValidateEdit(*transaction.value, revision).satisfied, "archive validation failed");
    request.expectedSourceSha256.clear();
    Expect(HasDiagnostic(exportRequest(), "MCP_ARCHIVE_SOURCE_REQUIRED"),
        "archive export guessed source identity from the directory revision");
    request.expectedSourceSha256 = std::string(64, 'a');
    request.uiRoot = source;
    Expect(HasDiagnostic(exportRequest(), "MCP_ARCHIVE_UI_ROOT_MISMATCH"), "archive export switched UI root");
    request.uiRoot = fixture / "ui";
    request.effectiveLayers = workspace.Catalog().Layers();
    std::reverse(request.effectiveLayers->begin(), request.effectiveLayers->end());
    Expect(HasDiagnostic(exportRequest(), "MCP_ARCHIVE_UI_LAYERS_MISMATCH"), "archive export reordered layers");
    request.effectiveLayers.reset();
    request.fieldEdits.emplace_back();
    Expect(HasDiagnostic(exportRequest(), "MCP_ARCHIVE_REQUEST_SCOPE_INVALID"), "archive export mixed field edits");
    request.fieldEdits.clear();
    bool externalHookCalled = false;
    request.beforePublish = [&]() { externalHookCalled = true; return War3Map::Status::Ok(); };
    Expect(HasDiagnostic(exportRequest(), "MCP_ARCHIVE_REQUEST_SCOPE_INVALID") && !externalHookCalled,
        "archive export accepted an untrusted publishing hook");
    request.beforePublish = {};
    const auto missingArchive = exportRequest();
    Expect(!missingArchive.Succeeded() && HasDiagnostic(missingArchive, "SOURCE_LEASE_FAILED"),
        "trusted GUI transaction did not reach the common archive service or accepted a missing source");
    Expect(workspace.HasActiveTransaction() && workspace.ActiveTransactionId() == *transaction.value &&
        workspace.Revision() == revision && !std::filesystem::exists(request.targetCandidate) &&
        ReadBytes(source / "map" / "war3map.wtg") == originalWtg &&
        ReadBytes(source / "map" / "war3map.wct") == originalWct,
        "archive failure consumed/rebound the transaction or changed the original pair");
    Expect(workspace.ValidateEdit(*transaction.value, revision).satisfied &&
        workspace.Rollback(*transaction.value, revision).Succeeded(), "failed archive export retained a publishing lease");
    opened.value->reset();
    std::error_code error;
    std::filesystem::remove_all(fixture, error);
    Expect(!error, "archive transaction fixture cleanup failed");
}

} // namespace

int main() {
    try {
        TestSafeWorkspace();
        TestArchiveTransactionBinding();
        std::cout << "War3Map MCP safe workspace tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "War3Map MCP safe workspace tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
