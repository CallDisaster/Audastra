// 真实 YDWE 模板产物与物编封印的提交边界。设置 FORGE_YDWE_ROOT 后才运行；
// 未设置返回 77。只读本机工具链，在私有临时目录使用原创合成地图。
// 模板脚本尚未经过 JASS 检查，本测试不伪造 checkedBy，也不发布保存候选。
#include "War3MapCore/Services/ObjectTransactionSaveGuards.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Script/LuaTemplateTransaction.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Script;
using namespace War3Map::Services;
using Snapshot = std::shared_ptr<const ObjectTransactionSnapshot>;

constexpr std::string_view kOriginalName = "forge guard original";
constexpr std::string_view kCandidateName = "forge guard candidate";
constexpr std::string_view kChangedName = "forge guard beyond seal";
constexpr std::string_view kScript = "function main takes nothing returns nothing\nendfunction\n";

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Codes(const auto& result) {
    std::string codes;
    for (const auto& diagnostic : result.diagnostics)
        codes += diagnostic.code + ": " + diagnostic.message + "\n";
    return codes;
}
bool HasCode(const Status& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

struct Scratch {
    std::filesystem::path root;
    Scratch() {
        static std::atomic<std::uint64_t> sequence{0};
        root = std::filesystem::temp_directory_path()
            / ("Audastra-ObjectTemplateGuards-" + std::to_string(GetCurrentProcessId()) + "-"
                + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(root), "private template test directory must be newly created");
        root = std::filesystem::canonical(root);
    }
    ~Scratch() {
        std::error_code error;
        if (root.filename().wstring().starts_with(L"Audastra-ObjectTemplateGuards-"))
            std::filesystem::remove_all(root, error);
    }
};

std::string RenameTemplate(std::string_view name) {
    // 目标 YDWE 将带 w2lobject 标记的模板对象移入 session.old；
    // 重复执行创建模板的产物与旧值比较，而非直接写 rawSlk getter。
    // 同一模板在原始值上产生输出、在候选值上零输出，才验证最终输入。
    std::string text = "<?\nlocal slk = require 'slk'\nlocal o = slk.ability.AHhb:new('A9Z1')\n";
    text += "o.Name = '" + std::string(name) + "'\n?>" + std::string(kScript);
    return text;
}

const MemberReplacement& AbilityOutput(const LuaTemplateOutput& output) {
    Require(output.objectMembers.size() == 1 && output.objectMembers.front().name == "war3map.w3a",
        "real template must output exactly the ability table");
    return output.objectMembers.front();
}

void RequireNativeName(const LuaTemplateOutput& output, std::string_view rawcode, std::string_view name) {
    const auto parsed = NativeObjectCodec(ObjectType::Ability).Parse(AbilityOutput(output).data);
    const auto* native = parsed.Succeeded() ? dynamic_cast<const NativeObjectDocument*>(parsed.document.get()) : nullptr;
    const auto* object = native ? native->FindObject({ObjectType::Ability, std::string(rawcode)}) : nullptr;
    Require(object != nullptr, "real template native output contains " + std::string(rawcode));
    const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
        [](const ObjectModification& modification) { return modification.key.fieldId == "anam"; });
    Require(field != object->modifications.end() && field->value == Value{std::string(name)},
        "real template native output has the expected Name value for " + std::string(rawcode));
}

struct Fixture {
    LuaTemplateToolchain toolchain;
    Scratch scratch;
    MapArchiveSession source;
    std::vector<std::byte> originalAbility;
    Snapshot snapshot;
    std::vector<MemberReplacement> finalReplacements;
    MapScriptBuildRequest saveRequest;
    std::string sourceSha;
    unsigned runs = 0;

    explicit Fixture(LuaTemplateToolchain selected) : toolchain(std::move(selected)) {
        // 原生祖先由真实 frontend 生成，保证对象名/父级/字段地址与工具链一致。
        const auto seed = OpenMap("seed.w3x", {{"war3map.j", Bytes(kScript)}});
        const auto initialSources = SlkMapSourceSnapshot::Capture(seed, {}, {}, 0);
        Require(initialSources.Succeeded() && initialSources.value, "capture bootstrap sources: " + Codes(initialSources));
        const auto original = Run(*initialSources.value, RenameTemplate(kOriginalName));
        originalAbility = AbilityOutput(original).data;
        source = OpenMap("source.w3x", {{"war3map.j", Bytes(kScript)},
            {"war3map.w3a", originalAbility}, {"preserve.bin", {std::byte{0}, std::byte{0xff}}}});
        sourceSha = source.SourceSha256();

        const auto lniBytes = ConvertNativeObjectDataToLni(ObjectType::Ability, originalAbility);
        Require(lniBytes.Succeeded() && lniBytes.value, "real template native projects into LNI: " + Codes(lniBytes));
        auto parsed = LniObjectCodec(ObjectType::Ability).Parse(*lniBytes.value);
        auto* editable = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
        Require(editable != nullptr, "real template LNI parses: " + Codes(parsed));
        const auto* object = editable->FindObject({ObjectType::Ability, "A9Z1"});
        Require(object != nullptr, "actual template creates A9Z1");
        const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
            [](const ObjectModification& modification) { return modification.key.fieldId == "anam"; });
        Require(field != object->modifications.end() && field->value == Value{std::string(kOriginalName)},
            "actual template Name property produces the expected native anam field");
        const auto address = field->key;
        const auto objectKey = object->key;
        Require(editable->SetModification(objectKey, address, std::string(kCandidateName)).Succeeded(),
            "candidate name is edited through the real LNI document API");
        const auto frozen = ObjectTransactionSnapshot::Freeze(*editable, originalAbility, sourceSha);
        Require(frozen.Succeeded() && frozen.value, "freeze same-source ability candidate: " + Codes(frozen));
        snapshot = *frozen.value;
        finalReplacements.push_back({"war3map.w3a", {snapshot->EncodedNative().begin(), snapshot->EncodedNative().end()}});
        saveRequest.archive = &source;
        saveRequest.documentRevision = 17;
        saveRequest.finalSaveMembers = MapScriptFinalMembers{finalReplacements, {}};
    }

    MapArchiveSession OpenMap(std::string_view filename, const std::vector<MapArchiveMember>& members) const {
        const auto saved = WriteVerifiedArchive(scratch.root / std::filesystem::path(filename), members);
        Require(saved.Succeeded() && saved.value, "write verified synthetic template map: " + Codes(saved));
        auto opened = MapArchiveSession::Open(saved.value->path);
        Require(opened.Succeeded() && opened.value, "open actual template source: " + Codes(opened));
        return std::move(*opened.value);
    }

    LuaTemplateOutput Run(const SlkMapSourceSnapshot& sources, std::string_view text) {
        const auto work = scratch.root / ("work-" + std::to_string(++runs));
        Require(std::filesystem::create_directory(work), "each real interpreter execution owns its work directory");
        LuaTemplateLimits limits;
        limits.timeout = std::chrono::seconds{60};
        const auto output = RunLuaTemplateTransaction(text, sources, toolchain, work, limits);
        Require(output.Succeeded() && output.value, "real YDWE template execution failed: " + Codes(output));
        Require(output.value->script == kScript, "real interpreter removes template tags and preserves the emitted script");
        return *output.value;
    }

    SlkMapSourceSnapshot CandidateSources() const {
        // 对应 BuildMapScript 的 wholeChunk 捕获入口：同次 finalSaveMembers view，
        // 原始归档未写入 candidate。这里仅执行模板边界，不冒称整图编译已通过。
        const auto& final = *saveRequest.finalSaveMembers;
        auto captured = SlkMapSourceSnapshot::Capture(*saveRequest.archive,
            final.replacements, final.removals, saveRequest.documentRevision);
        Require(captured.Succeeded() && captured.value, "capture final-save candidate: " + Codes(captured));
        const auto& candidate = captured.value->Members()[1];
        const auto& original = captured.value->OriginalMembers()[1];
        Require(candidate.name == "war3map.w3a" && original.name == candidate.name
            && candidate.identity.presence == SlkSessionSourcePresence::Present
            && original.identity.presence == SlkSessionSourcePresence::Present
            && std::ranges::equal(candidate.bytes, snapshot->EncodedNative())
            && original.bytes == originalAbility
            && captured.value->Revision() == saveRequest.documentRevision,
            "template input owns candidate bytes and preserves the actual native ancestor");
        return std::move(*captured.value);
    }

    Status Guard(const LuaTemplateOutput& actualOutput) const {
        // 直接核验真实模板产物。脚本尚未经过 JASS 检查，不构造 PipelineResult。
        auto generated = actualOutput.objectMembers;
        generated.push_back({"war3map.j", Bytes(actualOutput.script)});
        const std::array<Snapshot, 1> seals{snapshot};
        return ValidateGeneratedObjectMembersForSave(source, seals, generated, "war3map.j");
    }

    void Unchanged() const {
        const auto sourceHash = Sha256File(source.SourcePath());
        const auto candidateHash = Sha256(snapshot->EncodedNative());
        const auto lniHash = Sha256(snapshot->SerializedLni());
        Require(sourceHash.Succeeded() && sourceHash.value && *sourceHash.value == sourceSha
            && source.VerifySourceUnchanged().Succeeded()
            && snapshot->VerifyAgainst(sourceSha, originalAbility).Succeeded()
            && candidateHash.Succeeded() && candidateHash.value && *candidateHash.value == snapshot->CandidateSha256()
            && lniHash.Succeeded() && lniHash.value && *lniHash.value == snapshot->LniSha256(),
            "real template and guard leave original archive and owned snapshot bytes unchanged");
        Require(!std::filesystem::exists(scratch.root / "candidate.w3x"), "template boundary test never publishes a candidate map");
    }
};

void TestRealTemplateHonorsCandidateAndCannotExceedSeal(const LuaTemplateToolchain& toolchain) {
    Fixture fixture(toolchain);
    const auto originalSources = SlkMapSourceSnapshot::Capture(fixture.source, {}, {}, 0);
    Require(originalSources.Succeeded() && originalSources.value, "capture actual native ancestor");
    const auto authorized = fixture.Run(*originalSources.value, RenameTemplate(kCandidateName));
    RequireNativeName(authorized, "A9Z1", kCandidateName);
    Require(std::ranges::equal(AbilityOutput(authorized).data, fixture.snapshot->EncodedNative()),
        "real template produces the exact sealed candidate when applying the same name edit");
    Require(fixture.Guard(authorized).Succeeded(), "actual template output equal to the candidate passes the seal guard");

    const auto finalSources = fixture.CandidateSources();
    const auto unchanged = fixture.Run(finalSources, RenameTemplate(kCandidateName));
    Require(unchanged.objectMembers.empty(), "real template reads final candidate and repeating the edit generates no native output");
    Require(fixture.Guard(unchanged).Succeeded(), "a no-op real template still verifies the same source seal");

    const auto changed = fixture.Run(finalSources, RenameTemplate(kChangedName));
    RequireNativeName(changed, "A9Z1", kChangedName);
    Require(!std::ranges::equal(AbilityOutput(changed).data, fixture.snapshot->EncodedNative()),
        "real template produces changed native bytes for another Name value");
    const auto changedGuard = fixture.Guard(changed);
    Require(!changedGuard.Succeeded() && HasCode(changedGuard, "SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED"),
        "actual template field mutation is rejected before publication: " + Codes(changedGuard));

    const std::string addedTemplate = "<?\nlocal slk = require 'slk'\n"
        "local retained = slk.ability.AHhb:new('A9Z1')\nretained.Name = '" + std::string(kCandidateName) + "'\n"
        "local o = slk.ability.AHhb:new('A9Z2')\no.Name = 'forge guard extra'\n?>" + std::string(kScript);
    const auto added = fixture.Run(finalSources, addedTemplate);
    RequireNativeName(added, "A9Z1", kCandidateName);
    RequireNativeName(added, "A9Z2", "forge guard extra");
    const auto addedGuard = fixture.Guard(added);
    Require(!addedGuard.Succeeded() && HasCode(addedGuard, "SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED"),
        "actual template extra-object output is rejected before publication: " + Codes(addedGuard));
    fixture.Unchanged();
}
} // namespace

int main() {
    try {
        wchar_t root[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", root, 1024);
        if (length == 0) {
            std::cout << "SKIP: FORGE_YDWE_ROOT not set; real template guard gate not executed\n";
            return 77;
        }
        Require(length < 1024, "FORGE_YDWE_ROOT exceeds the supported path budget");
        const auto toolchain = ResolveLuaTemplateToolchain(std::filesystem::path(root));
        Require(toolchain.Succeeded() && toolchain.value, "configured YDWE template toolchain is unavailable: " + Codes(toolchain));
        TestRealTemplateHonorsCandidateAndCannotExceedSeal(*toolchain.value);
        std::cout << "ObjectTransactionTemplateSaveGuardsTests: real YDWE candidate/no-op/field/addition boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ObjectTransactionTemplateSaveGuardsTests: " << error.what() << '\n';
        return 1;
    }
}
