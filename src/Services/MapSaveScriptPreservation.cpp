#include "MapSaveScriptPreservation.h"

#include "War3MapCore/Jass/JassDeclarationIndex.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <algorithm>
#include <map>
#include <string>

namespace War3Map::Services::ScriptPreservation {
namespace {

bool SameName(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? char(value + 'a' - 'A') : value; };
        return lower(a) == lower(b);
    });
}

// 源归档里的成员原字节；读取失败时返回空，调用方按“不能证明”处理。
struct SourceMember { bool present = false; std::vector<std::byte> bytes; };

std::optional<SourceMember> ReadSource(const MapArchiveSession& archive, std::string_view name) {
    const std::vector<std::string> names{std::string(name)};
    auto captured = archive.CaptureExactMembers(names);
    if (!captured.Succeeded() || !captured.value || captured.value->size() != 1) return std::nullopt;
    auto& member = captured.value->front();
    return SourceMember{member.present, std::move(member.bytes)};
}

// 本次保存的最终成员与源归档相同（同样存在且字节相同，或同样不存在）。
bool Unchanged(const MapSaveTask& task, std::string_view name) {
    const auto replaced = std::find_if(task.replacements.begin(), task.replacements.end(),
        [&](const auto& member) { return SameName(member.name, name); });
    const bool removed = std::any_of(task.removals.begin(), task.removals.end(),
        [&](const auto& member) { return SameName(member, name); });
    if (replaced == task.replacements.end() && !removed) return true;
    const auto source = ReadSource(*task.archive, name);
    if (!source) return false;
    if (replaced == task.replacements.end()) return !source->present;
    return source->present && source->bytes == replaced->data;
}

std::optional<std::vector<ObjectData::ObjectRecord>> SourceRecords(const MapSaveTask& task,
    ObjectData::ObjectType type, const std::vector<ObjectData::ObjectRecord>& current) {
    const auto name = ObjectData::NativeFileName(type);
    // 成员未变时两边用同一份记录，比较结果只反映真实改动。
    if (Unchanged(task, name)) return current;
    const auto source = ReadSource(*task.archive, name);
    if (!source) return std::nullopt;
    if (!source->present) return std::vector<ObjectData::ObjectRecord>{};
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(source->bytes);
    const auto* native = parsed.Succeeded() && parsed.document
        ? dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
    if (!native) return std::nullopt;
    return std::vector<ObjectData::ObjectRecord>(native->Objects().begin(), native->Objects().end());
}

std::string Signature(const Jass::DeclarationSymbol& symbol) {
    std::string text = "takes ";
    if (symbol.parameters.empty()) text += "nothing";
    for (std::size_t index = 0; index < symbol.parameters.size(); ++index) {
        if (index) text += ", ";
        text += symbol.parameters[index].type;
    }
    return text + " returns " + (symbol.type.empty() ? std::string("nothing") : symbol.type);
}

std::optional<std::map<std::string, std::string>> Functions(std::span<const std::byte> bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const Jass::JassSource sources[]{{"war3map.j", text}};
    const auto index = Jass::IndexJassDeclarations(sources);
    if (!index.completed) return std::nullopt;
    std::map<std::string, std::string> functions;
    for (const auto& symbol : index.symbols)
        if (symbol.kind == Jass::DeclarationKind::Function) functions.emplace(symbol.name, Signature(symbol));
    return functions;
}

} // namespace

std::optional<std::vector<std::byte>> UnaffectedSourceScript(const MapSaveTask& task,
    const Script::MapScriptBuildRequest& request, const Script::MapScriptBuildResult& built,
    std::span<const MemberReplacement> prepared, std::string_view scriptMember) {
    if (!task.preserveUnaffectedScript || !task.archive || request.luaTarget || !built.pipeline || built.luaPipeline || built.classicLuaBundle)
        return std::nullopt;
    const auto& pipeline = *built.pipeline;
    // 启动和客户端身份属于构建语义；仅有历史按需批准、实际无依赖时仍可保留作者脚本。
    if (request.toolchain.mapRuntimeBoot) {
        const auto& boot = *request.toolchain.mapRuntimeBoot;
        if (boot.ownsExisting || (boot.enabled && !boot.onlyIfUsed)) return std::nullopt;
        if (boot.enabled) {
            const auto usage = Script::AnalyzeMapRuntimeUsage(pipeline.script);
            if (!usage.Succeeded() || usage.value->required) return std::nullopt;
        }
    }
    // 模板生成物编或导入文件时这些产物也要写入，不能只沿用脚本。
    if (!pipeline.objectMembers.empty() || !pipeline.templateImports.empty()) return std::nullopt;
    if (prepared.size() != 1 || !SameName(prepared.front().name, scriptMember)) return std::nullopt;
    for (const std::string_view name : {std::string_view("war3map.wtg"), std::string_view("war3map.wct"), scriptMember})
        if (!Unchanged(task, name)) return std::nullopt;

    auto source = request;
    source.finalSaveMembers.reset();
    source.editedMapMembers.clear();
    source.removedMapMembers.clear();
    source.compile = false;
    source.toolchain.onStage = nullptr;
    auto units = SourceRecords(task, ObjectData::ObjectType::Unit, request.units);
    auto items = SourceRecords(task, ObjectData::ObjectType::Item, request.items);
    auto abilityRecords = SourceRecords(task, ObjectData::ObjectType::Ability, request.abilities);
    if (!units || !items || !abilityRecords) return std::nullopt;
    source.units = std::move(*units);
    source.items = std::move(*items);
    source.abilities = std::move(*abilityRecords);
    const auto regenerated = Script::BuildMapScript(source);
    if (!regenerated.generated.Succeeded() || regenerated.generated.text != built.generated.text) return std::nullopt;
    if (pipeline.templateWholeChunk || pipeline.templateExpressions > 0) {
        // 模板还会读物编（slk 库、技能命令串）：用源地图状态完整构建一次，最终脚本相同才算不受影响。
        source.compile = true;
        source.toolchain.workDir = request.toolchain.workDir / "source-check";
        const auto full = Script::BuildMapScript(source);
        const auto* script = full.FinalScript();
        if (!full.pipeline || !script || *script != pipeline.script
            || !full.pipeline->objectMembers.empty() || !full.pipeline->templateImports.empty())
            return std::nullopt;
    }

    auto original = ReadSource(*task.archive, scriptMember);
    if (!original || !original->present) return std::nullopt;
    // 源脚本须是完整地图脚本；空文件或占位内容不能沿用。
    const auto functions = Functions(original->bytes);
    if (!functions || !functions->contains("main") || !functions->contains("config")) return std::nullopt;
    return std::move(original->bytes);
}

void ReportSignatureChanges(std::span<const std::byte> source, std::span<const std::byte> saved,
    bool preserved, std::vector<Diagnostic>& diagnostics) {
    const auto before = Functions(source);
    const auto after = Functions(saved);
    if (!before || !after) return;
    std::vector<std::string> changes;
    for (const auto& [name, signature] : *before) {
        const auto found = after->find(name);
        if (found != after->end() && found->second != signature)
            changes.push_back(name + "：" + signature + " → " + found->second);
    }
    if (changes.empty()) return;
    constexpr std::size_t kShown = 10;
    std::string list;
    for (std::size_t index = 0; index < changes.size() && index < kShown; ++index)
        list += (index ? "；" : "") + changes[index];
    if (changes.size() > kShown) list += "；等 " + std::to_string(changes.size()) + " 个";
    const auto count = std::to_string(changes.size());
    diagnostics.push_back(preserved
        ? Diagnostic{DiagnosticSeverity::Info, "构建", "SCRIPT_FUNCTION_SIGNATURE_DIFFERS",
            "本机函数库生成的脚本有 " + count + " 个函数签名与地图原脚本不同，本次沿用原脚本：" + list}
        : Diagnostic{DiagnosticSeverity::Warning, "构建", "SCRIPT_FUNCTION_SIGNATURE_CHANGED",
            "重建后的脚本有 " + count + " 个函数签名与地图原脚本不同，多半是本机函数库版本与作者保存时不同：" + list});
}

} // namespace War3Map::Services::ScriptPreservation
