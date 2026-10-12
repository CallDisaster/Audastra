#include "War3MapCore/Hash.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/TerrainBlightSaveService.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "../MapDataFixtures.h"

#include <Windows.h>

#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.code == code) return true;
    return false;
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Forge-map-save-service-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(path_), "create private test directory");
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::filesystem::path Source(const TempDirectory& directory) {
    const auto path = directory.Path() / "source.w3x";
    const std::vector<MapArchiveMember> members{
        {"war3map.wtg", Bytes("original-wtg")},
        {"war3map.wct", Bytes("original-wct")},
        {"war3map.j", Bytes("original-script")},
        {"unmanaged.bin", Bytes("opaque-preserved")},
    };
    auto written = WriteVerifiedArchive(path, members);
    Require(written.Succeeded(), "write original source archive");
    return path;
}

constexpr std::array kClassicV1Types{ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability,
    ObjectData::ObjectType::Item, ObjectData::ObjectType::Upgrade, ObjectData::ObjectType::Doodad,
    ObjectData::ObjectType::Destructable};

std::filesystem::path ObjectV1EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring text(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, text.data(), size);
    Require(length > 0 && length < size, "v1 environment path read");
    text.resize(length);
    return std::filesystem::path(text);
}

std::filesystem::path ObjectV1EvidenceDirectory(std::string_view label) {
    auto root = ObjectV1EnvironmentPath(L"FORGE_OBJECT_V1_EVIDENCE_DIR");
    if (root.empty()) root = std::filesystem::temp_directory_path() / "Forge-object-v1-evidence";
    const auto directory = root / ("services-" + std::string(label) + "-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(directory), "create unique v1 evidence directory");
    return directory; // 有意保留原创来源、独立副本和候选，便于核对逐成员证据。
}

std::string ObjectV1MemberHash(std::span<const std::byte> bytes) {
    const auto hashed = Sha256(bytes);
    Require(hashed.Succeeded() && hashed.value, "hash v1 member evidence");
    return *hashed.value;
}

std::vector<std::byte> SyntheticObjectV1(ObjectData::ObjectType type) {
    MapDataFixtures::Fixture fixture;
    fixture.Int(1); fixture.Int(1); fixture.Raw("hfoo"); fixture.Int(0); fixture.Int(2);
    fixture.Raw("I001"); fixture.Int(0);
    if (type == ObjectData::ObjectType::Ability || type == ObjectData::ObjectType::Upgrade) {
        fixture.Int(2); fixture.Int(3);
    }
    fixture.Int(42); fixture.Raw("hfoo");
    fixture.Raw("S001"); fixture.Int(3);
    if (type == ObjectData::ObjectType::Ability || type == ObjectData::ObjectType::Upgrade) {
        fixture.Int(4); fixture.Int(5);
    }
    fixture.String(std::string_view{"A\xd6\xd0", 3}); fixture.Int(0); fixture.Int(0);
    return fixture.bytes;
}

struct ObjectV1Change {
    ObjectData::ObjectKey object;
    ObjectData::ModificationKey field;
    ObjectData::Value before;
    ObjectData::Value after;
};

ObjectV1Change FirstObjectV1Value(const ObjectData::NativeObjectDocument& native) {
    for (const auto& object : native.Objects()) {
        for (const auto& field : object.modifications) {
            auto changed = field.value;
            if (const auto* integer = std::get_if<std::int32_t>(&field.value)) {
                changed = *integer == std::numeric_limits<std::int32_t>::max() ? *integer - 1 : *integer + 1;
            } else if (const auto* text = std::get_if<std::string>(&field.value)) {
                changed = *text + " FORGE-v1";
            } else if (const auto* real = std::get_if<float>(&field.value); real && std::isfinite(*real)) {
                changed = *real == 0.0f ? 1.0f : std::nextafter(*real, 0.0f);
            } else continue;
            return {object.key, field.key, field.value, std::move(changed)};
        }
    }
    throw std::runtime_error("v1 sample has no supported existing value");
}

void StageObjectV1Value(MapSaveTask& task, ObjectData::ObjectType type,
    const ObjectV1Change& change, bool undo) {
    const auto source = task.archive->ReadMember(ObjectData::NativeFileName(type));
    Require(source.Succeeded() && source.value, "read v1 source member");
    ObjectData::NativeObjectCodec codec(type);
    auto parsed = codec.Parse(*source.value);
    auto* native = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
    Require(native && native->Version() == 1 && native->Supports(ObjectData::NativeObjectOperation::EditExistingValue)
        && !native->IsEditable(), "v1 save task has existing-value capability only");
    const auto text = ObjectData::ConvertNativeObjectDataToLni(type, *source.value);
    Require(text.Succeeded() && text.value, "project v1 source into LNI");
    auto editable = ObjectData::LniObjectCodec(type).Parse(*text.value);
    auto* lni = editable.Succeeded() ? dynamic_cast<ObjectData::LniObjectDocument*>(editable.document.get()) : nullptr;
    Require(lni && lni->SetModification(change.object, change.field, undo ? change.before : change.after).Succeeded(),
        "edit existing v1 field through LNI");
    Require(ObjectData::ApplyLniChangesToNative(*native, *lni).Succeeded(), "apply frozen v1 LNI transaction");
    auto encoded = codec.Serialize(*native);
    Require(encoded.Succeeded() && encoded.value, "encode v1 owned transaction");
    task.replacements.push_back({std::string(ObjectData::NativeFileName(type)), std::move(*encoded.value)});
    task.nextBaselines[static_cast<std::size_t>(type)].reset(
        static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
}

void VerifyObjectV1UnchangedMembers(const MapArchiveSession& before, const MapArchiveSession& after,
    std::optional<ObjectData::ObjectType> changedType) {
    Require(before.Members().size() == after.Members().size(), "v1 save keeps named member count");
    for (const auto& member : before.Members()) {
        if (changedType && ObjectData::ObjectTypeFromNativeFileName(member.name) == changedType) continue;
        const auto bytes = after.ReadMember(member.name);
        Require(bytes.Succeeded() && bytes.value == std::optional{member.data}, "v1 save retains every unrelated named member");
    }
}

void ExerciseObjectV1MapSave(const std::filesystem::path& source, const std::filesystem::path& evidence) {
    const auto sourceHash = Sha256File(source);
    Require(sourceHash.Succeeded() && sourceHash.value, "hash v1 source map");
    const auto copy = evidence / "readonly-input-copy.w3x";
    Require(std::filesystem::copy_file(source, copy), "make new v1 independent test copy");
    Require(Sha256File(copy).value == sourceHash.value, "v1 copy matches source map SHA");
    auto baseline = MapArchiveSession::Open(copy);
    Require(baseline.Succeeded() && baseline.value, "open v1 independent copy");
    std::ofstream receipt(evidence / "members.tsv", std::ios::binary);
    Require(receipt.good(), "open v1 member evidence");
    receipt << "type\tobject\tfield\tlevel\tdata_index\tvariation\tbefore_member_sha\tafter_member_sha\tundo_member_sha\tcandidate_map_sha\tundo_map_sha\n";
    for (const auto type : kClassicV1Types) {
        const auto name = ObjectData::NativeFileName(type);
        const auto bytes = baseline.value->ReadMember(name);
        Require(bytes.Succeeded() && bytes.value, "six-category v1 source member exists");
        ObjectData::NativeObjectCodec codec(type);
        auto parsed = codec.Parse(*bytes.value);
        auto* native = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
        Require(native && native->Version() == 1, "six-category sample really uses version one");
        const auto change = FirstObjectV1Value(*native);
        MapSaveTask task;
        Require(task.FreezeArchive(*baseline.value).Succeeded(), "freeze v1 archive for common save service");
        task.target = evidence / (std::string(ObjectData::ToString(type)) + "-edited.w3x");
        StageObjectV1Value(task, type, change, false);
        MapSaveService::Run(task);
        if (!task.succeeded) for (const auto& diagnostic : task.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(task.succeeded && task.nextSession && !task.scriptRebuilt, "v1 common pure-data save succeeds");
        const auto changedBytes = task.nextSession->ReadMember(name);
        Require(changedBytes.Succeeded() && changedBytes.value && *changedBytes.value != *bytes.value,
            "v1 candidate has actual changed native member");
        auto reread = codec.Parse(*changedBytes.value);
        const auto* changed = reread.Succeeded() ? dynamic_cast<const ObjectData::NativeObjectDocument*>(reread.document.get()) : nullptr;
        const auto* changedObject = changed ? changed->FindObject(change.object) : nullptr;
        Require(changed && changed->Version() == 1 && changedObject, "v1 candidate reopens with same object identity and version");
        bool found = false;
        for (const auto& field : changedObject->modifications) {
            if (field.key == change.field && field.value == change.after) found = true;
        }
        Require(found && codec.Diff(*native, *changed).changes.size() == 1, "v1 candidate contains exactly the requested semantic change");
        VerifyObjectV1UnchangedMembers(*baseline.value, *task.nextSession, type);
        MapSaveTask undo;
        Require(undo.FreezeArchive(*task.nextSession).Succeeded(), "freeze saved v1 source for inverse transaction");
        undo.target = evidence / (std::string(ObjectData::ToString(type)) + "-undo.w3x");
        StageObjectV1Value(undo, type, change, true);
        MapSaveService::Run(undo);
        if (!undo.succeeded) for (const auto& diagnostic : undo.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        Require(undo.succeeded && undo.nextSession, "v1 inverse edit saves through the same service");
        VerifyObjectV1UnchangedMembers(*baseline.value, *undo.nextSession, std::nullopt);
        const auto undoneBytes = undo.nextSession->ReadMember(name);
        Require(undoneBytes.Succeeded() && undoneBytes.value == bytes.value, "v1 undo restores entire native member byte-for-byte");
        receipt << ObjectData::ToString(type) << '\t' << change.object.rawcode << '\t' << change.field.fieldId << '\t'
            << (change.field.level ? std::to_string(*change.field.level) : "none") << '\t'
            << (change.field.dataIndex ? std::to_string(*change.field.dataIndex) : "none") << '\t'
            << (change.field.variation ? std::to_string(*change.field.variation) : "none") << '\t'
            << ObjectV1MemberHash(*bytes.value) << '\t' << ObjectV1MemberHash(*changedBytes.value) << '\t'
            << ObjectV1MemberHash(*undoneBytes.value) << '\t' << task.nextSession->SourceSha256() << '\t'
            << undo.nextSession->SourceSha256() << '\n';
    }
    Require(Sha256File(source).value == sourceHash.value && Sha256File(copy).value == sourceHash.value,
        "v1 source and independent input copy never change");
    Require(receipt.good(), "write v1 member evidence");
    std::ofstream identity(evidence / "source.txt", std::ios::binary);
    identity << "source_sha256=" << *sourceHash.value << "\ncopy_sha256=" << *Sha256File(copy).value
        << "\nevidence=existing-value edits, common pure-data save, archive reread, exact member undo\n"
        << "script_rebuilt=false; native_editor=false; game_runtime=false\n";
    Require(identity.good(), "write v1 source identity");
    const auto pathText = evidence.u8string();
    std::cout << "Object v1 save: " << std::string_view(reinterpret_cast<const char*>(pathText.data()), pathText.size())
        << " source_sha256=" << *sourceHash.value << '\n';
}

void TestClassicV1CommonMapSaveAndTemplateBoundary() {
    const auto evidence = ObjectV1EvidenceDirectory("synthetic");
    std::vector<MapArchiveMember> members{{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
        {"Unmanaged/preserve.bin", {std::byte{0}, std::byte{0xff}, std::byte{0x12}}}};
    for (const auto type : kClassicV1Types) members.push_back({std::string(ObjectData::NativeFileName(type)), SyntheticObjectV1(type)});
    const auto source = evidence / "synthetic-source.w3x";
    Require(WriteVerifiedArchive(source, members).Succeeded(), "write six-category original v1 synthetic archive");
    ExerciseObjectV1MapSave(source, evidence);
    // 只验证已检查产物的提交合同，不把原创 checkedBy 标记冒称真实编译。
    Script::PipelineResult generated;
    generated.script = "function main takes nothing returns nothing\nendfunction\n";
    generated.syntaxChecked = true;
    generated.checkedBy = "synthetic v1 template-boundary fixture";
    for (const auto type : kClassicV1Types) {
        generated.objectMembers = {{std::string(ObjectData::NativeFileName(type)), SyntheticObjectV1(type)}};
        const auto bundle = Script::PrepareScriptSaveBundle(generated, "war3map.j");
        Require(!bundle.Succeeded() && !bundle.value && HasCode(bundle.diagnostics, "SCRIPT_BUNDLE_NATIVE"),
            "v1 existing-value support never permits generated whole-category template replacement");
    }
}

void TestOptionalRealClassicV1MapSave() {
    const auto source = ObjectV1EnvironmentPath(L"FORGE_OBJECT_V1_MAP");
    if (source.empty()) {
        std::cout << "SKIP real v1 map: FORGE_OBJECT_V1_MAP is unset; synthetic success is not real-map evidence\n";
        return;
    }
    ExerciseObjectV1MapSave(source, ObjectV1EvidenceDirectory("real"));
}

void TestFrozenSourceAndVerifiedCandidate() {
    TempDirectory directory;
    const auto source = Source(directory);
    const auto originalHash = Sha256File(source);
    Require(originalHash.Succeeded(), "hash source archive");
    MapSaveTask task;
    {
        auto opened = MapArchiveSession::Open(source);
        Require(opened.Succeeded(), "open source archive");
        Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze source archive");
    } // 执行时原 MapArchiveSession 已销毁，任务只能使用自己的快照。
    task.target = directory.Path() / "candidate.w3x";
    task.replacements = {{"war3map.wtg", Bytes("edited-wtg")}, {"war3map.wct", Bytes("edited-wct")}};
    std::vector<std::string> stages;
    MapSaveService::Run(task, [&](std::string_view stage) { stages.emplace_back(stage); });
    Require(task.succeeded && task.nextSession, "candidate and AfterSave must both succeed");
    Require(stages.size() == 2 && stages[0] == "archive-candidate" && stages[1] == "archive-reopen",
        "execution must report archive write and new baseline");
    const auto candidateHash = Sha256File(task.target);
    Require(candidateHash.Succeeded() && task.nextSession->SourceSha256() == *candidateHash.value,
        "new session must bind written candidate SHA");
    std::error_code pathError;
    Require(std::filesystem::equivalent(task.nextSession->SourcePath(), task.written, pathError) && !pathError,
        "new session must bind written candidate path");
    auto editedWtg = task.nextSession->ReadMember("war3map.wtg");
    auto editedWct = task.nextSession->ReadMember("war3map.wct");
    auto untouched = task.nextSession->ReadMember("unmanaged.bin");
    Require(editedWtg.Succeeded() && *editedWtg.value == Bytes("edited-wtg"), "WTG replaced");
    Require(editedWct.Succeeded() && *editedWct.value == Bytes("edited-wct"), "WCT replaced");
    Require(untouched.Succeeded() && *untouched.value == Bytes("opaque-preserved"), "unknown member retained");
    const auto sourceAfter = Sha256File(source);
    Require(sourceAfter.Succeeded() && *sourceAfter.value == *originalHash.value, "source archive unchanged");
}

void TestIncompleteScriptInputDoesNotWriteCandidate() {
    TempDirectory directory;
    const auto source = Source(directory);
    auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded(), "open script-gate fixture");
    MapSaveTask task;
    Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze script-gate archive");
    task.target = directory.Path() / "rejected.w3x";
    task.script.emplace().scriptMember = "war3map.j"; // 未提供可信 WTG 文档与函数目录。
    MapSaveService::Run(task);
    Require(!task.succeeded && HasCode(task.diagnostics, "SCRIPT_INPUT_MISSING"), "missing script inputs must reject save");
    Require(!std::filesystem::exists(task.target), "rejected candidate must not be published");
}

void TestIndependentUiPreparationGate() {
    TempDirectory directory;
    auto opened = MapArchiveSession::Open(Source(directory));
    Require(opened.Succeeded(), "open independent UI gate source");
    const auto write = [](const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary); output << text;
        Require(output.good(), "write private toolchain fixture");
    };
    const auto tool = directory.Path() / "modern";
    write(tool / "compiler" / "include" / "fixture.j", "// include\n");
    for (const auto* name : {"common.j", "blizzard.j"}) write(tool / "compiler" / "jass" / "24" / name, "// base\n");
    Require(QueryScriptRebuildCapability(tool).state == ScriptRebuildCapabilityState::Ready, "modern entry readiness query");
    Require(QueryScriptRebuildCapability({}).state == ScriptRebuildCapabilityState::Missing, "empty tool root is missing, not ready");
    struct UiCase { const char* name; const char* code; bool legacySkip; };
    const UiCase cases[] = {{"missing-common", "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING", false},
        {"empty-common", "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING", false}, {"missing-include", "SCRIPT_LAYOUT_INCLUDE_MISSING", false},
        {"conflict", "SCRIPT_LAYOUT_AMBIGUOUS", false}, {"legacy", "SCRIPT_TOOLCHAIN_UNSUPPORTED", true},
        {"plain-ui", "SCRIPT_INPUT_MISSING", false}};
    for (const auto& item : cases) {
        const auto ui = directory.Path() / item.name;
        write(ui / "config", "base\n");
        if (std::string_view(item.name) != "plain-ui") {
            write(ui / "plugin" / "wave" / "include" / "fixture.j", "// include\n");
            for (const auto* name : {"common.j", "blizzard.j"}) write(ui / "jass" / "system" / "ht" / name, "// old base\n");
            const auto common = ui / "jass" / "system" / "ht" / "common.j";
            if (std::string_view(item.name) == "missing-common") Require(std::filesystem::remove(common), "remove common fixture");
            else if (std::string_view(item.name) == "empty-common") write(common, "");
            else if (std::string_view(item.name) == "missing-include") {
                Require(std::filesystem::remove(ui / "plugin" / "wave" / "include" / "fixture.j")
                    && std::filesystem::remove(ui / "plugin" / "wave" / "include"), "remove include fixture");
            } else if (std::string_view(item.name) == "conflict") std::filesystem::create_directories(ui / "compiler");
        }
        const auto capability = QueryScriptRebuildCapability(tool, ui);
        const auto expected = item.legacySkip ? ScriptRebuildCapabilityState::Unsupported
            : std::string_view(item.name) == "plain-ui" ? ScriptRebuildCapabilityState::Ready : ScriptRebuildCapabilityState::Unknown;
        Require(capability.state == expected && capability.layout.has_value() == (expected == ScriptRebuildCapabilityState::Ready),
            "read-only query matches preparation gate without publishing partial layout");
        if (item.legacySkip) {
            const auto noTool = QueryScriptRebuildCapability(directory.Path() / "missing-tool", ui);
            Require(noTool.state == ScriptRebuildCapabilityState::Unsupported && noTool.diagnostics.front().code == "SCRIPT_TOOLCHAIN_UNSUPPORTED"
                && HasCode(noTool.diagnostics, "SCRIPT_LAYOUT_ROOT_MISSING"), "legacy UI is unsupported even when compiler root is missing");
            const auto own = QueryScriptRebuildCapability(ui, ui);
            Require(own.state == ScriptRebuildCapabilityState::Ready && own.layout &&
                own.layout->executionPolicy == Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired &&
                !own.layout->semanticsSupported, "own legacy layout is only an entrance requiring captured framework inputs");
            for (const bool allow : {false, true}) {
                MapSaveTask task; Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze framework entrance source");
                ScriptSaveInputSources inputs; inputs.toolRoot = ui; inputs.uiRoot = ui; inputs.allowMissingToolchain = allow;
                const auto prepared = PrepareScriptSaveInputs(task, std::move(inputs));
                Require(!prepared.Succeeded() && HasCode(prepared.diagnostics, "SCRIPT_INPUT_MISSING") &&
                    !task.script && !task.scriptSkipped, "framework entrance cannot convert missing actual script inputs into a skip");
            }
        }
        for (const bool allow : {false, true}) {
            MapSaveTask task; Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze UI gate source");
            ScriptSaveInputSources inputs; inputs.toolRoot = tool; inputs.uiRoot = ui; inputs.allowMissingToolchain = allow;
            const auto prepared = PrepareScriptSaveInputs(task, std::move(inputs));
            const bool skipped = allow && item.legacySkip;
            Require(prepared.Succeeded() == skipped && HasCode(prepared.diagnostics, item.code)
                && task.scriptSkipped == skipped && !task.script, "UI gate preserves failure versus explicitly authorized source-script skip");
            if (skipped) Require(prepared.value == ScriptSavePreparationState::Skipped, "complete legacy UI only returns explicit skip");
        }
    }
}

void TestSharedPerFileEncodingDeclarations() {
    TempDirectory directory;
    const auto write = [](const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary); output << text; Require(output.good(), "write encoding fixture");
    };
    const auto tool = directory.Path() / "framework", ui = tool / "share" / "mpq";
    write(ui / "config", "base\n"); write(ui / "base" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
    write(tool / "plugin" / "wave" / "include" / "WaveForce.i", "// forced\n");
    for (const auto* file : {"common.j", "blizzard.j"}) write(tool / "jass" / "system" / "ht" / file, "// base\n");
    auto opened = MapArchiveSession::Open(Source(directory)); Require(opened.Succeeded(), "open encoding archive");
    // 以归档实际绑定目录声明来源；TEMP的8.3别名不等于已冻结的普通来源路径。
    const auto sourcePath = opened.value->SourcePath().parent_path() / "ambiguous.j";
    const std::string original = "// \xe4\xb8\xad\xe6\x96\x87\n#define VALUE 11\n"; write(sourcePath, original);
    auto declared = ParseScriptSourceEncodingDeclaration(sourcePath, "utf8");
    Require(declared.Succeeded() && !ParseScriptSourceEncodingDeclaration("relative.j", "utf8").Succeeded()
        && !ParseScriptSourceEncodingDeclaration(sourcePath, "auto").Succeeded()
        && !ParseScriptSourceEncodingDeclaration(sourcePath.parent_path() / ".." / "ambiguous.j", "gbk").Succeeded(),
        "shared declaration parser refuses ambiguous encoding choice and unbound path spelling");
    auto alias = *declared.value; auto spelling = alias.path.generic_wstring();
    alias.path = spelling; // 同源斜杠写法不能获得第二个覆盖槽位。
    const auto duplicate = NormalizeScriptSourceEncodingDeclarations({*declared.value, alias});
    Require(!duplicate.Succeeded() && HasCode(duplicate.diagnostics, "SCRIPT_SOURCE_ENCODING_DUPLICATE"), "same file declaration cannot shadow");
    const auto catalog = Trigger::UiCatalog::Load(ui);
    Require(catalog.Succeeded(), "open encoding catalog");
    const auto prepare = [&](MapSaveTask& task, std::vector<Script::ScriptProfileSourceEncoding> declarations,
        const std::filesystem::path& root = std::filesystem::path{},
        std::vector<Script::ScriptProfileIncludeAccess> includeAccess = {}) {
        Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze encoding task");
        ScriptSaveInputSources inputs; inputs.toolRoot = root.empty() ? tool : root; inputs.uiRoot = ui;
        inputs.triggers = std::make_unique<Trigger::TriggerDocument>(); inputs.catalog = std::make_unique<Trigger::UiCatalog>(*catalog.value);
        inputs.layers = catalog.value->Layers(); inputs.scriptMember = "war3map.j"; inputs.sourceEncodings = std::move(declarations);
        inputs.includeAccess = std::move(includeAccess);
        return PrepareScriptSaveInputs(task, std::move(inputs));
    };
    MapSaveTask task;
    Require(prepare(task, {*declared.value}).Succeeded() && task.script && task.script->toolchain.frameworkInputs, "public Sources reaches frozen profile");
    const auto& frozen = *task.script->toolchain.frameworkInputs;
    Require(frozen.profile.sourceEncodings == std::vector<Script::ScriptProfileSourceEncoding>{*declared.value}
        && frozen.entryPlan.profileSourceEncodings == frozen.profile.sourceEncodings
        && Script::ValidateScriptToolchainProfile(frozen.profile).value == frozen.entryPlan.profileIdentitySha256
        && Script::VerifyScriptFrameworkInputs(frozen, *task.script->toolchain.layout).Succeeded(), "declarations share full Capture/Verify identity");
    Script::ScriptPreprocessSourceRequest input; input.mainDirectory = frozen.profile.mainSourceDirectory;
    input.mainText = "#include \"ambiguous.j\"\nfunction main takes nothing returns nothing\nendfunction\n";
    input.frameworkDirectivePolicy = Script::FrameworkDirectivePolicy(frozen);
    const auto captured = Script::CaptureScriptPreprocessSources(frozen.entryPlan, Script::FrameworkSourceRequest(frozen), input);
    if (!captured.Succeeded()) for (const auto& diagnostic : captured.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(captured.Succeeded() && Script::ReplayScriptPreprocessSources(*captured.value).text == captured.value->preprocessed.text,
        "declared ambiguous include reaches identical owned replay");
    bool rawPreserved = false;
    for (const auto& file : captured.value->includes) if (file.binding.path == sourcePath && file.bytes == Bytes(original)) rawPreserved = true;
    Require(rawPreserved, "declared source original bytes remain owned");
    auto altered = frozen; altered.profile.sourceEncodings.front().encoding = Script::ScriptProfileEncoding::Gbk;
    Require(!Script::VerifyScriptFrameworkInputs(altered, *task.script->toolchain.layout).Succeeded(), "encoding change invalidates profile");
    altered = frozen; ++altered.profile.targetVersionMacro;
    Require(!Script::VerifyScriptFrameworkInputs(altered, *task.script->toolchain.layout).Succeeded(), "version identity cannot change through declarations");
    TempDirectory outsideDirectory; const auto outside = outsideDirectory.Path() / "ungranted-source.j";
    write(outside, "// existing but outside every captured source root\n"); altered = frozen;
    altered.profile.sourceEncodings.push_back({outside, Script::ScriptProfileEncoding::Utf8});
    const auto outsideProfile = Script::CaptureScriptFrameworkInputs(altered.profile, altered.layout, altered.catalog);
    Require(outsideProfile.Succeeded(), "unknown declaration is logical identity only");
    auto outsideInput = input; const auto outsideBytes = outside.lexically_relative(directory.Path()).generic_u8string();
    outsideInput.mainText = "$include \"" + std::string(outsideBytes.begin(), outsideBytes.end()) + "\"\n";
    const auto refusedScope = Script::CaptureScriptPreprocessSources(outsideProfile.value->entryPlan,
        Script::FrameworkSourceRequest(*outsideProfile.value), outsideInput);
    Require(!refusedScope.Succeeded() && HasCode(refusedScope.diagnostics, "SCRIPT_INCLUDE_PATH_SCOPE"),
        "unknown declaration grants no outside-root read scope even when its file exists");
    const auto access = ParseScriptIncludeAccessDeclaration(outside, "file", "gbk", true);
    const auto absentPath = outside.parent_path() / "not-created";
    const auto optional = ParseScriptIncludeAccessDeclaration(absentPath, "directory", "auto", false);
    Require(access.Succeeded() && optional.Succeeded() && !optional.value->required && !std::filesystem::exists(absentPath)
        && !ParseScriptIncludeAccessDeclaration(outside, "native", "gbk", true).Succeeded(),
        "shared access parser is declaration-only and does not infer tool/native scope");
    MapSaveTask accessTask;
    Require(prepare(accessTask, {{outside, Script::ScriptProfileEncoding::Utf8}}, {}, {*access.value}).Succeeded()
        && accessTask.script && accessTask.script->toolchain.frameworkInputs, "shared include access reaches frozen task");
    const auto& accessFrozen = *accessTask.script->toolchain.frameworkInputs;
    Require(accessFrozen.profile.ordinaryIncludeAccess == std::vector<Script::ScriptProfileIncludeAccess>{*access.value}
        && Script::VerifyScriptFrameworkInputs(accessFrozen, *accessTask.script->toolchain.layout).Succeeded(),
        "access list participates in common Capture/Verify identity");
    auto accessInput = input; const auto absolute = outside.generic_u8string();
    accessInput.mainText = "#include \"" + std::string(absolute.begin(), absolute.end()) + "\"\n";
    const auto accessCapture = Script::CaptureScriptPreprocessSources(accessFrozen.entryPlan,
        Script::FrameworkSourceRequest(accessFrozen), accessInput);
    Require(accessCapture.Succeeded() && Script::ReplayScriptPreprocessSources(*accessCapture.value).text == accessCapture.value->preprocessed.text,
        "explicit exact-file access and per-file encoding reach owned replay");
    Require(std::filesystem::remove(accessTask.script->toolchain.workDir), "remove access preparation-only workspace");
    MapSaveTask rejected;
    Require(!prepare(rejected, {*declared.value, alias}).Succeeded() && !rejected.script && !rejected.scriptSkipped,
        "invalid declaration batch publishes no partial task");
    const auto modern = directory.Path() / "modern";
    write(modern / "compiler" / "include" / "empty.j", "// modern\n");
    for (const auto* file : {"common.j", "blizzard.j"}) write(modern / "compiler" / "jass" / "24" / file, "// base\n");
    MapSaveTask modernTask; Require(modernTask.FreezeArchive(*opened.value).Succeeded(), "freeze modern declaration task");
    ScriptSaveInputSources modernInputs; modernInputs.toolRoot = modern; modernInputs.sourceEncodings = {*declared.value}; modernInputs.allowMissingToolchain = true;
    const auto refused = PrepareScriptSaveInputs(modernTask, std::move(modernInputs));
    Require(!refused.Succeeded() && HasCode(refused.diagnostics, "SCRIPT_SOURCE_ENCODINGS_UNSUPPORTED")
        && !modernTask.script && !modernTask.scriptSkipped, "modern nonempty declarations do not silently skip");
    MapSaveTask unsupportedAccess; Require(unsupportedAccess.FreezeArchive(*opened.value).Succeeded(), "freeze modern access task");
    ScriptSaveInputSources unsupportedInputs; unsupportedInputs.toolRoot = modern; unsupportedInputs.includeAccess = {*access.value};
    unsupportedInputs.allowMissingToolchain = true;
    const auto refusedAccess = PrepareScriptSaveInputs(unsupportedAccess, std::move(unsupportedInputs));
    Require(HasCode(refusedAccess.diagnostics, "SCRIPT_INCLUDE_ACCESS_UNSUPPORTED")
        && !unsupportedAccess.script && !unsupportedAccess.scriptSkipped, "unsupported access never silently reuses script");
    Require(std::filesystem::remove(task.script->toolchain.workDir), "remove this empty preparation-only workspace");
}

void TestPublicTriggerTextQueryKeepsSourceIdentity() {
    TempDirectory directory;
    const auto ui = directory.Path() / "ui";
    const auto write = [](const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary); output << text;
        Require(output.good(), "write query catalog fixture");
    };
    write(ui / "config", "base\n"); // 与 LoadLayers 合成的 CRLF 字节不同。
    for (const auto* name : {"define.txt", "event.txt", "condition.txt", "call.txt"}) write(ui / "base" / name, "");
    write(ui / "base" / "action.txt", "[DoNothing]\n");
    const auto catalog = Trigger::UiCatalog::Load(ui);
    Require(catalog.Succeeded(), "load raw LF catalog");
    MapDataFixtures::Fixture wtg;
    wtg.Raw("WTG!"); wtg.Int(7); wtg.Int(1); wtg.Int(1); wtg.String("Category"); wtg.Int(0);
    wtg.Int(2); wtg.Int(0); wtg.Int(1); wtg.String("Plain"); wtg.String("");
    for (const auto value : {0, 1, 0, 0, 0, 1, 0}) wtg.Int(value);
    MapDataFixtures::Fixture wct;
    const auto sized = [&](std::string_view text) { wct.Int(static_cast<std::int32_t>(text.size() + 1)); wct.String(text); };
    wct.Int(1); wct.String(""); sized("//中文\n"); wct.Int(1); sized("old-slot");
    const auto parsed = Trigger::WtgWctCodec{}.Parse(wtg.bytes, wct.bytes, *catalog.value);
    Require(parsed.Succeeded(), "parse bounded query source pair");
    const auto source = directory.Path() / "query-source.w3x";
    const std::vector<MapArchiveMember> members{{"war3map.wtg", wtg.bytes}, {"war3map.wct", wct.bytes}};
    const auto written = WriteVerifiedArchive(source, members);
    Require(written.Succeeded(), "write query source fixture");
    MapTriggerTextQueryRequest request;
    request.sourceMap = source; request.uiRoot = ui; request.expectedSourceSha256 = written.value->sha256;
    request.expectedCatalogSha256 = catalog.value->Sha256(); request.query.limit = 4;
    const auto page = QueryMapTriggerText(request);
    Require(page.Succeeded() && page.value->page.catalogSha256 == catalog.value->Sha256()
        && page.value->page.wtgSha256 == parsed.value->SourceWtgSha256() && page.value->page.text == "//",
        "public query uses exact raw config SHA and paired source instead of synthetic CRLF identity");
    request.query = {Trigger::TriggerTextSlot::TriggerCode, parsed.value->triggers[0].id, 0, 65536};
    const auto gui = QueryMapTriggerText(request);
    Require(gui.Succeeded() && !gui.value->page.customText && gui.value->guiTarget
        && gui.value->guiTarget->expectedRootIds.empty() && gui.value->page.text == "old-slot",
        "GUI text query supplies explicit full empty-tree binding without converting it");
    request.expectedCatalogSha256 = std::string(64, 'a');
    Require(HasCode(QueryMapTriggerText(request).diagnostics, "TRIGGER_EDIT_SOURCE_STALE"), "stale query catalog refused");
    const auto after = Sha256File(source);
    Require(after.Succeeded() && *after.value == request.expectedSourceSha256, "queries preserve source map SHA");
}

void TestPreflightConsumesFrozenTaskWithoutPublishing() {
    TempDirectory directory;
    const auto source = Source(directory);
    const auto hash = Sha256File(source);
    auto opened = MapArchiveSession::Open(source);
    Require(hash.Succeeded() && opened.Succeeded(), "open preflight fixture");
    MapSaveTask task;
    Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze independent preflight task");
    task.replacements = {{"war3map.wtg", Bytes("next-wtg")}, {"war3map.wct", Bytes("next-wct")}};
    Require(MapSaveService::Preflight(task).Succeeded(), "preflight needs no output target");
    Require(task.executed && !task.succeeded && !task.nextSession && task.written.empty()
        && task.backup.empty(), "preflight must not advertise archive publication or commit");
    for (const auto& entry : std::filesystem::directory_iterator(directory.Path()))
        Require(entry.path() == source, "preflight creates no archive or backup");
    const auto after = Sha256File(source);
    Require(after.Succeeded() && *after.value == *hash.value, "preflight preserves source bytes");
    task.target = directory.Path() / "reused.w3x";
    MapSaveService::Run(task);
    Require(HasCode(task.diagnostics, "SAVE_TASK_ALREADY_EXECUTED")
        && !std::filesystem::exists(task.target), "preflight cannot bypass a fresh save snapshot");
}

void TestPreflightUsesSharedGuardsAndChecksLateSourceChanges() {
    TempDirectory directory;
    const auto source = Source(directory);
    auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded(), "open preflight rejection fixture");
    for (const auto& bad : {std::string("war3map.wtg"), std::string("../unsafe.bin")}) {
        MapSaveTask task;
        Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze rejected preflight");
        task.replacements = {{bad, Bytes("rejected")}};
        const auto checked = MapSaveService::Preflight(task);
        Require(!checked.Succeeded() && HasCode(checked.diagnostics,
            bad == "war3map.wtg" ? "TRIGGER_PAIR_REQUIRED" : "BUILD_NAME_UNSAFE"),
            "preflight preserves archive writer rejection reasons");
    }
    MapSaveTask script;
    Require(script.FreezeArchive(*opened.value).Succeeded(), "freeze incomplete script preflight");
    script.script.emplace().scriptMember = "war3map.j";
    Require(HasCode(MapSaveService::Preflight(script).diagnostics, "SCRIPT_INPUT_MISSING"),
        "preflight shares the script build gate");
    MapSaveTask stale;
    Require(stale.FreezeArchive(*opened.value).Succeeded(), "freeze late source check");
    const auto checked = MapSaveService::Preflight(stale, [&](std::string_view stage) {
        if (stage == "archive-preflight") {
            std::ofstream changed(source, std::ios::binary | std::ios::app);
            changed.put('X');
            Require(changed.good(), "modify only the private source fixture");
        }
    });
    Require(!checked.Succeeded() && HasCode(checked.diagnostics, "SOURCE_CHANGED"),
        "source changed after task preparation must reject preflight");
}

void TestVersionedFormatCannotBypassScriptGate() {
    TempDirectory directory;
    const auto baseline = MapDataFixtures::FullMapSettingsV25();
    const auto parsed = MapData::MapSettings::Parse(baseline.bytes);
    Require(parsed.Succeeded() && parsed.value, "parse complete classic W3I gate fixture");
    auto settings = *parsed.value;
    settings.formatVersion = 31;
    settings.gameVersion = {1, 32, 10, 20456};
    const auto encoded = settings.Serialize();
    Require(encoded.Succeeded() && encoded.value, "serialize versioned W3I gate fixture");
    const auto source = directory.Path() / "versioned-source.w3x";
    const std::vector<MapArchiveMember> modernMembers{{"war3map.w3i", *encoded.value},
        {"war3map.wtg", Bytes("original-wtg")}};
    Require(WriteVerifiedArchive(source, modernMembers).Succeeded(), "write versioned save gate fixture");
    auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded() && opened.value, "open versioned save gate fixture");
    MapSaveTask task;
    Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze versioned save gate fixture");
    task.target = directory.Path() / "versioned-rejected.w3x";
    task.replacements = {{"war3map.wtg", Bytes("edited-wtg")}};
    // 故意不提供脚本快照，证明 MapSaveService 本身在任何写出之前拒绝。
    MapSaveService::Run(task);
    Require(!task.succeeded && HasCode(task.diagnostics, "SAVE_MAP_FORMAT_UNSUPPORTED")
        && !std::filesystem::exists(task.target), "v31 source cannot bypass explicit save gate");

    settings.formatVersion = 25;
    settings.gameVersion = {};
    const auto classic = settings.Serialize();
    Require(classic.Succeeded() && classic.value, "serialize classic W3I gate fixture");
    MapData::UnitPlacements placements;
    MapData::UnitPlacement unit;
    unit.rawcode = "H001";
    unit.scale = {1.0f, 1.0f, 1.0f};
    placements.units.push_back(unit);
    auto classicUnits = placements.Serialize();
    Require(classicUnits.Succeeded() && classicUnits.value, "serialize classic unit gate fixture");
    auto skinnedUnits = *classicUnits.value;
    const std::array<std::byte, 4> skin{std::byte{'H'}, std::byte{'0'}, std::byte{'0'}, std::byte{'2'}};
    skinnedUnits.insert(skinnedUnits.begin() + 52, skin.begin(), skin.end());
    const auto skinRead = MapData::ReadUnitPlacementsVersioned(skinnedUnits);
    Require(skinRead.Succeeded() && skinRead.value->recordLayout == MapData::PlacementRecordLayout::SkinId,
        "SkinId fixture must actually select the new layout");
    const auto skinSource = directory.Path() / "skin-source.w3x";
    const std::vector<MapArchiveMember> skinMembers{{"war3map.w3i", *classic.value},
        {"war3mapUnits.doo", skinnedUnits}, {"war3map.wtg", Bytes("original-wtg")}};
    Require(WriteVerifiedArchive(skinSource, skinMembers).Succeeded(), "write SkinId save gate fixture");
    opened = MapArchiveSession::Open(skinSource);
    Require(opened.Succeeded() && opened.value, "open SkinId save gate fixture");
    MapSaveTask skinTask;
    Require(skinTask.FreezeArchive(*opened.value).Succeeded(), "freeze SkinId save gate fixture");
    skinTask.target = directory.Path() / "skin-rejected.w3x";
    skinTask.replacements = {{"war3map.wtg", Bytes("edited-wtg")}};
    MapSaveService::Run(skinTask);
    Require(!skinTask.succeeded && HasCode(skinTask.diagnostics, "SAVE_PLACEMENT_LAYOUT_UNSUPPORTED")
        && !std::filesystem::exists(skinTask.target), "SkinId source cannot bypass explicit save gate");
}

void TestThrowingProgressCallbackDoesNotChangeSaveResult() {
    TempDirectory directory;
    const auto source = Source(directory);
    auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded(), "open progress-callback fixture");
    MapSaveTask task;
    Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze progress-callback archive");
    task.target = directory.Path() / "progress-callback.w3x";
    MapSaveService::Run(task, [](std::string_view) { throw std::runtime_error("display failed"); });
    Require(task.succeeded && task.nextSession, "throwing progress callback must not abort archive save");
    Require(std::filesystem::exists(task.target), "candidate must be published despite progress callback failure");
    Require(!HasCode(task.diagnostics, "SAVE_INTERNAL_CPP_EXCEPTION")
        && !HasCode(task.diagnostics, "SAVE_INTERNAL_NONSTANDARD_EXCEPTION"), "progress callback must not misreport archive failure");
}

void TestProgramExceptionsFailClosedWithoutCollectingPrivateWhat() {
    TempDirectory directory;
    const auto source = Source(directory);
    const auto identity = Sha256File(source);
    Require(identity.Succeeded(), "hash exception-boundary source");
    auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded(), "open exception-boundary fixture");
    for (const bool standardException : {true, false}) {
        MapSaveTask task;
        Require(task.FreezeArchive(*opened.value).Succeeded(), "freeze exception-boundary source");
        task.target = directory.Path() / (standardException ? "std-rejected.w3x" : "nonstandard-rejected.w3x");
        task.beforePublish = [standardException]() -> Status {
            if (standardException) throw std::runtime_error("private original map script must not enter automatic diagnostics");
            throw 123;
        };
        MapSaveService::Run(task);
        Require(!task.succeeded && !task.nextSession && !std::filesystem::exists(task.target), "exception refuses archive publication");
        Require(HasCode(task.diagnostics, standardException ? "SAVE_INTERNAL_CPP_EXCEPTION" : "SAVE_INTERNAL_NONSTANDARD_EXCEPTION"),
            "standard and nonstandard internal exceptions have explicit classifications");
        for (const auto& diagnostic : task.diagnostics)
            Require(diagnostic.message.find("private original map script") == std::string::npos, "exception what is not collected");
        Require(Sha256File(source).value == identity.value, "exception preserves exact original source");
    }
}

void TestBlightSaveRejectsStaleOrMissingTerrainPair() {
    TempDirectory directory;
    const auto source = Source(directory);
    const auto actual = Sha256File(source);
    Require(actual.Succeeded(), "hash blight-gate source");
    TerrainBlightSaveRequest request;
    request.sourceMap = source;
    request.targetCandidate = directory.Path() / "blight-rejected.w3x";
    request.toolRoot = directory.Path() / "tool";
    request.gameRoot = directory.Path() / "game";
    request.expectedSourceSha256 = std::string(64, '0');
    request.expectedTerrainSha256 = std::string(64, '0');
    auto stale = SaveTerrainBlightCandidate(request);
    Require(!stale.Succeeded() && HasCode(stale.diagnostics, "SAVE_SOURCE_STALE"),
        "blight save must bind the caller's source SHA");
    request.expectedSourceSha256 = *actual.value;
    auto missing = SaveTerrainBlightCandidate(request);
    Require(!missing.Succeeded() && HasCode(missing.diagnostics, "SAVE_TERRAIN_PATHING_MISSING"),
        "blight save must reject a source without W3E and WPM");
    Require(!std::filesystem::exists(request.targetCandidate), "failed blight saves must not publish a candidate");
}

void TestGenericTerrainSaveRejectsUnverifiedAmplitude() {
    // 真实地图来源由专用验收运行显式提供；CI 无地图资产时仍执行上面的合成门禁。
    const DWORD capacity = GetEnvironmentVariableW(L"FORGE_D1_PARITY_MAP", nullptr, 0);
    if (capacity == 0) return;
    std::wstring text(capacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"FORGE_D1_PARITY_MAP", text.data(), capacity);
    Require(length > 0 && length < capacity, "read isolated terrain map path");
    text.resize(length);
    const std::filesystem::path mapPath(text);
    TempDirectory directory;
    auto opened = MapArchiveSession::Open(mapPath);
    Require(opened.Succeeded() && opened.value, "open isolated terrain map");
    auto terrain = opened.value->ReadMember("war3map.w3e");
    Require(terrain.Succeeded() && terrain.value, "read isolated W3E");
    auto terrainSha = Sha256(*terrain.value);
    Require(terrainSha.Succeeded() && terrainSha.value, "hash isolated W3E");
    MapEditSaveRequest request;
    request.sourceMap = mapPath;
    request.targetCandidate = directory.Path() / "height-over-evidence-limit.w3x";
    request.toolRoot = directory.Path() / "unused-tool";
    request.gameRoot = directory.Path() / "unused-game";
    request.expectedSourceSha256 = opened.value->SourceSha256();
    request.expectedTerrainSha256 = *terrainSha.value;
    MapEditTerrainStroke stroke;
    stroke.brush.kind = MapData::TerrainBrushKind::Raise;
    stroke.brush.radius = 1;
    stroke.brush.strength = 64.0f; // raw 净差最多 256，超过原版真保存已取证的 156。
    stroke.pointSpace = MapEditPointSpace::TerrainVertex;
    stroke.points.push_back({10, 10});
    request.terrainStrokes.push_back(std::move(stroke));
    auto result = SaveMapEditCandidate(request);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "MAP_TERRAIN_PATHING_UNVERIFIED"),
        "generic height save must retain the bounded native-evidence gate");
    Require(!std::filesystem::exists(request.targetCandidate), "over-limit height cannot publish a candidate");
    std::cout << "Generic terrain over-limit height reached WPM safety gate\n";
}

// 一张含五类成员的源图；请求像 Agent 一样先从源文档读行身份与字段原值，再写新值。
struct FieldFixture {
    TempDirectory directory;
    std::filesystem::path source;
    std::optional<MapDocument> map;
    std::string sourceSha;
};

void BuildFieldFixture(FieldFixture& fixture, std::size_t unitCount = 1) {
    MapData::SoundRecord sound{};
    sound.name = "gg_snd_GLHF";
    sound.filePath = "war3mapImported\\GLHF.wav";
    sound.eaxSetting = "DefaultEAXON";
    sound.volume = 127;
    sound.pitch = 1.0f;
    sound.coneOrientation = {0.0f, 0.0f, 1.0f};
    MapData::MapSettings settings;
    settings.formatVersion = 25;
    settings.mapName = "TRIGSTR_003";
    settings.custom.emplace();
    MapData::UnitPlacement unit;
    unit.rawcode = "nvil";
    unit.position = {-4625.814453f, 733.926270f, 0.0f};
    unit.rotation = std::bit_cast<float>(0x40bd7ed1u);
    unit.owner = 15; unit.creationId = 86;
    MapData::UnitPlacements units;
    for (std::size_t index = 0; index < unitCount; ++index) {
        unit.creationId = static_cast<std::int32_t>(86 + index); units.units.push_back(unit);
    }
    MapData::RegionRecord region{3936.0f, 1824.0f, 4192.0f, 1984.0f, "Barrels01", 169, {}, "", {0, 0, 0, 255}};
    auto firstRegion = region;
    firstRegion.creationNumber = 168;
    MapData::CameraRecord camera{54.62953567504883f, 128.0f, 0.0f, 0.0f, 0.0f,
        1650.0f, 0.0f, 70.0f, 5000.0f, 100.0f, "Intro 01A"};
    auto w3s = MapData::SoundsDocument{1, {sound}, {}}.Serialize();
    auto w3i = settings.Serialize();
    auto doo = units.Serialize();
    auto w3r = MapData::RegionsDocument{5, {firstRegion, region}, {}}.Serialize();
    auto w3c = MapData::CamerasDocument{0, {camera}, {}}.Serialize();
    Require(w3s.Succeeded() && w3i.Succeeded() && doo.Succeeded() && w3r.Succeeded() && w3c.Succeeded(),
        "serialize field fixture members");
    fixture.source = fixture.directory.Path() / "field-source.w3x";
    const std::vector<MapArchiveMember> members{{"war3map.w3s", *w3s.value}, {"war3map.w3i", *w3i.value},
        {"war3mapUnits.doo", *doo.value}, {"war3map.w3r", *w3r.value}, {"war3map.w3c", *w3c.value},
        {"war3map.wts", Bytes("STRING 3\n{\nOriginal\n}\n")},
        {"war3map.wtg", Bytes("original-wtg")}, {"war3map.wct", Bytes("original-wct")}};
    Require(WriteVerifiedArchive(fixture.source, members).Succeeded(), "write field fixture archive");
    auto opened = MapArchiveSession::Open(fixture.source);
    Require(opened.Succeeded() && opened.value, "open field fixture archive");
    fixture.sourceSha = opened.value->SourceSha256();
    fixture.map.emplace(MapDocument::FromArchive(*opened.value));
}

MapEditFieldOperation Edit(const FieldFixture& fixture, MapDataKind kind, std::size_t row,
    std::string_view fieldId, std::string_view input) {
    const auto* field = MapData::FindMapField(kind, fieldId);
    Require(field != nullptr, "field is in the catalog");
    auto key = MapData::MapFieldRowKey(*fixture.map, kind, row);
    auto before = MapData::ReadMapField(*fixture.map, kind, row, *field);
    auto after = MapData::ParseMapFieldInput(*field, input);
    auto memberSha = Sha256(fixture.map->Member(kind).sources.front().data);
    Require(key.Succeeded() && before.Succeeded() && after.Succeeded() && memberSha.Succeeded(),
        "read row key, raw value and member SHA from the source document");
    return {*memberSha.value, {kind, row, *key.value, std::string(fieldId), *before.value, *after.value}};
}

std::vector<Diagnostic> SaveFields(const FieldFixture& fixture, std::vector<MapEditFieldOperation> edits) {
    MapEditSaveRequest request;
    request.sourceMap = fixture.source;
    request.targetCandidate = fixture.directory.Path() / "field-candidate.w3x";
    request.toolRoot = request.gameRoot = fixture.directory.Path();
    request.expectedSourceSha256 = fixture.sourceSha;
    request.fieldEdits = std::move(edits);
    auto result = SaveMapEditCandidate(request);
    Require(!result.Succeeded() && !std::filesystem::exists(request.targetCandidate),
        "fixture without script inputs never publishes a field candidate");
    return result.diagnostics;
}

bool HasFieldGateCode(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code.starts_with("MAP_FIELD_") || diagnostic.code.starts_with("SAVE_FIELD_")
            || diagnostic.code == "SAVE_EDIT_SCOPE_INVALID") return true;
    }
    return false;
}

void TestFieldEditsBindSourceAndStayInScope() {
    FieldFixture fixture;
    BuildFieldFixture(fixture);
    using K = MapDataKind;
    // 五个成员、十四项修改在一个事务里通过全部字段门禁，止于缺脚本输入。区域整体右移
    // 需要同一请求同时改 left/right，单改 left 会越过右边界。
    const std::vector<MapEditFieldOperation> valid{
        Edit(fixture, K::Sounds, 0, "volume", "80"),
        Edit(fixture, K::Settings, 0, "map_name", "FORGE_WP2_A"),
        Edit(fixture, K::Settings, 0, "game_data_set", "2"),
        Edit(fixture, K::Units, 0, "facing", "90"),
        Edit(fixture, K::Regions, 1, "left", "4200"),
        Edit(fixture, K::Regions, 1, "right", "4400"),
        Edit(fixture, K::Cameras, 0, "target_x", "86.62953186035156"),
        Edit(fixture, K::Cameras, 0, "distance", "900"),
        Edit(fixture, K::Cameras, 0, "z_offset", "32"),
        Edit(fixture, K::Sounds, 0, "fade_in", "20"),
        Edit(fixture, K::Sounds, 0, "fade_out", "-1"),
        Edit(fixture, K::Units, 0, "owner", "7"), Edit(fixture, K::Units, 0, "hit_points", "50"),
        Edit(fixture, K::Units, 0, "mana", "60")};
    Require(!HasFieldGateCode(SaveFields(fixture, valid)), "multi-member field edits pass shared gates");

    const auto rejects = [&fixture](std::vector<MapEditFieldOperation> edits, std::string_view code) {
        Require(HasCode(SaveFields(fixture, std::move(edits)), code), code);
    };
    auto stale = valid[0]; stale.expectedMemberSha256 = std::string(64, '0');
    rejects({stale}, "SAVE_FIELD_MEMBER_STALE");
    stale = valid[0]; stale.edit.expected.integer = 126;
    rejects({stale}, "MAP_FIELD_VALUE_STALE");
    stale = valid[4]; stale.edit.row = 0;
    rejects({stale}, "MAP_FIELD_ROW_STALE");
    rejects({Edit(fixture, K::Sounds, 0, "pitch", "1.5")}, "MAP_FIELD_READ_ONLY");
    rejects({Edit(fixture, K::Sounds, 0, "volume", "128")}, "MAP_FIELD_VALUE_INVALID");
    rejects({Edit(fixture, K::Sounds, 0, "fade_in", "-2")}, "MAP_FIELD_VALUE_INVALID");
    rejects({Edit(fixture, K::Cameras, 0, "distance", "-1")}, "MAP_FIELD_VALUE_INVALID");
    rejects({Edit(fixture, K::Cameras, 0, "z_offset", "2000000")}, "MAP_FIELD_VALUE_INVALID");
    for (const auto& [field, value] : {std::pair{"owner", "16"}, {"hit_points", "101"}, {"mana", "-2"}})
        rejects({Edit(fixture, K::Units, 0, field, value)}, "MAP_FIELD_VALUE_INVALID");
    rejects({valid[4]}, "MAP_FIELD_ROW_INVALID");
    rejects({valid[0], valid[0]}, "MAP_FIELD_DUPLICATE");
    auto mixed = valid[1]; mixed.expectedMemberSha256 = std::string(64, 'a');
    rejects({valid[2], mixed}, "SAVE_FIELD_REQUEST_INVALID");
}

void TestFieldValueTextIsExact() {
    const auto* facing = MapData::FindMapField(MapDataKind::Units, "facing");
    const auto* left = MapData::FindMapField(MapDataKind::Regions, "left");
    const auto* name = MapData::FindMapField(MapDataKind::Settings, "map_name");
    Require(facing && left && name, "catalog exposes writable fields");
    auto degrees = MapData::ParseMapFieldInput(*facing, "90");
    Require(degrees.Succeeded() && std::bit_cast<float>(degrees.value->floatBits)
        == static_cast<float>(90.0 * std::numbers::pi_v<double> / 180.0), "degrees convert to radians like the UI");
    auto raw = MapData::ParseMapFieldRaw(left->type, "f32:44000000");
    Require(raw.Succeeded() && raw.value->floatBits == 0x44000000u
        && MapData::FormatMapFieldRaw(*raw.value) == "f32:44000000", "float raw text round-trips bit-exactly");
    auto bytes = MapData::ParseMapFieldRaw(name->type, "hex:545249475354525f303033");
    Require(bytes.Succeeded() && bytes.value->bytes == "TRIGSTR_003"
        && MapData::FormatMapFieldRaw(*bytes.value) == "hex:545249475354525f303033", "byte raw text round-trips");
    Require(!MapData::ParseMapFieldRaw(left->type, "512").Succeeded(), "float binding requires exact bits");
    const auto& life = *MapData::FindMapField(MapDataKind::Units, "hit_points");
    const auto& mana = *MapData::FindMapField(MapDataKind::Units, "mana");
    for (const auto& [maximum, expected] : {std::pair{500.0, 80}, {100.0, 100}}) {
        const auto parsed = MapData::ParseMapFieldInput(life, "400", {{maximum}});
        Require(parsed.Succeeded() && parsed.value->integer == expected, "absolute life clamps per object maximum");
    }
    Require(MapData::ParseMapFieldInput(life, "1", {{300.0}}).value->integer == 0
        && MapData::ParseMapFieldInput(life, "50%", {}).value->integer == 50
        && MapData::ParseMapFieldInput(mana, "50%", {{120.0}}).value->integer == 60,
        "life quantizes to integer percentage while mana stores absolute value");
    Require(!MapData::ParseMapFieldInput(life, "400", {}).Succeeded()
        && !MapData::ParseMapFieldInput(mana, "50%", {}).Succeeded()
        && !MapData::ParseMapFieldInput(life, "101%", {}).Succeeded(), "unknown maxima and invalid percentages rejected");
    const auto nativeMana = MapData::ParseMapFieldInput(mana, "400", {});
    Require(nativeMana.Succeeded() && nativeMana.value->integer == 400
        && HasCode(nativeMana.diagnostics, "MAP_FIELD_RUNTIME_CLAMP"), "absolute mana preserves raw value with runtime clamp diagnostic");
    Require(!MapData::ParseMapFieldInput(mana, "2147483648", {}).Succeeded()
        && !MapData::ParseMapFieldInput(mana, "400", {{-1.0}}).Succeeded(), "unknown maximum does not bypass integer or invalid-source bounds");
    for (const auto* field : {&life, &mana}) {
        const auto inherited = MapData::ParseMapFieldInput(*field, "默认", {});
        Require(inherited.Succeeded() && MapData::IsMapFieldDefault(*field, *inherited.value), "default sentinel preserved");
    }
    FieldFixture bulk; BuildFieldFixture(bulk, 33);
    std::vector<MapData::MapFieldEdit> edits;
    for (std::size_t row = 0; row < 33; ++row) edits.push_back(Edit(bulk, MapDataKind::Units, row, "owner", "2").edit);
    Require(!MapData::ApplyMapFieldEdits(*bulk.map, edits).Succeeded(), "Agent default request budget unchanged");
    const auto planned = MapData::ApplyMapFieldEdits(*bulk.map, edits, edits.size());
    Require(planned.Succeeded() && planned.value->units->units.back().owner == 2
        && bulk.map->Units()->units.back().owner == 15, "whole selection plans atomically beyond 32 without source mutation");
}

void TestDFieldsUseOriginalSaveWorkerAndUndoReplay() {
    using K = MapDataKind;
    FieldFixture gate;
    BuildFieldFixture(gate);
    Require(!HasFieldGateCode(SaveFields(gate, {Edit(gate, K::Settings, 0, "author", "作者"),
        Edit(gate, K::Settings, 0, "description", "说明"), Edit(gate, K::Settings, 0, "recommended_players", "2–4"),
        Edit(gate, K::Strings, 0, "value", "共享名称")})),
        "D多个成员通过真实MapEditSaveCandidate字段回读，继续受缺脚本门禁限制");
    TempDirectory directory;
    const auto source = directory.Path() / "d-source.w3x";
    const auto originalW3i = MapDataFixtures::FullMapSettingsV25().bytes;
    const auto originalWts = Bytes("// keep\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\n");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", originalW3i}, {"war3map.wts", originalWts},
        {"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")}, {"private/preserve.bin", Bytes("preserved")}};
    Require(WriteVerifiedArchive(source, members).Succeeded(), "原创D合成归档");
    const auto sourceSha = Sha256File(source);
    auto opened = MapArchiveSession::Open(source); Require(opened.Succeeded(), "打开独立D归档来源");
    const auto original = MapDocument::FromArchive(*opened.value);
    MapData::MapDataEditSession edits(original);
    const auto apply = [&](K kind, const char* fieldId, const char* text) {
        auto current = MapData::ProjectMapData(original, edits); Require(current.Succeeded(), "原会话D当前投影");
        const auto* field = MapData::FindMapField(kind, fieldId);
        const auto before = MapData::ReadMapField(*current.value, kind, 0, *field);
        const auto after = MapData::ParseMapFieldInput(*field, text);
        const auto key = MapData::MapFieldRowKey(*current.value, kind, 0);
        const auto sha = Sha256(current.value->Member(kind).sources.front().data);
        Require(before.Succeeded() && after.Succeeded() && key.Succeeded() && sha.Succeeded(), "D原值/SHA合同");
        const std::array changes{MapData::MapFieldEdit{kind, 0, *key.value, fieldId, *before.value, *after.value}};
        const auto prepared = MapData::PrepareMapDataFields(original, *current.value, *sha.value, changes);
        Require(prepared.Succeeded() && edits.ApplyPreparedFields(*prepared.value).Succeeded(), "D沿原密封命令提交");
    };
    apply(K::Strings, "value", "共享描述"); apply(K::Settings, "author", "保存作者");
    const auto projected = MapData::ProjectMapData(original, edits);
    const auto frozen = MapData::FreezeOriginalMapDataFields(original, *projected.value);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 2 && frozen.value->Operations().size() == 2, "D原来源累计冻结");
    MapSaveTask save;
    Require(save.FreezeArchive(*opened.value).Succeeded(), "原保存worker冻结同来源归档");
    save.target = directory.Path() / "d-candidate.w3x"; save.replacements = edits.Replacements();
    MapSaveService::Run(save);
    Require(save.succeeded && save.nextSession, "D真实原保存worker写归档并独立重开");
    const auto saved = MapDocument::FromArchive(*save.nextSession);
    Require(saved.Settings()->author == "保存作者" && saved.Settings()->description == "TRIGSTR_0042"
        && saved.ResolveString(saved.Settings()->description).value == std::optional<std::string>("共享描述"),
        "真实归档回读作者、原wire引用及共享文字");
    Require(saved.Member(K::Settings).sources.front().data == projected.value->Member(K::Settings).sources.front().data
        && saved.Member(K::Strings).sources.front().data == projected.value->Member(K::Strings).sources.front().data,
        "归档回读完整D字节与密封候选一致");
    for (const auto& member : members) if (member.name != "war3map.w3i" && member.name != "war3map.wts") {
        const auto preserved = save.nextSession->ReadMember(member.name);
        Require(preserved.Succeeded() && *preserved.value == member.data, "D纯数据保存保持脚本与不受管成员原字节");
    }
    edits.CommitSaved(saved);
    Require(!edits.IsDirty() && edits.Undo() && edits.Undo() && edits.IsDirty(), "真实保存后原Undo历史仍可恢复两个D命令");
    MapSaveTask undo;
    Require(undo.FreezeArchive(*save.nextSession).Succeeded(), "Undo保存冻结最近真实归档基线");
    undo.target = directory.Path() / "d-undo.w3x"; undo.replacements = edits.Replacements();
    MapSaveService::Run(undo);
    Require(undo.succeeded && undo.nextSession, "原Undo变化经相同保存worker再保存");
    const auto restored = MapDocument::FromArchive(*undo.nextSession);
    Require(restored.Member(K::Settings).sources.front().data == originalW3i && restored.Member(K::Strings).sources.front().data == originalWts,
        "Undo保存独立回读恢复完整原W3I/WTS含注释与CRLF");
    Require(sourceSha.Succeeded() && Sha256File(source).value == sourceSha.value, "全部候选操作保持原来源归档SHA");
}

std::vector<MapArchiveMember> SoundSaveMembers(std::vector<std::string> names = {"Raw Sound", "gg_snd_Unchanged"}) {
    MapData::SoundsDocument sounds;
    for (const auto& name : names) {
        MapData::SoundRecord sound{};
        sound.name = name; sound.filePath = "war3mapImported\\preserved.wav"; sound.eaxSetting = "DefaultEAXON";
        sound.flags = 7; sound.fadeInRate = 11; sound.fadeOutRate = 22; sound.volume = -1;
        sound.pitch = std::bit_cast<float>(MapData::kSoundDefaultRealBits); sound.pitchVariance = std::bit_cast<float>(0x80000000u);
        sound.priority = 8; sound.channel = 10; sound.minDistance = 100; sound.maxDistance = 2000; sound.distanceCutoff = 2500;
        sound.coneAngleInside = 90; sound.coneAngleOutside = 180; sound.coneOutsideVolume = 67; sound.coneOrientation = {0.5f, -0.25f, 0.75f};
        sounds.entries.push_back(std::move(sound));
    }
    const auto encoded = sounds.Serialize(); Require(encoded.Succeeded(), "原创完整 classic W3S v1 保存夹具");
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3map.w3s", *encoded.value},
        {"war3map.wts", Bytes("// keep\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\n")},
        {"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
        {"war3map.wtg", Bytes("original-wtg")}, {"war3map.wct", Bytes("original-wct")},
        {"war3mapImported/preserved.wav", Bytes("preserved-resource-identity")}, {"private/preserve.bin", Bytes("preserved")}};
}

void BuildSoundSaveFixture(FieldFixture& fixture, std::vector<MapArchiveMember> members) {
    fixture.source = fixture.directory.Path() / "sound-source.w3x";
    Require(WriteVerifiedArchive(fixture.source, members).Succeeded(), "写原创声音归档来源");
    const auto opened = MapArchiveSession::Open(fixture.source);
    Require(opened.Succeeded() && opened.value, "独立打开声音归档来源");
    fixture.sourceSha = opened.value->SourceSha256(); fixture.map.emplace(MapDocument::FromArchive(*opened.value));
}

std::vector<MapArchiveMember> ReferencedSettingsSaveMembers(std::string_view strings =
    "// keep\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\nSTRING 7\r\n{\r\n2–4\r\n}\r\n") {
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(settings.Succeeded(), "设置保存引用夹具来自真实经典 W3I");
    settings.value->author = "TRIGSTR_00042"; settings.value->recommendedPlayers = "TRIGSTR_7";
    const auto wire = settings.value->Serialize(); Require(wire.Succeeded(), "设置引用夹具完整原生编码");
    return {{"war3map.w3i", *wire.value}, {"war3map.wts", Bytes(strings)},
        {"war3map.wtg", Bytes("original-wtg")}, {"war3map.wct", Bytes("original-wct")},
        {"war3map.j", Bytes("original-script")}, {"unmanaged.bin", Bytes("opaque-preserved")}};
}

void TestSettingsReferencesUseTheExistingCandidateSaveGate() {
    using K = MapDataKind;
    FieldFixture fixture; BuildSoundSaveFixture(fixture, ReferencedSettingsSaveMembers());
    const std::vector<MapEditFieldOperation> references{Edit(fixture, K::Settings, 0, "author", "共享作者/描述"),
        Edit(fixture, K::Settings, 0, "description", "共享作者/描述"), Edit(fixture, K::Settings, 0, "recommended_players", "3–5")};
    Require(!HasFieldGateCode(SaveFields(fixture, references)),
        "直接保存三项设置引用通过原字段/成员/回读门禁，规范为两项 WTS 正文，并继续受脚本输入门禁限制");
    auto mixed = references; mixed.push_back(Edit(fixture, K::Settings, 0, "game_data_set", "2"));
    Require(!HasFieldGateCode(SaveFields(fixture, mixed)), "引用加数值规范为 W3I/WTS 两成员，原精确回读不把引用字节误当新正文");
    auto conflict = references; conflict[1].edit.value.bytes = "冲突描述";
    Require(HasCode(SaveFields(fixture, conflict), "MAP_FIELD_STRING_CONFLICT"), "直接保存同编号冲突在写归档前全批拒绝");
    conflict = references; conflict.push_back(Edit(fixture, K::Strings, 0, "value", "直接正文冲突"));
    Require(HasCode(SaveFields(fixture, conflict), "MAP_FIELD_STRING_CONFLICT"), "直接 WTS 操作与引用设置不能相互覆盖");
    auto stale = references; stale[1].expectedMemberSha256 = std::string(64, '0');
    Require(HasCode(SaveFields(fixture, stale), "SAVE_FIELD_REQUEST_INVALID"),
        "仅后续同成员 SHA 不同先由既有批次绑定门禁拒绝，不进入归档或引用规划");
    for (auto& operation : stale) operation.expectedMemberSha256 = std::string(64, '0');
    Require(HasCode(SaveFields(fixture, stale), "SAVE_FIELD_MEMBER_STALE"), "后续同成员引用操作也必须绑定原 W3I SHA");
    stale = references; stale[1].edit.expected.bytes = "TRIGSTR_7";
    Require(HasCode(SaveFields(fixture, stale), "MAP_FIELD_VALUE_STALE"), "直接保存继续绑定精确 W3I wire 引用");
    FieldFixture missing; auto withoutStrings = ReferencedSettingsSaveMembers(); withoutStrings.erase(withoutStrings.begin() + 1);
    BuildSoundSaveFixture(missing, withoutStrings);
    Require(HasCode(SaveFields(missing, {Edit(missing, K::Settings, 0, "description", "new")}), "MAP_FIELD_SOURCE_UNSAFE"), "直接保存不能为引用重建缺失 WTS");
    FieldFixture duplicate;
    BuildSoundSaveFixture(duplicate, ReferencedSettingsSaveMembers("STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\nSTRING 7\n{\n2–4\n}\n"));
    Require(HasCode(SaveFields(duplicate, {Edit(duplicate, K::Settings, 0, "description", "new")}), "WTS_REFERENCE_AMBIGUOUS"), "直接保存不能任选重复 WTS 编号的一条正文");
    const auto reopened = MapArchiveSession::Open(fixture.source);
    Require(reopened.Succeeded() && reopened.value->SourceSha256() == fixture.sourceSha, "所有直接保存门禁保持原归档 SHA");
    for (const auto& member : ReferencedSettingsSaveMembers()) {
        const auto bytes = reopened.value->ReadMember(member.name);
        Require(bytes.Succeeded() && *bytes.value == member.data, "引用保存拒绝未发布且完整保留所有来源成员");
    }
}

void TestSettingsReferencesSaveUndoAndRedoThroughTheOriginalWorker() {
    using K = MapDataKind;
    FieldFixture fixture; const auto members = ReferencedSettingsSaveMembers(); BuildSoundSaveFixture(fixture, members);
    const auto opened = MapArchiveSession::Open(fixture.source); Require(opened.Succeeded(), "冻结引用归档来源");
    const auto original = *fixture.map;
    MapData::MapDataEditSession edits(original);
    auto requested = *original.Settings(); requested.author = requested.description = "共享作者/描述"; requested.recommendedPlayers = "3–5";
    const auto applied = edits.EditSettings(requested);
    Require(applied.Succeeded() && HasCode(applied.diagnostics, "MAP_FIELD_SHARED_STRING") && edits.Revision() == 1
        && edits.Replacements().size() == 1 && edits.Replacements().front().name == "war3map.wts",
        "整页设置 API 与字段命令共用正文规则，三项引用在一个原命令中仅改 WTS");
    const auto receipt = edits.LastEdit();
    const auto current = MapData::ProjectMapData(original, edits); Require(current.Succeeded(), "引用计划投影保留原所有成员");
    Require(current.value->Member(K::Settings).sources.front().data == members.front().data, "整页引用编辑保留完整原 W3I 字节");
    const auto frozen = MapData::FreezeOriginalMapDataFields(original, *current.value);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 1 && frozen.value->Operations().size() == 2, "共享别名合并为两个稳定 WTS 保存操作");
    auto wire = std::string(reinterpret_cast<const char*>(members[1].data.data()), members[1].data.size());
    wire.replace(wire.find("Original description"), std::string("Original description").size(), "共享作者/描述");
    wire.replace(wire.find("2–4"), std::string("2–4").size(), "3–5");
    const auto expected = Bytes(wire);
    const auto verify = [&](const MapArchiveSession& archive, bool changed) {
        for (const auto& member : members) {
            const auto bytes = archive.ReadMember(member.name);
            Require(bytes.Succeeded() && *bytes.value == (changed && member.name == "war3map.wts" ? expected : member.data),
                "独立归档回读仅两段 WTS 正文改变，完整 W3I/引用前导零、脚本、触发器和未知成员不变");
        }
        const auto map = MapDocument::FromArchive(archive);
        Require(map.Settings()->author == "TRIGSTR_00042" && map.Settings()->description == "TRIGSTR_0042"
            && map.Settings()->recommendedPlayers == "TRIGSTR_7"
            && map.ResolveString(map.Settings()->description).value == std::optional<std::string>(changed ? "共享作者/描述" : "Original description")
            && map.ResolveString(map.Settings()->recommendedPlayers).value == std::optional<std::string>(changed ? "3–5" : "2–4"), "实际保存后原引用与解析正文一致");
    };
    MapSaveTask save; Require(save.FreezeArchive(*opened.value).Succeeded(), "原 worker 冻结引用来源");
    save.target = fixture.directory.Path() / "reference-candidate.w3x"; save.replacements = edits.Replacements(); MapSaveService::Run(save);
    Require(save.succeeded && save.nextSession, "原 worker 写出引用候选并回读，未宣称原版或游戏通过");
    const auto independent = MapArchiveSession::Open(save.target); Require(independent.Succeeded(), "另一实例独立重开引用候选"); verify(*independent.value, true);
    edits.CommitSaved(MapDocument::FromArchive(*save.nextSession));
    Require(!edits.IsDirty() && edits.Undo(), "引用保存后原 Undo 历史保持");
    MapSaveTask undo; Require(undo.FreezeArchive(*save.nextSession).Succeeded(), "Undo 冻结最新候选");
    undo.target = fixture.directory.Path() / "reference-undo.w3x"; undo.replacements = edits.Replacements(); MapSaveService::Run(undo);
    Require(undo.succeeded && undo.nextSession, "原 Undo 仍经同一保存 worker 写出");
    const auto undone = MapArchiveSession::Open(undo.target); Require(undone.Succeeded(), "独立重开引用 Undo 候选"); verify(*undone.value, false);
    edits.CommitSaved(MapDocument::FromArchive(*undo.nextSession));
    Require(!edits.IsDirty() && edits.Redo() && edits.LastEdit().CommandIdentity() == receipt.CommandIdentity(), "原 Redo 保留相同引用命令身份");
    MapSaveTask redo; Require(redo.FreezeArchive(*undo.nextSession).Succeeded(), "Redo 冻结最近归档");
    redo.target = fixture.directory.Path() / "reference-redo.w3x"; redo.replacements = edits.Replacements(); MapSaveService::Run(redo);
    Require(redo.succeeded && redo.nextSession, "原 Redo 仍经同一 worker 写出");
    const auto redone = MapArchiveSession::Open(redo.target); Require(redone.Succeeded(), "独立重开引用 Redo 候选"); verify(*redone.value, true);
    Require(Sha256File(fixture.source).value == std::optional<std::string>(fixture.sourceSha), "全部引用候选与 Undo/Redo 保存保持源图 SHA");
}

void TestSoundFieldsKeepOriginalCandidateSaveGates() {
    using K = MapDataKind;
    FieldFixture fixture; BuildSoundSaveFixture(fixture, SoundSaveMembers());
    const std::vector<MapEditFieldOperation> valid{Edit(fixture, K::Sounds, 0, "volume", "0"),
        Edit(fixture, K::Sounds, 0, "fade_in", "2147483647"), Edit(fixture, K::Sounds, 0, "fade_out", "-1")};
    Require(!HasFieldGateCode(SaveFields(fixture, valid)),
        "三个声音整数通过原 SaveMapEditCandidate 源身份/精确字段回读，并继续受缺脚本输入门禁限制");
    auto stale = valid[0]; stale.expectedMemberSha256 = std::string(64, '0');
    Require(HasCode(SaveFields(fixture, {stale}), "SAVE_FIELD_MEMBER_STALE"), "声音候选保存继续绑定源 W3S 完整 SHA");
    stale = valid[0]; stale.edit.expected.integer = 0;
    Require(HasCode(SaveFields(fixture, {stale}), "MAP_FIELD_VALUE_STALE"), "声音候选保存核 exact 原 -1，不能猜默认零");
    auto late = valid[2]; late.edit.value.integer = -2;
    Require(HasCode(SaveFields(fixture, {valid[0], valid[1], late}), "MAP_FIELD_VALUE_INVALID"),
        "原保存入口的合法声音前缀与非法末项整批拒绝，不能写部分候选");
    const auto reopened = MapArchiveSession::Open(fixture.source);
    Require(reopened.Succeeded() && reopened.value->SourceSha256() == fixture.sourceSha, "声音保存拒绝不改变原归档 SHA");
    for (const auto& member : SoundSaveMembers()) {
        const auto unchanged = reopened.value->ReadMember(member.name);
        Require(unchanged.Succeeded() && *unchanged.value == member.data, "声音保存门禁保留所有原归档成员字节");
    }
    for (const auto& names : {std::vector<std::string>{"Ambient Bell", "Ambient_Bell"},
        std::vector<std::string>{"Ambient Bell", "gg_snd_Ambient_Bell"}}) {
        FieldFixture ambiguous; BuildSoundSaveFixture(ambiguous, SoundSaveMembers(names));
        Require(HasCode(SaveFields(ambiguous, {Edit(ambiguous, K::Sounds, 0, "volume", "80")}), "MAP_FIELD_SOUND_NAME_AMBIGUOUS"),
            "独立 CLI 保存同样拒绝实际 gg_snd 脚本符号歧义，不能绕过 prepared 资格");
    }
}

void TestSoundsUseOriginalSaveWorkerAndUndoReplay() {
    using K = MapDataKind;
    FieldFixture fixture;
    const auto members = SoundSaveMembers(); BuildSoundSaveFixture(fixture, members);
    auto opened = MapArchiveSession::Open(fixture.source); Require(opened.Succeeded(), "冻结同一声音归档来源");
    const auto original = *fixture.map;
    std::vector<MapData::MapFieldEdit> changes;
    for (const auto& edit : {Edit(fixture, K::Sounds, 0, "volume", "80"), Edit(fixture, K::Sounds, 0, "fade_in", "-1"),
        Edit(fixture, K::Sounds, 0, "fade_out", "2147483647")}) changes.push_back(edit.edit);
    const auto memberSha = Sha256(original.Member(K::Sounds).sources.front().data);
    const auto prepared = MapData::PrepareMapDataFields(original, original, *memberSha.value, changes);
    MapData::MapDataEditSession edits(original);
    Require(prepared.Succeeded() && edits.ApplyPreparedFields(*prepared.value).Succeeded(), "声音三个字段沿一个原密封命令发布");
    const auto receipt = edits.LastEdit();
    const auto projected = MapData::ProjectMapData(original, edits); Require(projected.Succeeded(), "当前声音事务完整投影");
    const auto frozen = MapData::FreezeOriginalMapDataFields(original, *projected.value);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 1 && frozen.value->Operations().size() == 3,
        "原保存输入由同一 W3S 三个 exact 字段差异冻结");
    auto expected = original.Member(K::Sounds).sources.front().data;
    const std::size_t flagsOffset = 8 + std::string_view("Raw Sound").size() + 1
        + std::string_view("war3mapImported\\preserved.wav").size() + 1 + std::string_view("DefaultEAXON").size() + 1;
    const auto patch = [&](std::size_t offset, std::int32_t value) {
        Require(offset + 4 <= expected.size(), "独立声音定长窗口在完整成员内");
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned index = 0; index < 4; ++index) expected[offset + index] = static_cast<std::byte>((bits >> (index * 8)) & 0xff);
    };
    patch(flagsOffset + 4, -1); patch(flagsOffset + 8, std::numeric_limits<std::int32_t>::max()); patch(flagsOffset + 12, 80);
    Require(projected.value->Member(K::Sounds).sources.front().data == expected,
        "声音密封候选逐字节等于独立三个整数窗口 oracle，保留另一行与其它数值位型");
    std::vector<MapData::MapFieldEdit> originalEdits;
    for (const auto& operation : frozen.value->Operations()) {
        Require(operation.expectedMemberSha256 == *memberSha.value, "原保存冻结绑定最初 W3S SHA");
        originalEdits.push_back(operation.edit);
    }
    const auto tables = MapData::ApplyMapFieldEdits(original, originalEdits);
    MapData::MapDataEditSession replay(original);
    Require(tables.Succeeded() && tables.value->sounds && replay.EditSounds(*tables.value->sounds).Succeeded()
        && replay.Replacements().size() == 1 && replay.Replacements().front().data == expected,
        "冻结字段回放经现 EditSounds 保存提交得到相同完整 W3S");
    const auto checkMembers = [&](const MapArchiveSession& archive, bool changed) {
        for (const auto& member : members) {
            const auto read = archive.ReadMember(member.name);
            Require(read.Succeeded() && *read.value == (changed && member.name == "war3map.w3s" ? expected : member.data),
                "独立归档重开逐成员证明仅 W3S 三个窗口改变，脚本、触发器、W3I/WTS、导入与未知成员保留");
        }
        const auto map = MapDocument::FromArchive(archive);
        Require(map.Sounds() && map.Sounds()->entries.size() == 2
            && map.Sounds()->entries[0].volume == (changed ? 80 : -1)
            && map.Sounds()->entries[0].fadeInRate == (changed ? -1 : 11)
            && map.Sounds()->entries[0].fadeOutRate == (changed ? std::numeric_limits<std::int32_t>::max() : 22)
            && std::bit_cast<std::uint32_t>(map.Sounds()->entries[0].pitch) == MapData::kSoundDefaultRealBits
            && std::bit_cast<std::uint32_t>(map.Sounds()->entries[0].pitchVariance) == 0x80000000u,
            "真实保存回读 exact 整数、默认 real sentinel 和负零");
    };
    MapSaveTask save;
    Require(save.FreezeArchive(*opened.value).Succeeded(), "原纯数据保存 worker 冻结声音来源");
    save.target = fixture.directory.Path() / "sound-candidate.w3x"; save.replacements = replay.Replacements();
    MapSaveService::Run(save);
    Require(save.succeeded && save.nextSession, "原纯数据 worker 写出声音候选并重开，未宣称 WE 或游戏验收");
    const auto independent = MapArchiveSession::Open(save.target); Require(independent.Succeeded(), "另一次独立打开声音候选");
    checkMembers(*independent.value, true);
    const auto saved = MapDocument::FromArchive(*save.nextSession); edits.CommitSaved(saved);
    Require(!edits.IsDirty() && edits.Undo() && edits.IsDirty(), "真实保存后原声音 Undo 历史仍可用");
    MapSaveTask undo;
    Require(undo.FreezeArchive(*save.nextSession).Succeeded(), "声音 Undo 保存冻结最近归档基线");
    undo.target = fixture.directory.Path() / "sound-undo.w3x"; undo.replacements = edits.Replacements(); MapSaveService::Run(undo);
    Require(undo.succeeded && undo.nextSession, "原声音 Undo 经相同保存 worker 写出");
    const auto undoIndependent = MapArchiveSession::Open(undo.target); Require(undoIndependent.Succeeded(), "独立打开声音 Undo 归档");
    checkMembers(*undoIndependent.value, false);
    const auto restored = MapDocument::FromArchive(*undo.nextSession); edits.CommitSaved(restored);
    Require(!edits.IsDirty() && edits.Redo() && edits.LastEdit().CommandIdentity() == receipt.CommandIdentity(),
        "Undo 保存后原声音 Redo 仍保持同一个命令身份");
    MapSaveTask redo;
    Require(redo.FreezeArchive(*undo.nextSession).Succeeded(), "声音 Redo 保存冻结最近归档基线");
    redo.target = fixture.directory.Path() / "sound-redo.w3x"; redo.replacements = edits.Replacements(); MapSaveService::Run(redo);
    Require(redo.succeeded && redo.nextSession, "原声音 Redo 经相同保存 worker 写出");
    const auto redoIndependent = MapArchiveSession::Open(redo.target); Require(redoIndependent.Succeeded(), "独立打开声音 Redo 归档");
    checkMembers(*redoIndependent.value, true);
    Require(Sha256File(fixture.source).value == std::optional<std::string>(fixture.sourceSha), "声音所有候选与 Undo/Redo 保存保持源图 SHA");
}

void TestUnitCoordinatesUseTheExistingFieldSaveGate() {
    FieldFixture fixture; BuildFieldFixture(fixture);
    const std::vector<MapEditFieldOperation> xyz{Edit(fixture, MapDataKind::Units, 0, "x", "32"),
        Edit(fixture, MapDataKind::Units, 0, "y", "-48"), Edit(fixture, MapDataKind::Units, 0, "z", "8")};
    Require(!HasFieldGateCode(SaveFields(fixture, xyz)), "XYZ passes the existing fieldEdits gates and reaches the missing-script-input boundary");
    auto invalid = xyz; invalid.back().edit.value.floatBits = 0x7f800000u;
    Require(HasCode(SaveFields(fixture, invalid), "MAP_FIELD_VALUE_INVALID"), "late invalid native XYZ is refused before archive publication");
    auto reopened = MapArchiveSession::Open(fixture.source);
    Require(reopened.Succeeded() && reopened.value->SourceSha256() == fixture.sourceSha, "field save gate checks retain original source archive");
}

void TestPlacementSaveBindsSourceAndSharesWriter() {
    FieldFixture fixture;
    BuildFieldFixture(fixture);
    const auto key = MapData::MapFieldRowKey(*fixture.map, MapDataKind::Units, 0);
    const auto sha = Sha256(fixture.map->Member(MapDataKind::Units).sources.front().data);
    Require(key.Succeeded() && key.value && sha.Succeeded() && sha.value, "read placement identity");
    const MapEditPlacementSource source{MapData::PlacementFile::Units, 0, *key.value, *sha.value};
    const auto save = [&fixture](MapEditPlacementOperation operation) {
        MapEditSaveRequest request;
        request.sourceMap = fixture.source;
        request.targetCandidate = fixture.directory.Path() / "placement-candidate.w3x";
        request.toolRoot = request.gameRoot = fixture.directory.Path();
        request.expectedSourceSha256 = fixture.sourceSha;
        request.placementEdit = std::move(operation);
        auto result = SaveMapEditCandidate(request);
        Require(!result.Succeeded() && !std::filesystem::exists(request.targetCandidate),
            "fixture without script inputs never publishes a placement candidate");
        return result.diagnostics;
    };
    const auto valid = save(MapEditPlacementCopy{{source}, {128, 0, 0}});
    Require(!HasCode(valid, "SAVE_PLACEMENT_REQUEST_INVALID")
        && !HasCode(valid, "SAVE_PLACEMENT_ROW_STALE")
        && !HasCode(valid, "SAVE_PLACEMENT_EDIT_SCOPE_INVALID"),
        "bound placement copy reaches shared script preparation");
    auto stale = source;
    stale.expectedRowKey += ":old";
    Require(HasCode(save(MapEditPlacementCopy{{stale}, {128, 0, 0}}), "SAVE_PLACEMENT_ROW_STALE"),
        "stale row identity blocks copy before saving");
    stale = source;
    stale.expectedMemberSha256 = std::string(64, '0');
    Require(HasCode(save(MapEditPlacementRotation{stale, 1.0f}), "SAVE_PLACEMENT_MEMBER_STALE"),
        "stale member blocks rotation before saving");
    const auto rotated = save(MapEditPlacementRotation{source, 1.0f});
    Require(!HasCode(rotated, "SAVE_PLACEMENT_EDIT_SCOPE_INVALID"),
        "bound rotation reaches shared script preparation");
    auto unit = fixture.map->Units()->units.front(); unit.rawcode = "H001"; unit.heroLevel = 8;
    const auto created = save(MapEditPlacementCreate{*sha.value, std::vector<MapData::UnitPlacement>{unit}});
    Require(!HasCode(created, "SAVE_PLACEMENT_REQUEST_INVALID") && !HasCode(created, "SAVE_PLACEMENT_EDIT_SCOPE_INVALID")
        && !HasCode(created, "SAVE_PLACEMENT_MEMBER_STALE"), "typed creation reaches unchanged shared script gate");
    Require(HasCode(save(MapEditPlacementCreate{std::string(64, '0'), std::vector<MapData::UnitPlacement>{unit}}),
        "SAVE_PLACEMENT_MEMBER_STALE"), "new records cannot bypass original member binding");
    const auto removed = save(MapEditPlacementDelete{{source}});
    Require(!HasCode(removed, "SAVE_PLACEMENT_REQUEST_INVALID") && !HasCode(removed, "SAVE_PLACEMENT_EDIT_SCOPE_INVALID"),
        "deleting final classic unit reaches unchanged shared script gate");
    Require(HasCode(save(MapEditPlacementDelete{{source, source}}), "PLACEMENT_DUPLICATE"), "duplicate save delete refused");
    Require(HasCode(save(MapEditPlacementDelete{{stale}}), "SAVE_PLACEMENT_MEMBER_STALE"), "delete save checks original sha");
}

void TestCreateDeleteUseOriginalSaveWorkerAndSavedUndo() {
    for (const bool creating : {false, true}) {
        FieldFixture fixture; BuildFieldFixture(fixture);
        auto opened = MapArchiveSession::Open(fixture.source); Require(opened.Succeeded(), "open create/delete archive fixture");
        const auto& original = *fixture.map;
        const auto sha = Sha256(original.Member(MapDataKind::Units).sources.front().data);
        const auto key = MapData::MapFieldRowKey(original, MapDataKind::Units, 0);
        Require(sha.Succeeded() && key.Succeeded(), "bind native create/delete fixture");
        MapData::MapDataPlacementIntent intent = MapData::MapDataPlacementDelete{{{
            MapData::MakePlacementId(MapData::PlacementFile::Units, 0, original.Units()->units[0]), *key.value, *sha.value}}};
        if (creating) {
            auto record = original.Units()->units.front(); record.rawcode = "H001"; record.heroLevel = 5;
            intent = MapData::MapDataPlacementCreate{*sha.value, std::vector<MapData::UnitPlacement>{record}};
        }
        const auto plan = MapData::PrepareMapDataPlacement(original, original, intent);
        Require(plan.Succeeded(), "prepare native create/delete save");
        MapData::MapDataEditSession session(original);
        Require(session.ApplyPreparedPlacement(*plan.value).Succeeded(), "apply native create/delete save");
        const auto persist = [&](const MapArchiveSession& input, const char* name, const MapDocument& expected) {
            MapSaveTask task; Require(task.FreezeArchive(input).Succeeded(), "freeze original save worker archive");
            task.target = fixture.directory.Path() / (std::string(name) + ".w3x"); task.replacements = session.Replacements();
            MapSaveService::Run(task); Require(task.succeeded && task.nextSession, "create/delete original save worker failed");
            auto reopened = MapArchiveSession::Open(task.target); Require(reopened.Succeeded(), "independent reopen create/delete candidate");
            const auto read = MapDocument::FromArchive(*reopened.value);
            for (const auto& member : expected.Members()) for (const auto& source : member.sources) {
                const auto observed = reopened.value->ReadMember(member.memberName);
                Require(observed.Succeeded() && *observed.value == source.data, "create/delete saved member bytes differ");
            }
            Require(read.Units() && read.Units()->units.size() == expected.Units()->units.size(), "saved placement count wrong");
            session.CommitSaved(read); return std::move(*reopened.value);
        };
        auto saved = persist(*opened.value, "placement-saved", plan.value->After());
        Require(!session.IsDirty() && session.Undo(), "saved create/delete lost original undo history");
        auto undone = persist(saved, "placement-undone", original);
        Require(!session.IsDirty() && session.Redo(), "saved undo lost create/delete redo history");
        auto redone = persist(undone, "placement-redone", plan.value->After());
        Require(redone.VerifySourceUnchanged().Succeeded() && Sha256File(fixture.source).value == fixture.sourceSha,
            "native create/delete save touched source archive");
    }
}

void TestDoodadsUseOriginalSaveWorkerAndSavedUndoRedo() {
    // 原创 wire 直接写字段；expected 只补指定窗口/clone 原记录，不调用产品 serializer 算期望。
    MapDataFixtures::Fixture wire;
    wire.Raw("W3do"); wire.Int(8); wire.Int(11); wire.Int(2);
    const auto first = wire.bytes.size();
    wire.Raw("A001"); wire.Int(2);
    wire.Float(96); wire.Float(-64); wire.Float(-0.0f); wire.Float(0.25f);
    wire.Float(1.25f); wire.Float(0.75f); wire.Float(2.25f); wire.Byte(2); wire.Byte(77);
    wire.Int(17); wire.Int(2); wire.Int(2);
    wire.Raw("I001"); wire.Int(65); wire.Raw("I002"); wire.Int(35); wire.Int(0); wire.Int(41);
    const auto firstEnd = wire.bytes.size();
    wire.Raw("A002"); wire.Int(1);
    wire.Float(-32); wire.Float(48); wire.Float(12); wire.Float(0);
    wire.Float(1); wire.Float(1); wire.Float(1); wire.Byte(0); wire.Byte(100);
    wire.Int(-1); wire.Int(0); wire.Int(55);
    const auto tail = wire.bytes.size();
    wire.Int(0); wire.Int(1); wire.Raw("A003"); wire.Int(3); wire.Int(-5); wire.Int(8);
    const auto patch = [](std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t bits) {
        Require(offset + 4 <= bytes.size(), "原创 DOO oracle 窗口必须在完整成员内");
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((bits >> (i * 8)) & 0xff);
    };
    TempDirectory directory;
    const auto source = directory.Path() / "doodad-source.w3x";
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", wire.bytes},
        {"war3map.wtg", Bytes("opaque-original-wtg")}, {"war3map.wct", Bytes("opaque-original-wct")},
        {"war3map.w3e", Bytes("opaque-original-terrain")}, {"war3map.wpm", Bytes("opaque-original-pathing")},
        {"unmanaged.bin", {std::byte{0}, std::byte{0xff}, std::byte{0x31}}}};
    Require(WriteVerifiedArchive(source, members).Succeeded(), "原创经典 DOO 来源必须真实写入 MPQ");
    const auto opened = MapArchiveSession::Open(source); Require(opened.Succeeded(), "独立打开原创 DOO 来源");
    const auto original = MapDocument::FromArchive(*opened.value);
    Require(original.Doodads() && original.Doodads()->doodads.size() == 2
        && original.Doodads()->specialDoodads.size() == 1, "原创 DOO 普通记录与特殊尾必须完整解析");
    const auto sourceSha = opened.value->SourceSha256();
    for (int operation = 0; operation < 3; ++operation) {
        MapData::MapDataEditSession edits(original);
        const auto id = MapData::MakePlacementId(MapData::PlacementFile::Doodads, 0, original.Doodads()->doodads[0]);
        auto expected = wire.bytes;
        if (operation == 2) {
            Require(MapData::CopyPlacements(edits, std::span(&id, 1), {32, -16, 4}).Succeeded(), "普通地物复制提交原事务");
            std::vector<std::byte> clone(wire.bytes.begin() + first, wire.bytes.begin() + firstEnd);
            patch(clone, 8, std::bit_cast<std::uint32_t>(128.0f));
            patch(clone, 12, std::bit_cast<std::uint32_t>(-80.0f));
            patch(clone, 16, std::bit_cast<std::uint32_t>(4.0f)); patch(clone, clone.size() - 4, 56);
            patch(expected, 12, 3); expected.insert(expected.begin() + tail, clone.begin(), clone.end());
        } else {
            const MapData::PlacementMove move{id, operation == 0 ? std::array<float, 3>{128, -48, 12}
                : original.Doodads()->doodads[0].position, operation == 1 ? std::optional<float>(1.5f) : std::nullopt};
            Require(MapData::MovePlacements(edits, std::span(&move, 1)).Succeeded(), "普通地物移动/旋转提交原事务");
            if (operation == 0) for (unsigned axis = 0; axis < 3; ++axis)
                patch(expected, first + 8 + axis * 4, std::bit_cast<std::uint32_t>(move.position[axis]));
            else patch(expected, first + 20, std::bit_cast<std::uint32_t>(1.5f));
        }
        const auto receipt = edits.LastEdit();
        Require(edits.Revision() == 1 && receipt.HasPlacementLineage() && edits.Replacements().size() == 1
            && edits.Replacements()[0].data == expected, "原事务完整 DOO 必须等于独立字节 oracle");
        const auto persist = [&](const MapArchiveSession& baseline, const char* stage, const std::vector<std::byte>& target) {
            MapSaveTask task; Require(task.FreezeArchive(baseline).Succeeded(), "冻结真实 MPQ 保存基线");
            task.target = directory.Path() / (std::to_string(operation) + "-" + stage + ".w3x");
            task.replacements = edits.Replacements(); MapSaveService::Run(task);
            Require(task.succeeded && task.nextSession && !task.scriptRebuilt, "原纯数据 worker 成功保存；本层不宣称 JASS/原版/game通过");
            auto independent = MapArchiveSession::Open(task.target); Require(independent.Succeeded(), "另一次磁盘 MPQ 独立打开");
            Require(independent.value->Members().size() == members.size(), "保存不能增删原成员");
            for (const auto& member : members) {
                const auto bytes = independent.value->ReadMember(member.name);
                Require(bytes.Succeeded() && *bytes.value == (member.name == "war3map.doo" ? target : member.data),
                    "独立回读完整 DOO 和所有未编辑成员必须逐字节匹配");
            }
            const auto saved = MapDocument::FromArchive(*independent.value);
            Require(saved.Doodads() && saved.Doodads()->specialDoodads.size() == 1
                && saved.Doodads()->doodads[0].itemSets.size() == 2
                && saved.Doodads()->doodads[0].itemSets[0].items.size() == 2,
                "嵌套掉落与特殊尾不能因保存丢失");
            edits.CommitSaved(saved);
            return std::move(*independent.value);
        };
        auto saved = persist(*opened.value, "saved", expected);
        Require(!edits.IsDirty() && edits.Undo() && edits.IsDirty(), "CommitSaved后同一地物命令仍可Undo");
        auto undone = persist(saved, "undo", wire.bytes);
        Require(!edits.IsDirty() && edits.Redo() && edits.LastEdit().CommandIdentity() == receipt.CommandIdentity(),
            "Undo重存后Redo仍使用原命令身份");
        auto redone = persist(undone, "redo", expected);
        Require(!edits.IsDirty() && redone.VerifySourceUnchanged().Succeeded()
            && Sha256File(source).value == std::optional<std::string>(sourceSha), "三次保存保持来源与历史身份");
    }
}

} // namespace

void TestMapCreationRejectsUnboundedOrExistingInputs() {
    MapCreateOptions options;
    Require(ValidateMapCreateOptions(options).Succeeded(), "known profile and default name");
    options.profile = "classic-lordaeron-128-flat-v1";
    Require(!ValidateMapCreateOptions(options).Succeeded(), "unproven dimension is not inferred");
    options.profile = std::string(kClassicFlatMapProfile);
    for (const auto& bad : {std::string(), std::string(129, 'x'), std::string("a\0b", 3),
            std::string("a\nb"), std::string("\xc0\xaf", 2)}) {
        options.name = bad;
        Require(!ValidateMapCreateOptions(options).Succeeded(), "invalid metadata is refused");
    }
    options.name = std::string(128, 'x');
    Require(ValidateMapCreateOptions(options).Succeeded(), "inclusive text byte limit");
    options.author = std::string(129, 'x');
    Require(!ValidateMapCreateOptions(options).Succeeded(), "author shares text budget");
    TempDirectory scratch;
    MapCreateRequest request;
    request.target = scratch.Path() / "new.w3x";
    request.workspace = scratch.Path() / "work";
    const auto missing = CreateMapCandidate(request);
    Require(HasCode(missing.diagnostics, "CREATE_PATH_INVALID"), "missing tools block before IO");
    Require(!std::filesystem::exists(request.target) && !std::filesystem::exists(request.workspace),
        "invalid request creates no candidate or workspace");
    request.target = scratch.Path();
    request.toolRoot = scratch.Path(); request.gameRoot = scratch.Path();
    Require(HasCode(CreateMapCandidate(request).diagnostics, "CREATE_PATH_INVALID"),
        "existing target is rejected before loading a catalog");
}

int main() {
    try {
        TestFrozenSourceAndVerifiedCandidate();
        TestClassicV1CommonMapSaveAndTemplateBoundary();
        TestOptionalRealClassicV1MapSave();
        TestIncompleteScriptInputDoesNotWriteCandidate();
        TestIndependentUiPreparationGate();
        TestSharedPerFileEncodingDeclarations();
        TestPublicTriggerTextQueryKeepsSourceIdentity();
        TestPreflightConsumesFrozenTaskWithoutPublishing();
        TestPreflightUsesSharedGuardsAndChecksLateSourceChanges();
        TestVersionedFormatCannotBypassScriptGate();
        TestThrowingProgressCallbackDoesNotChangeSaveResult();
        TestProgramExceptionsFailClosedWithoutCollectingPrivateWhat();
        TestBlightSaveRejectsStaleOrMissingTerrainPair();
        TestGenericTerrainSaveRejectsUnverifiedAmplitude();
        TestFieldEditsBindSourceAndStayInScope();
        TestFieldValueTextIsExact();
        TestDFieldsUseOriginalSaveWorkerAndUndoReplay();
        TestSettingsReferencesUseTheExistingCandidateSaveGate();
        TestSettingsReferencesSaveUndoAndRedoThroughTheOriginalWorker();
        TestSoundFieldsKeepOriginalCandidateSaveGates();
        TestSoundsUseOriginalSaveWorkerAndUndoReplay();
        TestUnitCoordinatesUseTheExistingFieldSaveGate();
        TestPlacementSaveBindsSourceAndSharesWriter();
        TestCreateDeleteUseOriginalSaveWorkerAndSavedUndo();
        TestDoodadsUseOriginalSaveWorkerAndSavedUndoRedo();
        TestMapCreationRejectsUnboundedOrExistingInputs();
        std::cout << "Map save service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
