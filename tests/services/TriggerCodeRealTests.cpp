#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Trigger::TriggerCodeEditor;

constexpr std::string_view kSourceSha = "9121e72f699da4456e332a4a9f2c4dbdcedb00fa9e5be1f5bf224d2f3e7b7379";
constexpr std::string_view kCatalogSha = "97e1f39ec724d9cd1334f0b9ea1bc43e9502a9ac2f34356f6e7c910525aa8e13";
constexpr StableId kConvertedTrigger = 1369094286722480975ULL;
constexpr std::string_view kNewTriggerName = "AudastraC1Nightly_20261006";

void Require(bool value, std::string_view reason) {
    if (!value) throw std::runtime_error(std::string(reason));
}

void Diagnostics(std::span<const Diagnostic> diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
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
            "读取 C1 真图验收开关失败");
        return {};
    }
    std::wstring value(size, L'\0');
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length) {
        const auto error = GetLastError();
        Require(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND,
            "读取 C1 真图验收开关失败");
        return {};
    }
    Require(length < size, "读取 C1 真图验收开关失败");
    value.resize(length);
    return std::filesystem::path(value);
}

std::vector<std::byte> Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(input.good(), "读取已捕获 C1 成员失败");
    const auto size = input.tellg();
    Require(size >= 0 && size <= 64 * 1024 * 1024, "已捕获 C1 成员超过读取预算");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(input.good(), "读取已捕获 C1 成员不完整");
    return bytes;
}

bool SameRaw(const Trigger::RawText& left, const Trigger::RawText& right) {
    return left.encoding == right.encoding && Take(left.ToBytes(), "来源文本编码不可读取")
        == Take(right.ToBytes(), "来源文本编码不可读取");
}

bool SameOrigin(const OriginSpan& left, const OriginSpan& right) {
    return left.source == right.source && left.offset == right.offset && left.length == right.length;
}

std::vector<StableId> EcaIds(const Trigger::TriggerDocument& document, std::span<const std::uint32_t> indices) {
    std::vector<StableId> ids;
    ids.reserve(indices.size());
    for (const auto index : indices) {
        Require(index < document.ecaNodes.size(), "真实 ECA 引用越界");
        ids.push_back(document.ecaNodes[index].id);
    }
    return ids;
}

std::vector<StableId> ArgumentIds(const Trigger::TriggerDocument& document, std::span<const std::uint32_t> indices) {
    std::vector<StableId> ids;
    ids.reserve(indices.size());
    for (const auto index : indices) {
        Require(index < document.argumentNodes.size(), "真实参数引用越界");
        ids.push_back(document.argumentNodes[index].id);
    }
    return ids;
}

// 只读遍历原 GUI 闭包，以节点身份证明转换没有移除或重写其他触发器。
struct Closure {
    std::unordered_set<std::uint32_t> ecas;
    std::unordered_set<std::uint32_t> arguments;
};

Closure ConvertedClosure(const Trigger::TriggerDocument& document, const Trigger::TriggerRecord& trigger) {
    Closure result;
    std::vector<std::pair<bool, std::uint32_t>> todo;
    for (const auto index : trigger.ecas) todo.emplace_back(false, index);
    while (!todo.empty()) {
        const auto [argument, index] = todo.back();
        todo.pop_back();
        if (argument) {
            Require(index < document.argumentNodes.size(), "待转换原参数越界");
            if (!result.arguments.insert(index).second) continue;
            const auto& node = document.argumentNodes[index];
            if (node.callEca) todo.emplace_back(false, *node.callEca);
            if (node.indexArgument) todo.emplace_back(true, *node.indexArgument);
        } else {
            Require(index < document.ecaNodes.size(), "待转换原 ECA 越界");
            if (!result.ecas.insert(index).second) continue;
            const auto& node = document.ecaNodes[index];
            Require(node.sourceTriggerId == trigger.id, "原转换闭包含有其他触发器所有的 ECA");
            for (const auto child : node.children) todo.emplace_back(false, child);
            for (const auto child : node.arguments) todo.emplace_back(true, child);
        }
    }
    return result;
}

void VerifyUnrelatedGraph(const Trigger::TriggerDocument& before, const Trigger::TriggerDocument& after,
    const Closure& removed, StableId newTrigger) {
    Require(after.triggers.size() == before.triggers.size() + 1 && after.categories.size() == before.categories.size()
        && after.variables.size() == before.variables.size() && after.variablesHeader == before.variablesHeader
        && after.wct.version == before.wct.version, "C1 事务改变无关目录/变量数量或格式头");
    for (std::size_t index = 0; index < before.triggers.size(); ++index)
        Require(after.triggers[index].id == before.triggers[index].id, "C1 事务改变原触发器行顺序");
    Require(after.triggers.back().id == newTrigger, "新自定义触发器没有唯一追加在目录末尾");
    for (std::size_t index = 0; index < before.categories.size(); ++index) {
        const auto& a = before.categories[index]; const auto& b = after.categories[index];
        Require(a.id == b.id && a.categoryId == b.categoryId && a.comment == b.comment && SameRaw(a.name, b.name)
            && SameOrigin(a.origin, b.origin),
            "C1 事务改变原类别");
    }
    for (std::size_t index = 0; index < before.variables.size(); ++index) {
        const auto& a = before.variables[index]; const auto& b = after.variables[index];
        Require(a.id == b.id && a.unknown == b.unknown && a.isArray == b.isArray && a.size == b.size
            && a.hasDefault == b.hasDefault && SameRaw(a.name, b.name) && SameRaw(a.typeName, b.typeName)
            && SameRaw(a.value, b.value) && SameOrigin(a.origin, b.origin), "C1 事务改变原 GUI 变量");
    }
    std::unordered_map<StableId, const Trigger::TriggerRecord*> triggers;
    for (const auto& trigger : after.triggers)
        Require(triggers.emplace(trigger.id, &trigger).second, "编辑图存在重复触发器身份");
    Require(triggers.contains(newTrigger), "新自定义触发器身份不存在");
    for (const auto& a : before.triggers) {
        Require(triggers.contains(a.id), "C1 事务移除原触发器");
        const auto& b = *triggers.at(a.id);
        Require(SameRaw(a.name, b.name) && SameRaw(a.description, b.description) && a.type == b.type
            && a.enabled == b.enabled && a.initiallyClosed == b.initiallyClosed && a.categoryId == b.categoryId
            && SameOrigin(a.origin, b.origin),
            "C1 事务改变原触发器元数据");
        if (a.id != kConvertedTrigger)
            Require(a.runOnInit == b.runOnInit && a.customTextFlag == b.customTextFlag && SameRaw(a.customText, b.customText)
                && EcaIds(before, a.ecas) == EcaIds(after, b.ecas), "C1 事务改变未选触发器代码或根树");
    }
    Require(after.ecaNodes.size() == before.ecaNodes.size() - removed.ecas.size()
        && after.argumentNodes.size() == before.argumentNodes.size() - removed.arguments.size(),
        "GUI 转换没有精确移除选中闭包");
    std::unordered_map<StableId, const Trigger::EcaNode*> ecas;
    std::unordered_map<StableId, const Trigger::ArgumentNode*> arguments;
    for (const auto& node : after.ecaNodes) Require(ecas.emplace(node.id, &node).second, "重复 ECA 身份");
    for (const auto& node : after.argumentNodes) Require(arguments.emplace(node.id, &node).second, "重复参数身份");
    for (std::size_t index = 0; index < before.ecaNodes.size(); ++index) {
        const auto& a = before.ecaNodes[index];
        if (removed.ecas.contains(static_cast<std::uint32_t>(index))) {
            Require(!ecas.contains(a.id), "转换后仍保留被移除 ECA");
            continue;
        }
        Require(ecas.contains(a.id), "转换移除了无关 ECA");
        const auto& b = *ecas.at(a.id);
        Require(a.type == b.type && a.childId == b.childId && a.sourceTriggerId == b.sourceTriggerId
            && a.enabled == b.enabled && SameRaw(a.name, b.name) && a.uiSource == b.uiSource
            && a.uiLayer == b.uiLayer && a.uiSignature == b.uiSignature && SameOrigin(a.origin, b.origin)
            && EcaIds(before, a.children) == EcaIds(after, b.children)
            && ArgumentIds(before, a.arguments) == ArgumentIds(after, b.arguments), "转换改写无关 ECA");
    }
    const auto callIdentity = [](const Trigger::TriggerDocument& document, const auto& index) {
        return index ? document.ecaNodes.at(*index).id : StableId{0};
    };
    const auto indexIdentity = [](const Trigger::TriggerDocument& document, const auto& index) {
        return index ? document.argumentNodes.at(*index).id : StableId{0};
    };
    for (std::size_t index = 0; index < before.argumentNodes.size(); ++index) {
        const auto& a = before.argumentNodes[index];
        if (removed.arguments.contains(static_cast<std::uint32_t>(index))) {
            Require(!arguments.contains(a.id), "转换后仍保留被移除参数");
            continue;
        }
        Require(arguments.contains(a.id), "转换移除了无关参数");
        const auto& b = *arguments.at(a.id);
        Require(a.type == b.type && a.insertCall == b.insertCall && a.insertIndex == b.insertIndex && SameRaw(a.value, b.value)
            && callIdentity(before, a.callEca) == callIdentity(after, b.callEca)
            && indexIdentity(before, a.indexArgument) == indexIdentity(after, b.indexArgument)
            && SameOrigin(a.origin, b.origin), "转换改写无关参数");
    }
}

// 复用 N5 与生产 MapEditSave 的物编查询/模板基线准备规则。
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
                && parsed.diagnostics.size() == 1 && parsed.diagnostics.front().severity == DiagnosticSeverity::Warning
                && parsed.diagnostics.front().code == warning, "经典物编只读资格不匹配");
            continue;
        }
        Require(native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)
            && parsed.diagnostics.empty(), "真实物编未获模板无损基线资格");
        const auto lniBytes = Take(ObjectData::ConvertNativeObjectDataToLni(type, bytes), "真实物编投影失败");
        auto lni = ObjectData::LniObjectCodec(type).Parse(lniBytes);
        auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(lni.document.get());
        Require(lni.Succeeded() && editable && editable->IsStructurallySafe() && lni.diagnostics.empty(), "真实物编投影解析失败");
        const auto projected = editable->Objects();
        Require(records.size() == projected.size() && std::equal(records.begin(), records.end(), projected.begin(),
            [](const auto& original, const auto& converted) {
                return original.key == converted.key && original.parentRawcode == converted.parentRawcode
                    && original.modifications.size() == converted.modifications.size()
                    && std::equal(original.modifications.begin(), original.modifications.end(), converted.modifications.begin(),
                        [](const auto& before, const auto& after) {
                            return before.key == after.key && before.value == after.value && before.valueWasOpaque == after.valueWasOpaque;
                        });
            }), "真实物编投影改变了对象或字段");
        inputs.editableInputs[index].reset(static_cast<ObjectData::LniObjectDocument*>(lni.document.release()));
        task.nextBaselines[index].reset(static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
    }
}

void Run(const std::filesystem::path& root, const std::filesystem::path& repository,
    const std::filesystem::path& source, const std::filesystem::path& tools, const std::filesystem::path& game) {
    Require(root.is_absolute() && source.is_absolute() && tools.is_absolute() && game.is_absolute(),
        "C1 真图验收的环境变量路径必须为绝对路径");
    Require(std::filesystem::is_directory(root), "配置的 C1 编辑器工作目录不存在");
    if (!repository.empty()) {
        Require(repository.is_absolute() && std::filesystem::equivalent(root, repository),
            "C1 真图验收根目录必须与已配置的 FORGE_REAL_REPO_ROOT 一致");
    }
    const auto directory = root / "artifacts/astra-rounds/nightly-maptest-20261006";
    const auto target = directory / "c1-asphyxia-candidate.w3x";
    Require(std::filesystem::is_directory(directory) && !std::filesystem::exists(target),
        "候选目录不存在或候选已存在；不覆盖现有文件");
    Require(std::filesystem::file_size(source) == 145081468 && Take(Sha256File(source), "源图 SHA 读取失败") == kSourceSha,
        "C1 原图不是锁定的只读来源");
    auto archive = Take(MapArchiveSession::Open(source), "C1 真实源图打开失败");
    Require(archive.SourceSha256() == kSourceSha, "真实源只读租约 SHA 不匹配");
    const auto capturedWtg = Read(directory / "asphyxia-source-war3map.wtg");
    const auto capturedWct = Read(directory / "asphyxia-source-war3map.wct");
    Require(capturedWtg.size() == 1984640 && Take(Sha256(capturedWtg), "WTG SHA 失败")
        == "8c7e8c4570693a0434dca9bdcd6aee384e2b8d6be5cad81a730869562e34b8ed", "捕获 WTG 身份不匹配");
    Require(capturedWct.size() == 2331 && Take(Sha256(capturedWct), "WCT SHA 失败")
        == "45717b8879f6599210eab866e11d3df76e0cbef32f1f69d9ebe3ab45a60d9426", "捕获 WCT 身份不匹配");
    Require(Take(archive.ReadMember("war3map.wtg"), "真实 WTG 成员读取失败") == capturedWtg
        && Take(archive.ReadMember("war3map.wct"), "真实 WCT 成员读取失败") == capturedWct,
        "捕获 WTG/WCT 不等于当前源图成员");
    const bool hasWts = archive.HasMember("war3map.wts");
    const auto originalWts = hasWts ? Take(archive.ReadMember("war3map.wts"), "真实 WTS 读取失败") : std::vector<std::byte>{};
    auto catalog = Take(Trigger::UiCatalog::Load(tools), "真实适配 UI 目录加载失败");
    Require(catalog.Sha256() == kCatalogSha, "真实适配 UI 目录身份变化");
    auto document = Take(Trigger::WtgWctCodec{}.Parse(capturedWtg, capturedWct, catalog), "真实 WTG/WCT 解析失败");
    auto originalEncoded = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "原 WTG/WCT 往返失败");
    Require(originalEncoded.wtg == capturedWtg && originalEncoded.wct == capturedWct, "原 WTG/WCT 不是无损往返");
    const auto original = document;
    const auto* selected = Trigger::TriggerDocumentEditor::FindTrigger(document, kConvertedTrigger);
    Require(selected && selected->name.text == "15-2-2 游侠实例退出记录_RangerCleanup"
        && selected->origin.offset == 1850190 && selected->origin.length == 399 && selected->customTextFlag == 0
        && selected->ecas.size() == 1 && selected->ecas.front() == 24033,
        "待转换真实小型 GUI 触发器身份不匹配");
    const auto selectedName = selected->name.text;
    const auto selectedCategory = selected->categoryId;
    const auto removed = ConvertedClosure(original, *selected);
    Require(!removed.ecas.empty() && removed.ecas.size() <= 32 && removed.arguments.size() <= 64,
        "所选小型 GUI 闭包超过本次转换验收范围");
    const auto originalHeader = Take(original.wct.customCode.ToBytes(), "真实头部原字节读取失败");
    Require(originalHeader.size() == 204 && Take(Sha256(originalHeader), "原头部 SHA 失败")
        == "efdfbe37a31cdf6f7c9cad4d8513acf45ce79c55b781a7ca08ba445df1c375d4"
        && std::all_of(originalHeader.begin(), originalHeader.end(), [](auto value) {
            return std::to_integer<unsigned char>(value) < 128;
        }), "真实头部不等于已核对的 204 字节 ASCII 来源");
    std::vector<Trigger::TriggerCodeEditReceipt> receipts;
    const auto apply = [&](Trigger::TriggerCodeEditPlan&& plan) {
        Require(plan.Changed(), "C1 本次编辑计划没有实际变化");
        const auto targetBlock = plan.Target();
        auto receipt = Take(TriggerCodeEditor::Apply(document, catalog, plan), "C1 密封编辑事务应用失败");
        Require(receipt.Changed() && receipt.Target().kind == targetBlock.kind
            && receipt.Target().triggerId == targetBlock.triggerId, "C1 密封收据身份不匹配");
        receipts.push_back(std::move(receipt));
    };
    auto creation = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, selectedCategory,
        std::string(kNewTriggerName)), "新建合法自定义代码骨架失败");
    const auto newTriggerId = creation.Target().triggerId;
    Require(newTriggerId && creation.Target().kind == Trigger::CodeBlockKind::CustomTrigger,
        "新建计划没有返回正式触发器身份");
    apply(std::move(creation));
    const auto* created = Trigger::TriggerDocumentEditor::FindTrigger(document, newTriggerId);
    Require(created && created->customTextFlag != 0 && created->ecas.empty(), "新建自定义代码骨架缺失");
    const auto skeleton = TriggerCodeEditor::NormalizedText(created->customText);
    Require(skeleton.find("function InitTrig_") != std::string::npos && skeleton.find("endfunction") != std::string::npos,
        "共用新建接口未生成合法 InitTrig 骨架");
    const auto customBody = skeleton + "\n// C1 中文注释：本轮新增 UTF-8 函数，原图没有 GBK 代码块。\n"
        "function AudastraC1Probe takes nothing returns integer\n"
        "    return udg_AudastraC1Acceptance\n"
        "endfunction\n";
    apply(Take(TriggerCodeEditor::PrepareText(document, catalog,
        {Trigger::CodeBlockKind::CustomTrigger, newTriggerId}, customBody), "自定义函数密封编辑准备失败"));
    const auto headerBody = TriggerCodeEditor::NormalizedText(original.wct.customCode)
        + "\n// C1 中文注释：新增全局变量，保留原 include 的编码及换行。\n"
        "globals\n    integer udg_AudastraC1Acceptance = 7\nendglobals\n";
    apply(Take(TriggerCodeEditor::PrepareText(document, catalog,
        {Trigger::CodeBlockKind::GlobalHeader, 0}, headerBody), "全局头部密封编辑准备失败"));
    Script::MapScriptInputs conversionInputs;
    conversionInputs.triggers = &document;
    conversionInputs.catalog = &catalog;
    conversionInputs.nullSafeHandleSet = std::filesystem::exists(tools / "compiler/include/YDTrigger/SaveLoadSystem/HTSystem.j");
    const auto generated = Take(Script::GenerateGuiTriggerSource(conversionInputs, kConvertedTrigger),
        "生产 GUI 转换生成器未支持所选真实触发器");
    Require(!generated.text.empty() && !generated.triggerVariable.empty() && !generated.initFunction.empty()
        && generated.text.find(generated.initFunction) != std::string::npos,
        "生产转换体缺少完整初始化函数或触发器身份");
    const auto conversionBody = TriggerCodeEditor::NormalizedText(Trigger::DecodeSourceText(
        std::as_bytes(std::span(generated.text.data(), generated.text.size()))));
    apply(Take(TriggerCodeEditor::PrepareConversion(document, catalog, kConvertedTrigger,
        conversionBody, generated.effectiveRunOnInit), "真实 GUI 转换密封计划失败"));
    VerifyUnrelatedGraph(original, document, removed, newTriggerId);
    auto editedEncoded = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "C1 编辑产物序列化失败");
    for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt)
        Check(TriggerCodeEditor::Replay(document, catalog, *receipt, false), "C1 逆序撤销失败");
    const auto undone = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "C1 撤销图序列化失败");
    Require(undone.wtg == capturedWtg && undone.wct == capturedWct, "C1 撤销没有恢复完整原 WTG/WCT 字节");
    for (const auto& receipt : receipts)
        Check(TriggerCodeEditor::Replay(document, catalog, receipt, true), "C1 正序重做失败");
    const auto redone = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog), "C1 重做图序列化失败");
    Require(redone.wtg == editedEncoded.wtg && redone.wct == editedEncoded.wct, "C1 重做没有恢复密封编辑字节");
    VerifyUnrelatedGraph(original, document, removed, newTriggerId);
    const auto editedHeader = Take(document.wct.customCode.ToBytes(), "编辑头部原编码写回失败");
    Require(editedHeader.size() > originalHeader.size() && std::equal(originalHeader.begin(), originalHeader.end(), editedHeader.begin())
        && SameRaw(original.wct.customComment, document.wct.customComment), "C1 编辑没有保留原头部字节或注释元数据");
    const auto* converted = Trigger::TriggerDocumentEditor::FindTrigger(document, kConvertedTrigger);
    Require(converted && converted->customTextFlag != 0 && converted->ecas.empty()
        && (converted->runOnInit != 0) == generated.effectiveRunOnInit
        && TriggerCodeEditor::NormalizedText(converted->customText) == conversionBody,
        "转换没有保持生产正文及初始化运行语义");
    MapSaveTask task;
    struct WorkspaceEvidence {
        const MapSaveTask& task;
        ~WorkspaceEvidence() noexcept {
            // 失败保留由服务管理的诊断目录，只报精确路径供根代理核对清理。
            try {
                if (task.scriptWorkspace.empty()) return;
                std::error_code error;
                const bool exists = std::filesystem::exists(task.scriptWorkspace, error);
                std::cerr << "script_workspace=" << task.scriptWorkspace.generic_string()
                    << "\nscript_workspace_exists=" << exists << "\nscript_workspace_probe_error=" << error.value() << '\n';
            } catch (...) {}
        }
    } workspaceEvidence{task};
    Check(task.FreezeArchive(archive), "C1 保存任务冻结失败");
    task.target = target;
    task.triggerChanges = true;
    task.requireGeneratedNativeByteEquality = true;
    task.replacements = {{"war3map.wtg", redone.wtg}, {"war3map.wct", redone.wct}};
    ScriptSaveInputSources inputs;
    inputs.layers = catalog.Layers();
    inputs.expectedEffectiveLayers = inputs.layers;
    inputs.catalog = std::make_unique<Trigger::UiCatalog>(catalog);
    inputs.triggers = std::make_unique<Trigger::TriggerDocument>(document);
    inputs.toolRoot = tools; inputs.uiRoot = tools; inputs.gameRoot = game;
    inputs.scriptMember = "war3map.j";
    inputs.documentRevision = receipts.size();
    PrepareObjects(task, inputs);
    const auto prepared = Take(PrepareScriptSaveInputs(task, std::move(inputs)), "C1 共用脚本输入准备失败");
    Require(prepared == ScriptSavePreparationState::Ready && task.script && !task.scriptSkipped,
        "C1 实际保存没有进入完整脚本重建链");
    task.beforePublish = [&archive]() { return archive.VerifySourceUnchanged(); };
    MapSaveService::Run(task, [](std::string_view stage) { std::cout << "save_stage=" << stage << '\n'; });
    Diagnostics(task.diagnostics);
    Require(task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.nextSession
        && task.written == target && task.backup.empty(), "C1 真实保存/编译/归档回读失败");
    Require(std::any_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "SCRIPT_REBUILT" && diagnostic.severity == DiagnosticSeverity::Info;
    }), "C1 没有完整保存链脚本检查回执");
    const auto savedWtg = Take(task.nextSession->ReadMember("war3map.wtg"), "候选 WTG 回读失败");
    const auto savedWct = Take(task.nextSession->ReadMember("war3map.wct"), "候选 WCT 回读失败");
    Require(savedWtg == redone.wtg && savedWct == redone.wct, "候选 WTG/WCT 不等于密封重做产物");
    auto reopened = Take(Trigger::WtgWctCodec{}.Parse(savedWtg, savedWct, catalog), "候选 WTG/WCT 重开失败");
    const auto reencoded = Take(Trigger::WtgWctCodec{}.Serialize(reopened, catalog), "候选重开后序列化失败");
    Require(reencoded.wtg == savedWtg && reencoded.wct == savedWct && reopened.triggers.size() == original.triggers.size() + 1,
        "候选重开不是精确往返或触发器数量变化");
    const auto findByName = [&](std::string_view name) -> const Trigger::TriggerRecord& {
        const auto found = std::find_if(reopened.triggers.begin(), reopened.triggers.end(),
            [name](const auto& trigger) { return trigger.name.text == name; });
        Require(found != reopened.triggers.end(), "候选代码触发器名称不存在");
        Require(std::count_if(reopened.triggers.begin(), reopened.triggers.end(),
            [name](const auto& trigger) { return trigger.name.text == name; }) == 1, "候选代码触发器名称不唯一");
        return *found;
    };
    const auto& savedCustom = findByName(kNewTriggerName);
    const auto& savedConverted = findByName(selectedName);
    Require(savedCustom.customTextFlag != 0 && savedCustom.ecas.empty()
        && TriggerCodeEditor::NormalizedText(savedCustom.customText) == customBody
        && savedConverted.customTextFlag != 0 && savedConverted.ecas.empty()
        && TriggerCodeEditor::NormalizedText(savedConverted.customText) == conversionBody
        && (savedConverted.runOnInit != 0) == generated.effectiveRunOnInit
        && Take(reopened.wct.customCode.ToBytes(), "候选全局头部字节失败") == editedHeader,
        "候选函数/头部/转换体或初始化语义回读不一致");
    Require(task.nextSession->HasMember("war3map.wts") == hasWts
        && (!hasWts || Take(task.nextSession->ReadMember("war3map.wts"), "候选 WTS 回读失败") == originalWts),
        "代码编辑保存改变了无关 WTS 成员");
    const auto script = Take(task.nextSession->ReadMember("war3map.j"), "候选完整 JASS 回读失败");
    const std::string_view finalScript(reinterpret_cast<const char*>(script.data()), script.size());
    Require(!script.empty() && finalScript.find("AudastraC1Probe") != std::string_view::npos
        && finalScript.find("udg_AudastraC1Acceptance") != std::string_view::npos,
        "实际编译脚本缺新增函数或全局变量");
    Check(archive.VerifySourceUnchanged(), "C1 真实源只读租约最终校验失败");
    Require(Take(Sha256File(source), "C1 最终原图 SHA 读取失败") == kSourceSha, "C1 原图发生变化");
    FinalizeScriptWorkspace(task);
    std::cout << "source_sha256=" << kSourceSha << "\ncatalog_sha256=" << catalog.Sha256()
        << "\nnew_trigger_id=" << newTriggerId << "\nconverted_trigger_id=" << kConvertedTrigger
        << "\nconverted_init_function=" << generated.initFunction
        << "\nconverted_effective_run_on_init=" << generated.effectiveRunOnInit
        << "\nremoved_eca_count=" << removed.ecas.size() << "\nremoved_argument_count=" << removed.arguments.size()
        << "\nsealed_receipt_count=" << receipts.size() << "\ncandidate_path=" << target.generic_string()
        << "\ncandidate_sha256=" << task.nextSession->SourceSha256()
        << "\nwtg_sha256=" << Take(Sha256(savedWtg), "WTG SHA 失败")
        << "\nwct_sha256=" << Take(Sha256(savedWct), "WCT SHA 失败")
        << "\nscript_sha256=" << Take(Sha256(script), "JASS SHA 失败")
        << "\nscript_rebuilt=true\nsource_unchanged=true\nundo_restored_original_bytes=true"
        << "\nreplay_restored_edited_bytes=true\nunrelated_gui_graph_preserved=true"
        << "\noriginal_global_code_ascii_bytes=204\nnew_chinese_comments_encoding=UTF8"
        << "\nexisting_gbk_custom_block_present=false\noriginal_editor_validated=false\nruntime_validated=false\n";
}
} // namespace

int main() {
    try {
        const auto root = EnvironmentPath(L"FORGE_TRIGGER_CODE_REAL_ROOT");
        const auto source = EnvironmentPath(L"FORGE_TRIGGER_CODE_REAL_MAP");
        const auto tools = EnvironmentPath(L"FORGE_TRIGGER_CODE_REAL_TOOL_ROOT");
        const auto game = EnvironmentPath(L"FORGE_REAL_GAME_ROOT");
        const auto repository = EnvironmentPath(L"FORGE_REAL_REPO_ROOT");
        if (root.empty() || source.empty() || tools.empty() || game.empty()) {
            std::cout << "SKIP: FORGE_TRIGGER_CODE_REAL_ROOT, FORGE_TRIGGER_CODE_REAL_MAP, "
                "FORGE_TRIGGER_CODE_REAL_TOOL_ROOT and FORGE_REAL_GAME_ROOT are required for the bound candidate-only acceptance.\n";
            return 77;
        }
        Run(root, repository, source, tools, game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TriggerCodeRealTests: " << error.what() << '\n';
        return 1;
    }
}
