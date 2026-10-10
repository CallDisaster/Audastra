#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Script;
using namespace War3Map::ObjectData;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
std::vector<std::byte> UnitBytes() {
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Require(native.Succeeded() && native.value->AddCustomObject("H001", "hfoo").Succeeded(), "new unit");
    Require(native.value->AddModification({ObjectType::Unit, "H001"}, {"unam"}, NativeValueType::String,
        std::string("generated unit")).Succeeded(), "generated name");
    return *NativeObjectCodec(ObjectType::Unit).Serialize(*native.value).value;
}

using EditableViews = std::array<const LniObjectDocument*, 7>;

std::unique_ptr<LniObjectDocument> ParseLni(ObjectType type, std::string_view text) {
    auto parsed = LniObjectCodec(type).Parse(Bytes(text));
    Require(parsed.Succeeded() && parsed.document, "original LNI fixture must parse");
    return std::unique_ptr<LniObjectDocument>(static_cast<LniObjectDocument*>(parsed.document.release()));
}

ScriptSaveBundle BundleFor(ObjectType type, const std::vector<std::byte>& native) {
    PipelineResult pipeline;
    pipeline.script = "function main takes nothing returns nothing\nendfunction\n";
    pipeline.syntaxChecked = true; pipeline.checkedBy = "synthetic preflight fixture";
    pipeline.objectMembers = {{std::string(NativeFileName(type)), native}};
    auto prepared = PrepareScriptSaveBundle(pipeline, "war3map.j");
    Require(prepared.Succeeded(), "fixture save bundle failed");
    return std::move(*prepared.value);
}

struct BundleState {
    std::vector<std::pair<std::string, std::vector<std::byte>>> members;
    std::array<std::vector<std::byte>, 7> native;
    std::array<std::string, 7> editable;
    bool operator==(const BundleState&) const = default;
};

BundleState Capture(const ScriptSaveBundle& bundle) {
    BundleState state;
    for (const auto& member : bundle.members) state.members.emplace_back(member.name, member.data);
    for (std::size_t i = 0; i < 7; ++i) {
        if (bundle.nativeBaselines[i]) {
            const auto bytes = NativeObjectCodec(static_cast<ObjectType>(i)).Serialize(*bundle.nativeBaselines[i]);
            Require(bytes.Succeeded(), "capture native baseline");
            state.native[i] = *bytes.value;
        }
        if (bundle.editableBaselines[i]) state.editable[i] = bundle.editableBaselines[i]->DocumentFingerprint();
    }
    return state;
}

std::array<std::string, 7> Fingerprints(const EditableViews& views) {
    std::array<std::string, 7> result;
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (views[i]) result[i] = views[i]->DocumentFingerprint();
    }
    return result;
}

void RejectedWithoutMutation(const ScriptSaveBundle& bundle, const EditableViews& views, std::string_view code) {
    const auto bundleBefore = Capture(bundle);
    const auto sourceBefore = Fingerprints(views);
    const auto result = PreflightScriptSaveBaselineReplacement(bundle, views);
    Require(!result.Succeeded(), "lossy editable replacement must fail before member merge");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code && diagnostic.severity == DiagnosticSeverity::Error;
    }), "expected explicit preflight diagnostic missing");
    Require(Capture(bundle) == bundleBefore && Fingerprints(views) == sourceBefore,
        "preflight rejection mutated source or consumed/changed bundle");
}

StandardObjectCatalog ParentCatalog() {
    CatalogLoadOptions options;
    options.stringFiles.clear();
    for (unsigned i = 0; i < 7; ++i) {
        options.sources.push_back({static_cast<ObjectType>(i), {"base.slk"}, {}, {"name"}, {"art"}});
    }
    auto loaded = StandardObjectCatalog::Load([](const std::string&) {
        return Result<std::vector<std::byte>>::Success(Bytes(
            "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
            "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Original test base\"\nE\n"));
    }, options);
    Require(loaded.Succeeded(), "synthetic standard parent catalog");
    return std::move(*loaded.value);
}

void TestNestedParentsAreProtected() {
    const auto catalog = ParentCatalog();
    // 复用既有继承测试的原创三层形状；证明危险输出已被真实写出视图扁平化。
    constexpr auto text = "[P001]\n_parent = \"hfoo\"\nI001 = 10\n\n"
        "[C001]\n_parent = \"P001\"\nS001 = \"Child\"\n\n"
        "[D001]\n_parent = \"C001\"\nI001 = 30\n\n";
    for (unsigned i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectType>(i);
        const auto current = ParseLni(type, text);
        auto native = NativeObjectDocument::CreateEmpty(type);
        Require(native.Succeeded() && ApplyLniChangesToNative(*native.value, *current, nullptr, &catalog).Succeeded(),
            "native projection of nested chain");
        Require(native.value->AddCustomObject("N001", "hfoo").Succeeded(), "template only adds unrelated object");
        const auto bytes = NativeObjectCodec(type).Serialize(*native.value);
        Require(bytes.Succeeded(), "encode generated full class");
        const auto bundle = BundleFor(type, *bytes.value);
        Require(current->FindObject({type, "C001"})->parentRawcode == "P001" &&
            bundle.editableBaselines[i]->FindObject({type, "C001"})->parentRawcode == "hfoo",
            "fixture must expose the previously silent parent flattening");
        EditableViews views{}; views[i] = current.get();
        RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_PARENT_CHAIN");
    }
    auto self = ParseLni(ObjectType::Unit, "[H001]\n_parent = \"H001\"\n\n");
    EditableViews views{}; views[0] = self.get();
    RejectedWithoutMutation(BundleFor(ObjectType::Unit, UnitBytes()), views, "SCRIPT_BUNDLE_PARENT_CHAIN");
}

void TestTextAndUnknownValuesAreProtected() {
    const auto bundle = BundleFor(ObjectType::Unit, UnitBytes());
    const std::array textCases{
        "; editor annotation: retain this author note\n[H001]\n_parent = \"hfoo\"\nunam = \"old\"\n\n",
        "[H001] -- section note\n_parent = \"hfoo\"\nunam = \"old\"\n\n",
        "[H001]\n_parent = \"hfoo\"\nunam = \"old\" -- per-field note\n\n",
        "[H001]\n_parent=\"hfoo\"\nunam=\"old\"\n\n",
        "[H001]\r\n_parent = \"hfoo\"\r\nunam = \"old\"\r\n\r\n",
        "# comment-only empty category\n"};
    for (const auto* text : textCases) {
        const auto current = ParseLni(ObjectType::Unit, text);
        EditableViews views{}; views[0] = current.get();
        RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_TEXT_BASELINE");
    }
    // 原文已是规范排版，仍不可把文本 metadata 冒充四字节 native 字段。
    const auto metadata = ParseLni(ObjectType::Unit,
        "[H001]\n_parent = \"hfoo\"\n_editor_note = \"author metadata\"\n\n");
    EditableViews views{}; views[0] = metadata.get();
    RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_BASELINE_UNREPRESENTABLE");
    for (const auto* text : {
        "[H001]\n_parent = \"hfoo\"\nunam = {\n\"opaque table\",\n}\n\n",
        "[H001]\n_parent = \"hfoo\"\nufoo = true\n\n"}) {
        const auto current = ParseLni(ObjectType::Unit, text); views[0] = current.get();
        RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_OPAQUE_BASELINE");
    }
    const auto badIndex = ParseLni(ObjectType::Unit,
        "[H001]\n_parent = \"hfoo\"\nunam[2] = \"invalid unit level\"\n\n");
    views[0] = badIndex.get();
    RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_BASELINE_UNREPRESENTABLE");
}

void TestCanonicalDomainAndCategoryIsolation() {
    auto currentBytes = ConvertNativeObjectDataToLni(ObjectType::Unit, UnitBytes());
    Require(currentBytes.Succeeded(), "canonical source conversion");
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*currentBytes.value);
    Require(parsed.Succeeded(), "canonical source parse");
    auto* current = static_cast<LniObjectDocument*>(parsed.document.get());
    Require(current->SetModification({ObjectType::Unit, "H001"}, {"unam"}, std::string("edited\nlong text")).Succeeded(),
        "ordinary UI scalar edit including canonical long string");
    auto bundle = BundleFor(ObjectType::Unit, UnitBytes());
    EditableViews views{}; views[0] = current;
    const auto before = Capture(bundle); const auto oldSource = current->DocumentFingerprint();
    Require(PreflightScriptSaveBaselineReplacement(bundle, views).Succeeded(), "canonical edited source is safe to replace");
    Require(Capture(bundle) == before && current->DocumentFingerprint() == oldSource,
        "successful preflight must also be readonly");
    views[0] = nullptr;
    Require(PreflightScriptSaveBaselineReplacement(bundle, views).Succeeded(), "absent class has no editing metadata to lose");
    auto empty = ParseLni(ObjectType::Unit, ""); views[0] = empty.get();
    Require(PreflightScriptSaveBaselineReplacement(bundle, views).Succeeded(), "explicit empty editable class");
    auto untouched = ParseLni(ObjectType::Ability, "; untouched author's ability notes\n"); views[1] = untouched.get();
    Require(PreflightScriptSaveBaselineReplacement(bundle, views).Succeeded(), "ungenerated category must not be rejected or rewritten");
    views[0] = untouched.get();
    RejectedWithoutMutation(bundle, views, "SCRIPT_BUNDLE_BASELINE_INVALID");
    views[0] = current;
    auto scriptOnly = PipelineResult{};
    scriptOnly.script = "function main takes nothing returns nothing\nendfunction\n";
    scriptOnly.syntaxChecked = true;
    auto script = PrepareScriptSaveBundle(scriptOnly, "war3map.j");
    Require(script.Succeeded() && PreflightScriptSaveBaselineReplacement(*script.value, views).Succeeded(),
        "script-only pipeline must preserve existing behavior");
}

void TestThreeWayRetainsLniEditingState() {
    const auto catalog = ParentCatalog();
    constexpr std::string_view sourceText =
        "; author note stays in the editable project\n"
        "[P001]\n_parent = \"hfoo\"\nI001 = 10\n\n"
        "[C001]\n_parent = \"P001\"\nS001 = \"Child\"\n\n"
        "[D001]\n_parent = \"C001\"\nI001 = 30\n\n";
    const auto current = ParseLni(ObjectType::Unit, sourceText);
    auto baseline = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Require(baseline.Succeeded() &&
        ApplyLniChangesToNative(*baseline.value, *current, nullptr, &catalog).Succeeded(),
        "nested LNI source projects to frozen native baseline");
    NativeObjectDocument generated = *baseline.value;
    const ObjectKey added{ObjectType::Unit, "N001"};
    Require(generated.AddCustomObject(added.rawcode, "hfoo").Succeeded() &&
        generated.AddModification(added, {"unam"}, NativeValueType::String,
            std::string("generated")).Succeeded(),
        "generator adds unrelated custom unit");

    const NativeObjectCodec codec(ObjectType::Unit);
    const auto baselineBefore = codec.Serialize(*baseline.value);
    const auto generatedBefore = codec.Serialize(generated);
    const auto currentBefore = current->DocumentFingerprint();
    Require(baselineBefore.Succeeded() && generatedBefore.Succeeded(),
        "three-way inputs serialize before merge");
    auto merged = MergeGeneratedNativeWithLniEdits(
        *baseline.value, *current, generated, nullptr, &catalog);
    Require(merged.Succeeded() && merged.value, "unrelated generated unit merges with LNI parent chain");
    const auto* child = merged.value->FindObject({ObjectType::Unit, "C001"});
    const auto* grandchild = merged.value->FindObject({ObjectType::Unit, "D001"});
    Require(child && child->parentRawcode == "P001" &&
        grandchild && grandchild->parentRawcode == "C001" &&
        merged.value->FindObject(added),
        "merged editor document retains nested parents and new unit");
    const auto encoded = LniObjectCodec(ObjectType::Unit).Serialize(*merged.value);
    Require(encoded.Succeeded() && encoded.value &&
        encoded.value->size() >= sourceText.size() &&
        std::equal(sourceText.begin(), sourceText.end(),
            reinterpret_cast<const char*>(encoded.value->data())),
        "existing LNI text including author note remains byte-identical");
    NativeObjectDocument projected = *baseline.value;
    Require(ApplyLniChangesToNative(projected, *merged.value, nullptr, &catalog).Succeeded() &&
        codec.Serialize(projected).value == generatedBefore.value,
        "merged LNI projects to the exact generated native member bytes");
    Require(codec.Serialize(*baseline.value).value == baselineBefore.value &&
        codec.Serialize(generated).value == generatedBefore.value &&
        current->DocumentFingerprint() == currentBefore,
        "successful merge does not mutate any input");
}

void TestThreeWayRejectsSameFieldConflict() {
    const ObjectKey key{ObjectType::Unit, "H001"};
    auto baseline = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    Require(baseline.Succeeded() && baseline.value->AddCustomObject(key.rawcode, "hfoo").Succeeded() &&
        baseline.value->AddModification(key, {"unam"}, NativeValueType::String,
            std::string("before")).Succeeded(), "conflict baseline");
    const auto converted = ConvertNativeObjectDataToLni(ObjectType::Unit,
        *NativeObjectCodec(ObjectType::Unit).Serialize(*baseline.value).value);
    Require(converted.Succeeded() && converted.value, "conflict baseline LNI projection");
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*converted.value);
    auto* current = parsed.Succeeded()
        ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(current && current->SetModification(key, {"unam"},
        std::string("current edit")).Succeeded(), "independent current edit");
    NativeObjectDocument generated = *baseline.value;
    Require(generated.SetModification(key, {"unam"}, std::string("generated edit")).Succeeded(),
        "independent generated edit");

    const NativeObjectCodec codec(ObjectType::Unit);
    const auto baselineBefore = codec.Serialize(*baseline.value);
    const auto generatedBefore = codec.Serialize(generated);
    const auto currentBefore = current->DocumentFingerprint();
    const auto rejected = MergeGeneratedNativeWithLniEdits(*baseline.value, *current, generated);
    Require(!rejected.Succeeded() && !rejected.value &&
        std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.code == "lni-merge-field-conflict";
        }), "divergent edits to one native field fail closed");
    Require(codec.Serialize(*baseline.value).value == baselineBefore.value &&
        codec.Serialize(generated).value == generatedBefore.value &&
        current->DocumentFingerprint() == currentBefore,
        "rejected merge does not mutate any input");
}

void Run() {
    // 原创合成的“编译器已检查”输出，只测试提交边界；不是JASS编译或Lua执行证据。
    PipelineResult pipeline; pipeline.script = "function main takes nothing returns nothing\nendfunction\n";
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "unchecked output rejected");
    pipeline.syntaxChecked = true; pipeline.checkedBy = "synthetic fixture";
    const auto bytes = UnitBytes();
    pipeline.objectMembers = {{"WaR3MAP.w3u", bytes}};
    auto result = PrepareScriptSaveBundle(pipeline, "scripts/war3map.j");
    Require(result.Succeeded() && result.value->members.size() == 2 && result.value->nativeBaselines[0] &&
        result.value->editableBaselines[0] && !result.value->nativeBaselines[1], "typed complete bundle prepared");
    const ObjectKey key{ObjectType::Unit, "H001"};
    Require(result.value->editableBaselines[0]->FindObject(key), "generated custom unit reaches editable baseline");
    // 第二次编辑从最终结果回灌，不丢模板首次生成的名称。
    auto& editable = *result.value->editableBaselines[0];
    Require(editable.SetModification(key, {"unam"}, std::string("second edit")).Succeeded(), "second edit");
    Require(ApplyLniChangesToNative(*result.value->nativeBaselines[0], editable).Succeeded(), "second edit uses final native");
    auto second = NativeObjectCodec(ObjectType::Unit).Serialize(*result.value->nativeBaselines[0]);
    Require(second.Succeeded() && *second.value != bytes, "second native differs");
    std::vector<MemberReplacement> pending{{"WAR3MAP.W3U", Bytes("old queued")}, {"resource/large.bin", std::vector<std::byte>(4096)}};
    const auto* unrelated = pending[1].data.data();
    Require(MergeScriptSaveMembers(pending, {}, result.value->members).Succeeded() && pending.size() == 3 &&
        pending[0].data == bytes && pending[1].data.data() == unrelated && result.value->members.empty(), "merge replaces same canonical name without copying unrelated payload");
    const auto before = pending;
    auto failed = PrepareScriptSaveBundle(pipeline, "war3map.j");
    Require(!MergeScriptSaveMembers(pending, {"war3map.w3u"}, failed.value->members).Succeeded() && pending[0].data == before[0].data,
        "delete conflicts reject before mutation");
    pending.push_back({"RESOURCE\\LARGE.BIN", {}});
    Require(!MergeScriptSaveMembers(pending, {}, failed.value->members).Succeeded(), "existing ambiguous overlay rejected");
    pipeline.objectMembers.push_back({"WAR3MAP.W3U", bytes});
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "case duplicate rejected");
    pipeline.objectMembers = {{"scripts/war3map.j", bytes}};
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "native output cannot overwrite script");
    pipeline.objectMembers = {{"../war3map.w3u", bytes}};
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "traversal rejected");
    pipeline.objectMembers = {{"war3map.w3u", {std::byte{2}}}};
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "truncated output rejected");
    auto unknown = bytes; unknown[0] = std::byte{99}; pipeline.objectMembers = {{"war3map.w3u", unknown}};
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "unknown output version rejected");
    pipeline.objectMembers.clear();
    for (unsigned i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectType>(i); const auto empty = NativeObjectDocument::CreateEmpty(type);
        pipeline.objectMembers.push_back({std::string(NativeFileName(type)), *NativeObjectCodec(type).Serialize(*empty.value).value});
    }
    auto empty = PrepareScriptSaveBundle(pipeline, "war3map.j");
    Require(empty.Succeeded() && empty.value->members.size() == 8, "all seven explicit empty-v2 deletion results remain actual members");
    for (std::size_t i = 0; i < 7; ++i) Require(empty.value->nativeBaselines[i] && empty.value->editableBaselines[i] &&
        empty.value->nativeBaselines[i]->Objects().empty(), "empty classes get new baselines");
    pipeline.diagnostics.push_back({DiagnosticSeverity::Error, "fixture", "failed", "late compiler error", {}});
    Require(!PrepareScriptSaveBundle(pipeline, "war3map.j").Succeeded(), "late compiler failure cannot publish any native results");
}
}
int main() {
    try {
        Run(); TestNestedParentsAreProtected(); TestTextAndUnknownValuesAreProtected();
        TestCanonicalDomainAndCategoryIsolation(); TestThreeWayRetainsLniEditingState();
        TestThreeWayRejectsSameFieldConflict();
        std::cout << "ScriptSaveBundle transaction preparation and editable baseline preflight passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
