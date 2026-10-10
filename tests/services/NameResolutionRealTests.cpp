#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/TriggerStringEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;

constexpr std::string_view kSourceSha = "172e3b1b170b6d4d4665efa2a99cfcab3ca41f598e990b17664448a6a3c8880a";
constexpr std::string_view kCatalogSha = "5dde1003284c315466473f6dce42554409d001e9a543b326c9a85d46c3a13695";
constexpr std::string_view kMarker = "Audastra N5 20261006";

void Require(bool value, std::string_view reason) {
    if (!value) throw std::runtime_error(std::string(reason));
}

void Diagnostics(std::span<const Diagnostic> diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}

template<class T> T Take(Result<T>&& result, std::string_view reason) {
    if (!result.Succeeded() || !result.value) {
        Diagnostics(result.diagnostics);
        throw std::runtime_error(std::string(reason));
    }
    return std::move(*result.value);
}

void Check(const Status& status, std::string_view reason) {
    if (!status.Succeeded()) {
        Diagnostics(status.diagnostics);
        throw std::runtime_error(std::string(reason));
    }
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "读取真图验收开关失败");
        return {};
    }
    std::wstring value(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "读取真图验收开关失败");
        return {};
    }
    Require(length < size, "读取真图验收开关失败");
    value.resize(length);
    return std::filesystem::path(value);
}

std::vector<std::byte> Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(input.good(), "读取已捕获成员失败");
    const auto size = input.tellg();
    Require(size >= 0 && size <= 64 * 1024 * 1024, "已捕获成员超过读取预算");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(input.good(), "读取已捕获成员不完整");
    return bytes;
}

bool SameArgument(const Trigger::ArgumentNode& left, const Trigger::ArgumentNode& right, bool compareId = true) {
    return (!compareId || left.id == right.id) && left.type == right.type
        && left.insertCall == right.insertCall && left.callEca == right.callEca
        && left.insertIndex == right.insertIndex && left.indexArgument == right.indexArgument
        && Take(left.value.ToBytes(), "参数原编码不可读取") == Take(right.value.ToBytes(), "参数原编码不可读取");
}

// 完整 native 基线来自源归档，并按生产保存入口的同一规则建立查询/模板输入。
void PrepareObjects(MapSaveTask& task, ScriptSaveInputSources& inputs) {
    constexpr std::array types{ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability,
        ObjectData::ObjectType::Item, ObjectData::ObjectType::Buff, ObjectData::ObjectType::Upgrade,
        ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable};
    for (std::size_t index = 0; index < types.size(); ++index) {
        const auto type = types[index];
        const auto member = std::string(ObjectData::NativeFileName(type));
        if (!task.archive->HasMember(member)) continue;
        const auto bytes = Take(task.archive->ReadMember(member), "读取真实物编成员失败");
        const ObjectData::NativeObjectCodec codec(type);
        auto parsed = codec.Parse(bytes);
        auto* native = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
        Require(parsed.Succeeded() && native && native->IsStructurallySafe(), "真实物编基线不可完整解析");
        Require(Take(codec.Serialize(*native), "真实物编不可往返") == bytes, "真实物编原编码往返不一致");
        const auto records = native->Objects();
        if (type == ObjectData::ObjectType::Unit) inputs.units.assign(records.begin(), records.end());
        if (type == ObjectData::ObjectType::Item) inputs.items.assign(records.begin(), records.end());
        if (type == ObjectData::ObjectType::Ability) inputs.abilities.assign(records.begin(), records.end());
        if (native->Version() == 1) {
            const auto warning = "native-v1-" + std::string(ObjectData::ToString(type)) + "-read-only";
            Require(native->Supports(ObjectData::NativeObjectOperation::EditExistingValue)
                && !native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)
                && parsed.diagnostics.size() == 1
                && parsed.diagnostics.front().severity == DiagnosticSeverity::Warning
                && parsed.diagnostics.front().code == warning, "经典物编只读资格不匹配");
            continue;
        }
        Require(native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)
            && parsed.diagnostics.empty(), "真实物编未获模板无损基线资格");
        auto lniBytes = Take(ObjectData::ConvertNativeObjectDataToLni(type, bytes), "真实物编投影失败");
        auto lni = ObjectData::LniObjectCodec(type).Parse(lniBytes);
        auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(lni.document.get());
        Require(lni.Succeeded() && editable && editable->IsStructurallySafe()
            && lni.diagnostics.empty(), "真实物编投影解析失败");
        const auto projected = editable->Objects();
        Require(records.size() == projected.size() && std::equal(records.begin(), records.end(), projected.begin(),
            [](const auto& original, const auto& converted) {
                return original.key == converted.key && original.parentRawcode == converted.parentRawcode
                    && original.modifications.size() == converted.modifications.size()
                    && std::equal(original.modifications.begin(), original.modifications.end(), converted.modifications.begin(),
                        [](const auto& before, const auto& after) {
                            return before.key == after.key && before.value == after.value
                                && before.valueWasOpaque == after.valueWasOpaque;
                        });
            }), "真实物编投影改变了对象或字段");
        inputs.editableInputs[index].reset(static_cast<ObjectData::LniObjectDocument*>(lni.document.release()));
        task.nextBaselines[index].reset(static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
    }
}

void Run(const std::filesystem::path& root, const std::filesystem::path& repository,
    const std::filesystem::path& source, const std::filesystem::path& tools, const std::filesystem::path& game) {
    Require(root.is_absolute() && source.is_absolute() && tools.is_absolute() && game.is_absolute(),
        "真图验收的环境变量路径必须为绝对路径");
    Require(std::filesystem::is_directory(root), "配置的编辑器工作目录不存在");
    if (!repository.empty()) {
        Require(repository.is_absolute() && std::filesystem::equivalent(root, repository),
            "真图验收根目录必须与已配置的 FORGE_REAL_REPO_ROOT 一致");
    }
    const auto directory = root / "artifacts/astra-rounds/name-display-20261006";
    const auto target = directory / "name-resolution-candidate.w3x";
    Require(std::filesystem::is_directory(directory) && !std::filesystem::exists(target),
        "候选目录不存在或候选已存在；不覆盖现有文件");
    Require(Take(Sha256File(source), "读取原图 SHA 失败") == kSourceSha, "原图不是第16节锁定的源版本");
    auto archive = Take(MapArchiveSession::Open(source), "打开真实源图失败");
    Require(archive.SourceSha256() == kSourceSha, "源图只读快照 SHA 不匹配");
    constexpr std::array names{"war3map.wtg", "war3map.wct", "war3map.wts", "war3map.w3u"};
    constexpr std::array hashes{
        "e1a4a081d8033d471af733f8c8502526007f867f792b8182b90c2051a124c233",
        "bf1b426ace0c1dff8faf190e0b9f2ed180386d491f7edf344ed029c095ebe797",
        "ba18abf94a911b8adfcd1f7b6a0b5545c8c2406782ac2396ee3d679f91974b44",
        "1f8637a61ffe7ac62a6e9d1ca80d3989427ffefee1ced7c855b95fb8c6a87b3f"};
    std::array<std::vector<std::byte>, 4> captured;
    for (std::size_t index = 0; index < names.size(); ++index) {
        captured[index] = Read(directory / names[index]);
        Require(Take(Sha256(captured[index]), "捕获成员 SHA 失败") == hashes[index], "捕获成员身份不匹配");
        Require(Take(archive.ReadMember(names[index]), "源图成员读取失败") == captured[index],
            "捕获成员不等于当前源图对应成员");
    }
    auto catalog = Take(Trigger::UiCatalog::Load(tools), "本机实际 UI 目录加载失败");
    Require(catalog.Sha256() == kCatalogSha, "本机 UI 目录身份已变化");
    auto document = Take(Trigger::WtgWctCodec{}.Parse(captured[0], captured[1], catalog), "实际 WTG/WCT 解析失败");
    const auto originalEncoded = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "实际 WTG/WCT 往返失败");
    Require(originalEncoded.wtg == captured[0] && originalEncoded.wct == captured[1], "实际 WTG/WCT 原字节往返不一致");
    const auto originalArguments = document.argumentNodes;
    const std::array<MapArchiveMember, 1> wtsSource{{{"war3map.wts", captured[2]}}};
    const auto map = MapDocument::FromMembers(wtsSource, source);
    Require(map.Strings().has_value(), "真实 WTS 不可完整读取");
    MapData::MapDataEditSession edits(map);
    NameResolutionContext context;
    context.mapDocument = &map;
    context.triggers = &document;
    std::optional<TriggerStringEditPlan> plan;
    std::size_t selected = 0;
    std::string editedText;
    std::unordered_set<StableId> activeTriggers;
    for (const auto& trigger : document.triggers)
        if (trigger.enabled && !trigger.customTextFlag) activeTriggers.insert(trigger.id);
    // 只选当前 GUI 中启用 ECA 的直接标量，避免嵌套 call/index 或无效引用。
    for (const auto& eca : document.ecaNodes) {
        if (!eca.enabled || !activeTriggers.contains(eca.sourceTriggerId)) continue;
        for (const auto index : eca.arguments) {
            Require(index < document.argumentNodes.size(), "真实参数索引越界");
            const auto& argument = document.argumentNodes[index];
            if (argument.type != 3 || argument.insertCall || argument.callEca
                || argument.insertIndex || argument.indexArgument || !StringReferenceId(argument.value.text)) continue;
            const auto resolved = ResolveName(context, argument.value.text, "string");
            if (resolved.state != NameResolutionState::Resolved || resolved.displayName.size() > 3800) continue;
            editedText = resolved.displayName + " | " + std::string(kMarker);
            auto prepared = PrepareTriggerStringEdit(context, argument.id, editedText, TriggerStringEditScope::OnlyThisArgument);
            if (!prepared.Succeeded() || !prepared.value) continue;
            selected = index;
            plan.emplace(std::move(*prepared.value));
            break;
        }
        if (plan) break;
    }
    Require(plan.has_value(), "真实图中没有可完整解析并编辑的 TRIGSTR 标量");
    const auto originalReference = document.argumentNodes[selected].value.text;
    const auto receipt = Take(ApplyTriggerStringEdit(document, edits, *plan), "真实 OnlyThisArgument 事务提交失败");
    Require(receipt.ArgumentId() == originalArguments[selected].id && edits.Revision() == 1, "事务身份或修订异常");
    Require(SameArgument(document.argumentNodes[selected], plan->After()), "提交参数不等于准备事务的完整结果");
    Require(document.argumentNodes.size() == originalArguments.size(), "参数数量发生变化");
    for (std::size_t index = 0; index < originalArguments.size(); ++index) {
        Require(SameArgument(originalArguments[index], document.argumentNodes[index]) == (index != selected),
            "OnlyThisArgument 没有精确只改一个参数");
    }
    const auto changedReference = document.argumentNodes[selected].value.text;
    Require(StringReferenceId(changedReference) && StringReferenceId(changedReference) != StringReferenceId(originalReference),
        "OnlyThisArgument 没有分配独立新 WTS 条目");
    auto encoded = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "真实编辑 WTG/WCT 序列化失败");
    Require(encoded.wtg != captured[0] && encoded.wct == captured[1], "标量编辑改变了 WCT 或未写入 WTG");
    MapSaveTask task;
    Check(task.FreezeArchive(archive), "真实保存任务冻结失败");
    task.target = target;
    task.triggerChanges = true;
    task.requireGeneratedNativeByteEquality = true;
    task.replacements = edits.Replacements();
    Require(task.replacements.size() == 1 && task.replacements.front().name == "war3map.wts", "事务产生了无关基础成员覆盖");
    const auto expectedWts = task.replacements.front().data;
    task.replacements.push_back({"war3map.wtg", encoded.wtg});
    task.replacements.push_back({"war3map.wct", encoded.wct});
    ScriptSaveInputSources inputs;
    inputs.layers = catalog.Layers();
    inputs.expectedEffectiveLayers = inputs.layers;
    inputs.catalog = std::make_unique<Trigger::UiCatalog>(catalog);
    inputs.triggers = std::make_unique<Trigger::TriggerDocument>(document);
    inputs.toolRoot = tools;
    inputs.uiRoot = tools;
    inputs.gameRoot = game;
    inputs.scriptMember = "war3map.j";
    inputs.documentRevision = edits.Revision();
    PrepareObjects(task, inputs);
    const auto prepared = Take(PrepareScriptSaveInputs(task, std::move(inputs)), "真实保存链脚本输入准备失败");
    Require(prepared == ScriptSavePreparationState::Ready && task.script && !task.scriptSkipped,
        "真实编辑保存没有进入完整脚本重建链");
    task.beforePublish = [&archive]() { return archive.VerifySourceUnchanged(); };
    MapSaveService::Run(task, [](std::string_view stage) { std::cout << "save_stage=" << stage << '\n'; });
    Diagnostics(task.diagnostics);
    Require(task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.nextSession
        && task.written == target && task.backup.empty(), "真实编辑候选未通过保存/脚本/归档回读门禁");
    Require(std::any_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "SCRIPT_REBUILT" && diagnostic.severity == DiagnosticSeverity::Info;
    }), "没有保存链完整脚本检查回执");
    const auto savedWtg = Take(task.nextSession->ReadMember("war3map.wtg"), "候选 WTG 回读失败");
    const auto savedWct = Take(task.nextSession->ReadMember("war3map.wct"), "候选 WCT 回读失败");
    Require(savedWtg == encoded.wtg && savedWct == encoded.wct, "候选 WTG/WCT 不等于密封编辑产物");
    const auto saved = Take(Trigger::WtgWctCodec{}.Parse(savedWtg, savedWct, catalog), "候选实际目录回读解析失败");
    Require(saved.argumentNodes.size() == document.argumentNodes.size(), "候选参数数量不匹配");
    // 来源 ID 由二进制偏移生成；新引用长度改变后，后续节点重开时自然取得新 ID。
    // 向量拓扑/参数字段/原编码逐项精确回读，不把保存前 ID 误当成重开后的全局身份。
    for (std::size_t index = 0; index < saved.argumentNodes.size(); ++index)
        Require(SameArgument(saved.argumentNodes[index], document.argumentNodes[index], false), "候选参数与冻结编辑图不一致");
    const auto savedWts = Take(task.nextSession->ReadMember("war3map.wts"), "候选 WTS 回读失败");
    Require(savedWts == expectedWts, "候选 WTS 不等于编辑事务产物");
    const auto strings = Take(MapData::ParseStringTable(savedWts), "候选 WTS 完整解析失败");
    Require(strings.entries.size() == map.Strings()->entries.size() + 1, "OnlyThisArgument 没有精确新增一个 WTS 条目");
    for (std::size_t index = 0; index < map.Strings()->entries.size(); ++index) {
        const auto& before = map.Strings()->entries[index];
        const auto& after = strings.entries[index];
        Require(after.id == before.id && after.value == before.value, "原 WTS 条目被改写或重排");
    }
    const auto newId = *StringReferenceId(changedReference);
    const auto added = std::find_if(strings.entries.begin(), strings.entries.end(),
        [newId](const auto& entry) { return entry.id == newId; });
    Require(added != strings.entries.end(), "新 WTS 条目不存在");
    const auto body = Trigger::DecodeSourceText(std::as_bytes(std::span(added->value.data(), added->value.size())));
    Require(body.text == editedText, "新 WTS 正文原编码往返后文本不一致");
    const std::array<MapArchiveMember, 1> savedSource{{{"war3map.wts", savedWts}}};
    const auto savedMap = MapDocument::FromMembers(savedSource, target);
    context.mapDocument = &savedMap;
    context.triggers = &saved;
    const auto resolved = ResolveName(context, saved.argumentNodes[selected].value.text, "string");
    Require(resolved.state == NameResolutionState::Resolved && resolved.displayName == editedText,
        "候选名称解析没有显示本次编辑正文");
    const auto script = Take(task.nextSession->ReadMember("war3map.j"), "候选完整 JASS 回读失败");
    Require(!script.empty(), "候选 JASS 为空");
    Check(archive.VerifySourceUnchanged(), "只读源租约最终校验失败");
    Require(Take(Sha256File(source), "最终原图 SHA 读取失败") == kSourceSha, "原图 SHA 发生变化");
    FinalizeScriptWorkspace(task);
    std::cout << "argument_id=" << receipt.ArgumentId() << "\nargument_index=" << selected
        << "\nargument_origin_offset=" << originalArguments[selected].origin.offset
        << "\nreference_before=" << originalReference << "\nreference_after=" << changedReference
        << "\nsource_sha256=" << kSourceSha << "\ncatalog_sha256=" << catalog.Sha256()
        << "\ncandidate_path=" << target.generic_string()
        << "\ncandidate_sha256=" << task.nextSession->SourceSha256()
        << "\nwtg_sha256=" << Take(Sha256(savedWtg), "WTG SHA 失败")
        << "\nwct_sha256=" << Take(Sha256(savedWct), "WCT SHA 失败")
        << "\nwts_sha256=" << Take(Sha256(savedWts), "WTS SHA 失败")
        << "\nscript_sha256=" << Take(Sha256(script), "JASS SHA 失败")
        << "\nscript_rebuilt=true\nsource_unchanged=true\nonly_this_argument=true"
        << "\noriginal_editor_validated=false\nruntime_validated=false\n";
}
} // namespace

int main() {
    try {
        const auto root = EnvironmentPath(L"FORGE_NAME_RESOLUTION_REAL_ROOT");
        const auto source = EnvironmentPath(L"FORGE_NAME_RESOLUTION_REAL_MAP");
        const auto tools = EnvironmentPath(L"FORGE_YDWE_ROOT");
        const auto game = EnvironmentPath(L"FORGE_REAL_GAME_ROOT");
        const auto repository = EnvironmentPath(L"FORGE_REAL_REPO_ROOT");
        if (root.empty() || source.empty() || tools.empty() || game.empty()) {
            std::cout << "SKIP: FORGE_NAME_RESOLUTION_REAL_ROOT, FORGE_NAME_RESOLUTION_REAL_MAP, "
                "FORGE_YDWE_ROOT and FORGE_REAL_GAME_ROOT are required for the bound candidate-only acceptance.\n";
            return 77;
        }
        Run(root, repository, source, tools, game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NameResolutionRealTests: " << error.what() << '\n';
        return 1;
    }
}
