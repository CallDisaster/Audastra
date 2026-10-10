#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include "../../src/Trigger/TriggerCodecDefinitionLookupMemo.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, std::string_view reason) { if (!value) throw std::runtime_error(std::string(reason)); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error("共用代码编辑操作失败。");
    }
    return std::move(*result.value);
}
void Check(Status status) { Require(status.Succeeded(), "代码重放拒绝。"); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream.write(text.data(), text.size());
    Require(stream.good(), "不能建立原创目录。");
}
EncodedTriggerDocument Wire(const TriggerDocument& document, const UiCatalog& catalog) {
    return Take(WtgWctCodec{}.Serialize(document, catalog));
}
void Same(const EncodedTriggerDocument& a, const EncodedTriggerDocument& b) {
    Require(a.wtg == b.wtg && a.wct == b.wct, "WTG/WCT 未精确恢复。");
}
// 保留改造前的独立字节布局 oracle；只用于原创夹具，不进入产品路径。
struct LegacyBindingWriter {
    std::string bytes;
    void Number(std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index) bytes.push_back(static_cast<char>(value >> (index * 8)));
    }
    void Text(std::string_view text) { Number(text.size()); bytes.append(text); }
    void Bytes(std::span<const std::byte> value) { Text({reinterpret_cast<const char*>(value.data()), value.size()}); }
    void Raw(const RawText& value) { Number(static_cast<unsigned>(value.encoding)); Text(value.text); Bytes(value.originalBytes); }
    void Origin(const OriginSpan& value) {
        const auto path = value.source.generic_u8string();
        Text({reinterpret_cast<const char*>(path.data()), path.size()});
        Number(value.offset); Number(value.length); Text(value.fingerprint);
    }
    void Record(const TriggerRecord& value) {
        Number(value.id); Raw(value.name); Raw(value.description);
        Number(value.type); Number(value.enabled); Number(value.customTextFlag);
        Number(value.initiallyClosed); Number(value.runOnInit); Number(value.categoryId);
        Number(value.ecas.size()); for (const auto index : value.ecas) Number(index);
        Raw(value.customText); Origin(value.origin);
    }
};
LegacyBindingWriter LegacyGraphBytes(const TriggerDocument& document, const UiCatalog& catalog) {
    const auto encoded = Wire(document, catalog);
    LegacyBindingWriter writer;
    writer.Number(document.HasTrustedSourceSeal()); writer.Text(document.SourceWtgSha256());
    writer.Text(document.SourceWctSha256()); writer.Text(document.UiCatalogSha256()); writer.Text(catalog.Sha256());
    writer.Bytes(encoded.wtg); writer.Bytes(encoded.wct);
    writer.Number(document.wct.version); writer.Raw(document.wct.customComment); writer.Raw(document.wct.customCode);
    writer.Number(document.categories.size());
    for (const auto& category : document.categories) { writer.Number(category.id); writer.Raw(category.name); writer.Origin(category.origin); }
    writer.Number(document.variables.size());
    for (const auto& variable : document.variables) {
        writer.Number(variable.id); writer.Raw(variable.name); writer.Raw(variable.typeName); writer.Raw(variable.value); writer.Origin(variable.origin);
    }
    writer.Number(document.triggers.size()); for (const auto& trigger : document.triggers) writer.Record(trigger);
    writer.Number(document.ecaNodes.size());
    for (const auto& eca : document.ecaNodes) {
        writer.Number(eca.id); writer.Number(eca.sourceTriggerId); writer.Raw(eca.name); writer.Origin(eca.origin);
        writer.Number(eca.arguments.size()); for (const auto index : eca.arguments) writer.Number(index);
        writer.Number(eca.children.size()); for (const auto index : eca.children) writer.Number(index);
        const auto path = eca.uiSource.generic_u8string();
        writer.Text({reinterpret_cast<const char*>(path.data()), path.size()}); writer.Text(eca.uiLayer); writer.Text(eca.uiSignature);
    }
    writer.Number(document.argumentNodes.size());
    for (const auto& argument : document.argumentNodes) {
        writer.Number(argument.id); writer.Raw(argument.value); writer.Origin(argument.origin);
        writer.Number(argument.callEca.has_value()); if (argument.callEca) writer.Number(*argument.callEca);
        writer.Number(argument.indexArgument.has_value()); if (argument.indexArgument) writer.Number(*argument.indexArgument);
    }
    return writer;
}
void LegacyGraphEquivalence(const TriggerDocument& document, const UiCatalog& catalog) {
    const auto reference = LegacyGraphBytes(document, catalog);
    const auto expected = Take(Sha256Text(reference.bytes));
    const auto identity = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
    Require(identity.Digest() == expected && identity.BytesCharged() == reference.bytes.size(), "流式图摘要必须逐字节等于旧 writer。");
    const auto encoded = Wire(document, catalog);
    Require(identity.CurrentWtgSha256() == Take(Sha256(encoded.wtg)) && identity.CurrentWctSha256() == Take(Sha256(encoded.wct)), "同次编码配对摘要。");
    Require(identity.EncodedBytes() == encoded.wtg.size() + encoded.wct.size(), "配对输入字节账。");
    Require(identity.ScratchBytes() < Sha256Stream::kBufferBytes + 16384, "小夹具的身份暂存须有界。");
}
TriggerDocument Empty(const UiCatalog& catalog) {
    auto document = Take(WtgWctCodec{}.CreateEmpty(catalog));
    Take(TriggerDirectoryEditor::AddCategory(document, "Code"));
    return document;
}
void TextEdits(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    // 原创已有 GB18030 / CRLF 块，来自编码后再解析；不冒充真图已有的编码。
    RawText original{"// 中文旧注释\r\nfunction Old takes nothing returns nothing\r\nendfunction\r\n", TextEncoding::Gb18030, {}};
    document.wct.customCode = DecodeSourceText(Take(original.ToBytes()));
    Require(document.wct.customCode.encoding == TextEncoding::Gb18030, "已有中文块应解析为 GB18030。");
    const auto baseline = Wire(document, catalog);
    auto noEdit = Take(TriggerCodeEditor::PrepareText(document, catalog, {}, TriggerCodeEditor::NormalizedText(document.wct.customCode)));
    Require(!noEdit.Changed(), "LF 展示未改不应标记 dirty。");
    auto noEditReceipt = Take(TriggerCodeEditor::Apply(document, catalog, noEdit));
    Same(baseline, Wire(document, catalog)); Check(TriggerCodeEditor::Replay(document, catalog, noEditReceipt, false));
    auto plan = Take(TriggerCodeEditor::PrepareText(document, catalog, {},
        "// 中文新注释\nfunction Old takes nothing returns nothing\nendfunction\n"));
    auto receipt = Take(TriggerCodeEditor::Apply(document, catalog, plan));
    Require(document.wct.customCode.encoding == TextEncoding::Gb18030
        && document.wct.customCode.text.find("新注释\r\n") != std::string::npos, "中文修改须维持原编码 / CRLF。");
    auto changed = Wire(document, catalog);
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, false)); Same(baseline, Wire(document, catalog));
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, true)); Same(changed, Wire(document, catalog));
    const auto current = Wire(document, catalog);
    Require(!TriggerCodeEditor::Apply(document, catalog, plan).Succeeded(), "已经应用的正文计划须 stale。");
    Same(current, Wire(document, catalog));
    document.wct.customComment.text = "changed metadata";
    Require(!TriggerCodeEditor::Replay(document, catalog, receipt, false).Succeeded(), "header 元数据变化应拒绝撤销。");
    Require(document.wct.customCode.text.find("新注释") != std::string::npos, "撤销拒绝应保留当前代码。");
}
void Rejections(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    document.wct.customCode = DecodeSourceText(std::vector<std::byte>{std::byte{0xff}});
    const auto baseline = Wire(document, catalog);
    Require(document.wct.customCode.encoding == TextEncoding::Latin1, "FF 原字节仅 Latin1 可保留。");
    Require(!TriggerCodeEditor::PrepareText(document, catalog, {}, "中文").Succeeded(), "Latin1 不可表示不能发布。");
    Require(!TriggerCodeEditor::PrepareText(document, catalog, {}, std::string("a\0b", 3)).Succeeded(), "NUL 拒绝。");
    Require(!TriggerCodeEditor::PrepareText(document, catalog, {}, std::string("\xc0\xaf", 2)).Succeeded(), "非 UTF8 编辑拒绝。");
    Same(baseline, Wire(document, catalog));
    document.wct.customCode = {std::string(3 * 1024 * 1024, ' '), TextEncoding::Utf8, {}};
    auto noChange = Take(TriggerCodeEditor::PrepareText(document, catalog, {}, document.wct.customCode.text));
    Require(!noChange.Changed(), "超编辑预算旧块未改仍能查看 / 提交。");
    Require(!TriggerCodeEditor::PrepareText(document, catalog, {}, document.wct.customCode.text + "x").Succeeded(), "真实修改执行 2 MiB 预算。");
    TriggerCodeEditPlan unsealed;
    Require(!TriggerCodeEditor::Apply(document, catalog, unsealed).Succeeded(), "空计划不能发布。");
    TriggerCodeEditReceipt emptyReceipt;
    Require(!TriggerCodeEditor::Replay(document, catalog, emptyReceipt, false).Succeeded(), "空收据不能撤销。");
}
void CreateAndCustom(const UiCatalog& catalog) {
    auto document = Empty(catalog); const auto baseline = Wire(document, catalog);
    auto plan = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, document.categories.front().categoryId, "Custom"));
    const auto id = plan.Target().triggerId;
    Require(id != 0 && plan.Changed(), "新建计划须给出最终稳定 ID。");
    auto receipt = Take(TriggerCodeEditor::Apply(document, catalog, plan));
    const auto* trigger = TriggerDocumentEditor::FindTrigger(document, id);
    Require(trigger && trigger->customTextFlag == 1 && trigger->customText.text.find("function InitTrig_Custom") != std::string::npos,
        "新建必须提供真实生产 init 名称骨架。");
    const auto created = Wire(document, catalog);
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, false)); Same(baseline, Wire(document, catalog));
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, true)); Same(created, Wire(document, catalog));
    trigger = TriggerDocumentEditor::FindTrigger(document, id);
    auto bodyPlan = Take(TriggerCodeEditor::PrepareText(document, catalog, {CodeBlockKind::CustomTrigger, id},
        trigger->customText.text + "// 作者注释\n"));
    TriggerDocumentEditor::FindTrigger(document, id)->runOnInit = 1;
    const auto stale = Wire(document, catalog);
    Require(!TriggerCodeEditor::Apply(document, catalog, bodyPlan).Succeeded(), "custom metadata stale 不能丢弃修改。");
    Same(stale, Wire(document, catalog));
}
void CreateCategoryAndCustom(const UiCatalog& catalog) {
    auto document = Take(WtgWctCodec{}.CreateEmpty(catalog));
    const auto baseline = Wire(document, catalog);
    auto plan = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, -1, "First", {},
        TriggerCodeEditor::kDefaultMaxEditedBytes, "触发器"));
    Require(document.categories.empty() && document.triggers.empty(), "准备新建不能提前发布类别。");
    auto receipt = Take(TriggerCodeEditor::Apply(document, catalog, plan));
    Require(document.categories.size() == 1 && document.categories.front().name.text == "触发器"
        && document.triggers.size() == 1 && document.triggers.front().categoryId == document.categories.front().categoryId,
        "首次创建类别与代码须一次发布。");
    const auto created = Wire(document, catalog);
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, false));
    Same(baseline, Wire(document, catalog));
    Require(document.categories.empty() && document.triggers.empty(), "一次撤销须同时移除类别和新代码。");
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, true)); Same(created, Wire(document, catalog));
    auto empty = Take(WtgWctCodec{}.CreateEmpty(catalog));
    const auto unchanged = Wire(empty, catalog);
    Require(!TriggerCodeEditor::PrepareNewCustom(empty, catalog, -1, "", {},
        TriggerCodeEditor::kDefaultMaxEditedBytes, "触发器").Succeeded(), "无效名称整体拒绝。");
    Same(unchanged, Wire(empty, catalog));
    Require(empty.categories.empty(), "正文或名称错误不能留下空类别。");
}
std::size_t Count(std::string_view text, std::string_view part) {
    std::size_t count = 0, position = 0;
    while ((position = text.find(part, position)) != std::string::npos) { ++count; position += part.size(); }
    return count;
}
// full=true：完整核验模式（Debug/测试/环境变量），撤销栈之外任何图变化都拒绝；
// full=false：默认局部核对，目标区域（记录、子树、相邻行、集合规模）在栈外被改时拒绝。
void Conversion(const UiCatalog& catalog, bool full) {
    struct RestoreVerification {
        bool previous = TriggerCodeEditor::FullReplayVerificationEnabled();
        ~RestoreVerification() { TriggerCodeEditor::SetFullReplayVerification(previous); }
    } restoreVerification;
    TriggerCodeEditor::SetFullReplayVerification(full);
    Require(TriggerCodeEditor::FullReplayVerificationEnabled() == full, "核验模式开关无效。");
    auto document = Empty(catalog);
    Check(TriggerDocumentEditor::AddVariable(document, "Data", "integer", true));
    const auto id = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Initialize"));
    Take(TriggerDocumentEditor::AppendRootEca(document, catalog, id, UiDefinitionKind::Event, "MapInitializationEvent"));
    auto action = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, id, UiDefinitionKind::Action, "SetVariable"));
    auto* eca = TriggerDocumentEditor::FindEca(document, action.rootEcaId);
    const auto leftIndex = eca->arguments.at(0), rightIndex = eca->arguments.at(1);
    const auto leftId = document.argumentNodes[leftIndex].id, rightId = document.argumentNodes[rightIndex].id;
    Check(TriggerDocumentEditor::SetScalarArgument(document, leftId, 1, "udg_Data"));
    Take(TriggerDocumentEditor::AppendArgumentCall(document, catalog, rightId, "integer", "Identity"));
    const auto callIndex = *document.argumentNodes[rightIndex].callEca;
    document.argumentNodes[document.ecaNodes[callIndex].arguments.at(0)].value.text = "42";
    ArgumentNode index; index.id = 9001; index.type = 3; index.value.text = "2";
    document.argumentNodes[leftIndex].insertIndex = 1;
    document.argumentNodes[leftIndex].indexArgument = static_cast<std::uint32_t>(document.argumentNodes.size());
    document.argumentNodes.push_back(index);
    const auto otherId = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Other"));
    Take(TriggerDocumentEditor::AppendRootEca(document, catalog, otherId, UiDefinitionKind::Action, "DoNothing"));
    auto* target = TriggerDocumentEditor::FindTrigger(document, id);
    target->description.text = "保持说明"; target->initiallyClosed = 1; target->runOnInit = 0;
    LegacyGraphEquivalence(document, catalog);
    const auto baseline = Wire(document, catalog);
    Script::MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &catalog;
    const auto before = Script::GenerateMapScript(inputs); Require(before.Succeeded(), "转换前生产生成失败。");
    auto source = Take(Script::GenerateGuiTriggerSource(inputs, id));
    Require(source.effectiveRunOnInit && source.text.find("udg_Data[2]") != std::string::npos
        && source.text.find("Identity(42)") != std::string::npos, "生产输出须保留数组索引和嵌套函数。");
    auto plan = Take(TriggerCodeEditor::PrepareConversion(document, catalog, id, source.text, source.effectiveRunOnInit));
    auto receipt = Take(TriggerCodeEditor::Apply(document, catalog, plan));
    target = TriggerDocumentEditor::FindTrigger(document, id);
    Require(target && target->description.text == "保持说明" && target->initiallyClosed == 1
        && target->runOnInit == 1 && target->customTextFlag == 1 && target->ecas.empty(), "转换必须保 metadata / 派生 map init。");
    const auto converted = Wire(document, catalog);
    LegacyGraphEquivalence(document, catalog);
    const auto after = Script::GenerateMapScript(inputs); Require(after.Succeeded(), "转换后生产生成失败。");
    const auto invocation = "call ConditionalTriggerExecute( " + source.triggerVariable + " )";
    Require(Count(before.text, invocation) == 1 && Count(after.text, invocation) == 1, "地图初始化不得丢失或重复。");
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, false)); Same(baseline, Wire(document, catalog));
    Require(document.argumentNodes.size() == 4 && document.ecaNodes.size() == 4, "撤销恢复完整 call / index 闭包。");
    Check(TriggerCodeEditor::Replay(document, catalog, receipt, true)); Same(converted, Wire(document, catalog));
    if (full) {
        TriggerDocumentEditor::FindTrigger(document, otherId)->description.text = "外部变化";
        const auto modified = Wire(document, catalog);
        Require(!TriggerCodeEditor::Replay(document, catalog, receipt, false).Succeeded(), "图变化必须拒绝 topology 收据。");
        Same(modified, Wire(document, catalog));
    } else {
        TriggerDocumentEditor::FindTrigger(document, id)->customText.text += "\n// 外部变化";
        const auto modified = Wire(document, catalog);
        Require(!TriggerCodeEditor::Replay(document, catalog, receipt, false).Succeeded(), "目标区域变化必须拒绝 topology 收据。");
        Same(modified, Wire(document, catalog));
        // 生成代码按变量名引用：栈外改了变量声明，同样拒绝。
        auto renamed = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, document.categories.front().categoryId, "Later"));
        document.variables.front().name.text = "DataRenamed";
        Require(!TriggerCodeEditor::Apply(document, catalog, renamed).Succeeded(), "变量声明变化必须拒绝代码计划。");
    }
}
void DisabledConversion(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    const auto id = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Disabled"));
    auto event = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, id, UiDefinitionKind::Event, "MapInitializationEvent"));
    Check(TriggerDocumentEditor::SetEcaEnabled(document, event.rootEcaId, false));
    Check(TriggerDocumentEditor::SetTriggerEnabled(document, id, false));
    Script::MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &catalog;
    auto source = Take(Script::GenerateGuiTriggerSource(inputs, id));
    Require(!source.text.empty() && !source.effectiveRunOnInit, "disabled trigger 仍生成正文，disabled event 不派生 init。");
    auto plan = Take(TriggerCodeEditor::PrepareConversion(document, catalog, id, source.text, source.effectiveRunOnInit));
    Take(TriggerCodeEditor::Apply(document, catalog, plan));
    Require(document.triggers.front().enabled == 0 && document.triggers.front().runOnInit == 0, "转换保持 disabled 标志。");
}
void EqualPayloadIdentityGuard(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    const auto id = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Pair"));
    const auto appended = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, id, UiDefinitionKind::Action, "SetVariable"));
    auto* eca = TriggerDocumentEditor::FindEca(document, appended.rootEcaId);
    for (const auto index : eca->arguments) { document.argumentNodes[index].type = 3; document.argumentNodes[index].value.text = "42"; }
    const auto original = Wire(document, catalog);
    const auto plan = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, document.categories.front().categoryId, "Next"));
    const auto first = eca->arguments.at(0), second = eca->arguments.at(1);
    Require(document.argumentNodes[first].id != document.argumentNodes[second].id, "原创参数须有独立 stable ID。");
    std::swap(eca->arguments[0], eca->arguments[1]);
    Same(original, Wire(document, catalog)); // 相同 payload 的重新归属不会改变文件字节。
    Require(!TriggerCodeEditor::Apply(document, catalog, plan).Succeeded(), "图身份必须覆盖相同参数的池索引归属。");
    Require(eca->arguments[0] == second && eca->arguments[1] == first, "拒绝发布须保留当前参数身份顺序。");
    Require(document.triggers.size() == 1, "stale 计划不能部分新建触发器。");
}
void GraphStreamBudgetAndMetadata(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    auto& category = document.categories.front();
    category.origin.source = std::filesystem::path(L"原创/中文 source.wtg");
    category.origin.offset = 91; category.origin.length = 25;
    document.wct.customComment = DecodeSourceText(std::vector<std::byte>{std::byte{0xff}});
    const auto original = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
    LegacyGraphEquivalence(document, catalog);
    category.origin.offset += 1;
    Require(!VerifyTriggerGraphReplayIdentity(document, catalog, original).Succeeded(), "相同文件字节的 origin 变化必须拒绝。");
    category.origin.offset -= 1;
    Require(VerifyTriggerGraphReplayIdentity(document, catalog, original).Succeeded(), "原元信息恢复后身份一致。");
    constexpr std::size_t cap = 64 * 1024 * 1024;
    const auto baseline = Take(CaptureTriggerGraphReplayIdentity(document, catalog)).BytesCharged();
    Require(baseline < cap, "原创基线需低于 cap。");
    category.origin.fingerprint.assign(cap - baseline - 1, 'q');
    Require(Take(CaptureTriggerGraphReplayIdentity(document, catalog)).BytesCharged() == cap - 1, "64MiB-1 准入。");
    category.origin.fingerprint.push_back('q');
    const auto exact = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
    Require(exact.BytesCharged() == cap && exact.ScratchBytes() < Sha256Stream::kBufferBytes + 16384, "64MiB exact 不拥有完整身份流。");
    LegacyGraphEquivalence(document, catalog);
    category.origin.fingerprint.push_back('q');
    Require(!CaptureTriggerGraphReplayIdentity(document, catalog).Succeeded(), "64MiB+1 必须拒绝完整身份。");
}
void BatchedMetadataLegacyLayoutBoundaries(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    const auto trigger = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Batched"));
    for (unsigned index = 0; index < 300; ++index)
        Take(TriggerDocumentEditor::AppendRootEca(document, catalog, trigger, UiDefinitionKind::Action, "DoNothing"));
    document.categories.front().id = (std::uint64_t{1} << 63) + 123;
    document.categories.front().origin.source = std::filesystem::path(L"原创/不同路径 queue.wtg");
    document.categories.front().origin.offset = 0x1020304;
    document.wct.customComment = DecodeSourceText(std::vector<std::byte>{std::byte{0xff}});
    const auto baseline = Wire(document, catalog);
    for (const auto size : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{8191},
        std::size_t{8192}, std::size_t{8193}, std::size_t{65535}, std::size_t{65536}, std::size_t{65537}}) {
        document.categories.front().origin.fingerprint.assign(size, 'q');
        if (size) document.categories.front().origin.fingerprint.back() = 'z';
        LegacyGraphEquivalence(document, catalog); // 旧 LE8 writer 独立全字节布局，不借优化 writer。
        Same(baseline, Wire(document, catalog)); // 仅 origin 元数据改变，实际成员仍完全相同。
        const auto identity = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
        document.categories.front().origin.fingerprint.push_back('x');
        Require(!VerifyTriggerGraphReplayIdentity(document, catalog, identity).Succeeded(),
            "短字段合批后仍必须拒绝整图同成员字节的 metadata drift");
        document.categories.front().origin.fingerprint.pop_back();
        Require(VerifyTriggerGraphReplayIdentity(document, catalog, identity).Succeeded(), "准确元数据恢复后仍可校验");
    }
}
// 保留 perf13 前的完整 WinAPI 编解码与 originalBytes 优先语义，独立于共享 UTF-8 helper。
using OracleByteVector = std::vector<std::byte>;
class LegacyRawTextValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
std::wstring LegacyRawBytesToWide(std::span<const std::byte> bytes, UINT codePage, DWORD flags) {
    if (bytes.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        codePage, flags, reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("text decode failed");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            codePage, flags, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), result.data(), size) != size) {
        throw std::runtime_error("text decode returned an inconsistent length");
    }
    return result;
}

std::string LegacyRawWideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("UTF-8 encode failed");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-8 encode returned an inconsistent length");
    }
    return result;
}

OracleByteVector LegacyRawWideToCodePage(std::wstring_view value, UINT codePage) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        codePage,
        codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : 0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        throw LegacyRawTextValidationError("text cannot be represented in its original encoding");
    }
    OracleByteVector result(static_cast<std::size_t>(size));
    if (WideCharToMultiByte(
            codePage,
            codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : 0,
            value.data(),
            static_cast<int>(value.size()),
            reinterpret_cast<char*>(result.data()),
            size,
            nullptr,
            nullptr) != size) {
        throw LegacyRawTextValidationError("text encoding returned an inconsistent length");
    }
    return result;
}

std::pair<std::string, TextEncoding> LegacyRawDecode(std::span<const std::byte> raw) {
    try {
        return {LegacyRawWideToUtf8(LegacyRawBytesToWide(raw, CP_UTF8, MB_ERR_INVALID_CHARS)), TextEncoding::Utf8};
    } catch (const std::runtime_error&) {
    }
    try {
        return {LegacyRawWideToUtf8(LegacyRawBytesToWide(raw, 54936, MB_ERR_INVALID_CHARS)), TextEncoding::Gb18030};
    } catch (const std::runtime_error&) {
    }
    std::wstring latin1;
    latin1.reserve(raw.size());
    for (const std::byte value : raw) {
        latin1.push_back(static_cast<wchar_t>(std::to_integer<unsigned char>(value)));
    }
    return {LegacyRawWideToUtf8(latin1), TextEncoding::Latin1};
}

Result<std::vector<std::byte>> LegacyRawToBytes(const RawText& value) {
    const auto& text = value.text;
    const auto encoding = value.encoding;
    const auto& originalBytes = value.originalBytes;
    try {
        if (!originalBytes.empty()) {
            const auto [decoded, ignoredEncoding] = LegacyRawDecode(originalBytes);
            (void)ignoredEncoding;
            if (decoded == text) {
                return Result<OracleByteVector>::Success(originalBytes);
            }
        }
        const std::wstring wide = LegacyRawBytesToWide(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(text.data()), text.size()),
            CP_UTF8,
            MB_ERR_INVALID_CHARS);
        switch (encoding) {
        case TextEncoding::Utf8:
        case TextEncoding::Utf8Bom:
            return Result<OracleByteVector>::Success(OracleByteVector(
                reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data() + text.size())));
        case TextEncoding::Gb18030:
            return Result<OracleByteVector>::Success(LegacyRawWideToCodePage(wide, 54936));
        case TextEncoding::Latin1: {
            OracleByteVector result;
            result.reserve(wide.size());
            for (const wchar_t character : wide) {
                if (character > 0xff) {
                    return Result<OracleByteVector>::Failure(
                        "Trigger.WtgWct", "text.latin1.unrepresentable",
                        "edited text cannot be represented as Latin-1");
                }
                result.push_back(static_cast<std::byte>(character));
            }
            return Result<OracleByteVector>::Success(std::move(result));
        }
        }
    } catch (const std::exception& error) {
        return Result<OracleByteVector>::Failure(
            "Trigger.WtgWct", "text.encode-failed", error.what());
    }
    return Result<OracleByteVector>::Failure(
        "Trigger.WtgWct", "text.unknown-encoding", "unknown text encoding");
}

OracleByteVector RawOracleBytes(std::string_view text) {
    return OracleByteVector(reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size()));
}
Result<OracleByteVector> CheckRawOracle(const RawText& value) {
    const auto before = value;
    const auto expected = LegacyRawToBytes(value);
    auto actual = value.ToBytes();
    Require(actual.Succeeded() == expected.Succeeded() && actual.value.has_value() == expected.value.has_value(),
        "UTF-8 快路径改变旧 ToBytes 成败/拥有结果。");
    if (actual.value) Require(*actual.value == *expected.value, "UTF-8 快路径改变旧编码结果字节。");
    Require(actual.diagnostics.size() == expected.diagnostics.size(), "UTF-8 快路径改变旧诊断数量。");
    for (std::size_t i = 0; i < actual.diagnostics.size(); ++i) {
        const auto& a = actual.diagnostics[i]; const auto& b = expected.diagnostics[i];
        Require(a.severity == b.severity && a.subsystem == b.subsystem && a.code == b.code && a.message == b.message
            && !a.origin && !b.origin, "UTF-8 快路径改变旧诊断身份/消息。");
    }
    Require(value.text == before.text && value.encoding == before.encoding && value.originalBytes == before.originalBytes,
        "ToBytes 不得修改 RawText 的文字、编码或原字节。");
    return actual;
}
void RawUtf8LegacyDifferential(const UiCatalog& catalog) {
    const std::array encodings{TextEncoding::Utf8, TextEncoding::Utf8Bom, TextEncoding::Gb18030,
        TextEncoding::Latin1, static_cast<TextEncoding>(255)};
    const auto bom = std::string("\xef\xbb\xbf", 3);
    const std::vector<std::string> valid{"", "ASCII\r\n", "中文", "😀", "é", std::string("a\0b", 3),
        std::string("\xc2\x80", 2), std::string("\xdf\xbf", 2), std::string("\xe0\xa0\x80", 3),
        std::string("\xed\x9f\xbf", 3), std::string("\xee\x80\x80", 3), std::string("\xef\xbf\xbf", 3),
        std::string("\xf0\x90\x80\x80", 4), std::string("\xf4\x8f\xbf\xbf", 4), bom + "BOM", bom + bom + "BOM"};
    for (const auto& text : valid) for (const auto encoding : encodings) {
        const auto bytes = RawOracleBytes(text);
        const auto actual = CheckRawOracle({text, encoding, bytes});
        if (!bytes.empty() || encoding != static_cast<TextEncoding>(255))
            Require(actual.Succeeded() && *actual.value == bytes, "合法原字节须按旧优先规则原保，包括 NUL/BOM/错编码枚举。");
        else Require(!actual.Succeeded(), "空原字节的未知枚举仍应拒绝。");
    }
    for (const auto& text : valid) {
        Require(Take(CheckRawOracle({text, TextEncoding::Utf8, {}})) == RawOracleBytes(text), "无原字节的合法 UTF-8 仍原样写出。");
        Require(Take(CheckRawOracle({text, TextEncoding::Utf8Bom, {}})) == RawOracleBytes(text), "Utf8Bom 枚举不应添加或剥离 BOM。");
    }
    const std::vector<std::string> malformed{std::string("\xc0\xaf", 2), std::string("\xc1\xbf", 2), std::string("\x80", 1),
        std::string("\xe4\xb8", 2), std::string("\xe4X\xad", 3), std::string("\xed\xa0\x80", 3), std::string("\xed\xbf\xbf", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xf5\x80\x80\x80", 4), std::string("\xfe", 1), std::string("\xff", 1),
        std::string("\xf0\x80\x80\x80", 4)};
    for (const auto& text : malformed) {
        const auto bytes = RawOracleBytes(text);
        Require(!CheckRawOracle({text, TextEncoding::Utf8, bytes}).Succeeded(), "相同但非法 UTF-8 不能借字节相等写出。");
        Require(!CheckRawOracle({text, TextEncoding::Utf8, {}}).Succeeded(), "新文字非法 UTF-8 仍走旧失败诊断。");
        const auto decoded = DecodeSourceText(bytes);
        Require(decoded.encoding != TextEncoding::Utf8, "旧 decoder 必须实际落到 GB18030/Latin1。");
        for (const auto encoding : encodings)
            Require(Take(CheckRawOracle({decoded.text, encoding, bytes})) == bytes, "非法 UTF-8 原字节仍可能按旧 GB/Latin1 解码文字原保。");
    }
    const OracleByteVector gb{std::byte{0xd6}, std::byte{0xd0}};
    const OracleByteVector latin{std::byte{0xff}};
    const auto chinese = DecodeSourceText(gb), narrow = DecodeSourceText(latin);
    Require(chinese.encoding == TextEncoding::Gb18030 && chinese.text == "中", "真正 GB18030 中文夹具不能假定编码。");
    Require(narrow.encoding == TextEncoding::Latin1 && narrow.text == "ÿ", "真正 FF Latin1 夹具必须独立识别。");
    for (const auto encoding : encodings) {
        Require(Take(CheckRawOracle({chinese.text, encoding, gb})) == gb, "错误/未知枚举不能破坏旧 GB 原字节优先级。");
        Require(Take(CheckRawOracle({narrow.text, encoding, latin})) == latin, "错误/未知枚举不能破坏旧 Latin1 原字节优先级。");
        const auto edited = CheckRawOracle({"new中文", encoding, RawOracleBytes("old")});
        if (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf8Bom)
            Require(edited.Succeeded() && *edited.value == RawOracleBytes("new中文"), "已编辑 UTF-8 应写真实新文字。");
        if (encoding == static_cast<TextEncoding>(255)) Require(!edited.Succeeded(), "保留未命中的未知枚举继续拒绝。");
    }
    Require(!CheckRawOracle({"中文", TextEncoding::Latin1, latin}).Succeeded(), "Latin1 不可表示编辑不授写出。");
    Require(Take(CheckRawOracle({"ÿ", TextEncoding::Latin1, {}})) == latin, "Latin1 可表示文字仍为实际单字节。");
    Require(Take(CheckRawOracle({"中", TextEncoding::Gb18030, {}})) == gb, "GB18030 实际写回不得换成 UTF-8。");
    Require(Take(CheckRawOracle({"changed", TextEncoding::Utf8, RawOracleBytes("old")})) == RawOracleBytes("changed"), "字节长度不同仍走旧编辑路径。");
    Require(Take(CheckRawOracle({"new", TextEncoding::Utf8, RawOracleBytes("old")})) == RawOracleBytes("new"), "同长但字节不同不得假原保。");
    Require(Take(CheckRawOracle({"BOM", TextEncoding::Utf8, RawOracleBytes(bom + "BOM")})) == RawOracleBytes("BOM"), "主动删除 BOM 按真实编辑写回。");
    Require(Take(CheckRawOracle({bom + "BOM", TextEncoding::Utf8Bom, RawOracleBytes("BOM")})) == RawOracleBytes(bom + "BOM"), "主动添加 BOM 保真实文字而非 enum 规范化。");
    for (const std::size_t size : {std::size_t{1}, std::size_t{32767}, std::size_t{32768}, std::size_t{32769}}) {
        const std::string text(size, 'A');
        Require(Take(CheckRawOracle({text, TextEncoding::Utf8, RawOracleBytes(text)})) == RawOracleBytes(text), "实际不同长度的完整输入验证/返回长度须准确。");
        Require(Take(CheckRawOracle({text, TextEncoding::Utf8, RawOracleBytes(text.substr(1))})) == RawOracleBytes(text), "长度门不能截断或越界比较原字节。");
    }
    // INT_MAX±1 的实际 vector 需 >2GiB；此组不巨配、不加测试专用产品接口，边界保源级明确守门。
    auto document = Empty(catalog);
    document.categories.front().name = DecodeSourceText(RawOracleBytes("UTF8中文😀"));
    document.wct.customComment = DecodeSourceText(RawOracleBytes(bom + "comment"));
    const auto before = Wire(document, catalog);
    LegacyGraphEquivalence(document, catalog); // 保留旧独立 LE8/整元数据 oracle 的真实调用。
    const auto savedName = document.categories.front().name;
    document.categories.front().name = {std::string("a\0b", 3), TextEncoding::Utf8, RawOracleBytes(std::string("a\0b", 3))};
    Require(Take(CheckRawOracle(document.categories.front().name)) == RawOracleBytes(std::string("a\0b", 3)), "RawText 合法 NUL 不得新增拒绝。");
    Require(!WtgWctCodec{}.Serialize(document, catalog).Succeeded(), "原 WriteCString 的 NUL 格式拒绝仍须生效。");
    document.categories.front().name = savedName;
    Same(before, Wire(document, catalog));
}
void CodecBorrowedUtf8WireAndPublicOwnership(const UiCatalog& catalog) {
    auto raw = DecodeSourceText(RawOracleBytes("UTF8中文😀"));
    const auto source = raw;
    auto owned = Take(CheckRawOracle(raw));
    Require(owned == raw.originalBytes && owned.data() != raw.originalBytes.data(),
        "公共 ToBytes 必须返回独立拥有的字节，不能泄露 Codec 私有借用。");
    owned.front() = std::byte{'Z'};
    Require(raw.text == source.text && raw.originalBytes == source.originalBytes,
        "修改公共结果不能改变来源 RawText。");
    const auto savedOwned = owned;
    raw.originalBytes.front() = std::byte{'X'};
    Require(owned == savedOwned, "修改来源原字节不能改变已返回的公共副本。");
    Require(Take(CheckRawOracle(raw)) == source.originalBytes, "来源原字节已变时仍须按当前文字走旧编辑路径。");

    auto document = Empty(catalog);
    Check(TriggerDocumentEditor::AddVariable(document, "Data", "integer"));
    const auto gui = Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "Gui"));
    const auto action = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, gui, UiDefinitionKind::Action, "SetVariable"));
    auto* eca = TriggerDocumentEditor::FindEca(document, action.rootEcaId);
    Check(TriggerDocumentEditor::SetScalarArgument(document, document.argumentNodes[eca->arguments.at(0)].id, 1, "udg_Data"));
    Check(TriggerDocumentEditor::SetScalarArgument(document, document.argumentNodes[eca->arguments.at(1)].id, 3, "42"));
    auto plan = Take(TriggerCodeEditor::PrepareNewCustom(document, catalog, document.categories.front().categoryId,
        "Custom", "// 中文😀\nfunction InitTrig_Custom takes nothing returns nothing\nendfunction\n"));
    Take(TriggerCodeEditor::Apply(document, catalog, plan));
    document.categories.front().name.text = "UTF8中文😀";
    document.wct.customComment.text = "格式与原字节";
    document.wct.customCode.text = "// 当前全局代码\r\n";
    document.triggers.front().description.text = "保留说明😀";
    const auto forEachRaw = [](TriggerDocument& value, const auto& visit) {
        visit(value.wct.customComment); visit(value.wct.customCode);
        for (auto& category : value.categories) visit(category.name);
        for (auto& variable : value.variables) { visit(variable.name); visit(variable.typeName); visit(variable.value); }
        for (auto& trigger : value.triggers) { visit(trigger.name); visit(trigger.description); visit(trigger.customText); }
        for (auto& node : value.ecaNodes) visit(node.name);
        for (auto& node : value.argumentNodes) visit(node.value);
    };
    forEachRaw(document, [](RawText& value) {
        value.encoding = TextEncoding::Utf8;
        value.originalBytes = RawOracleBytes(value.text);
    });
    const auto borrowedWire = Wire(document, catalog);
    auto owningControl = document;
    forEachRaw(owningControl, [](RawText& value) { value.originalBytes.clear(); });
    Same(borrowedWire, Wire(owningControl, catalog));
    auto reparsed = Take(WtgWctCodec{}.Parse(borrowedWire.wtg, borrowedWire.wct, catalog));
    Same(borrowedWire, Wire(reparsed, catalog));
    LegacyGraphEquivalence(document, catalog);

    // 独立小 WCT v1 布局核私有 writer；不重写 WTG/图序列化算法。
    const auto wct = [](std::span<const std::byte> code, bool globalTerminated, std::optional<bool> slotTerminated) {
        OracleByteVector bytes;
        const auto i32 = [&bytes](std::uint32_t value) {
            for (unsigned index = 0; index < 4; ++index) bytes.push_back(static_cast<std::byte>(value >> (index * 8)));
        };
        i32(1); bytes.push_back(std::byte{0});
        i32(code.empty() ? (globalTerminated ? 1U : 0U) : static_cast<std::uint32_t>(code.size() + 1));
        bytes.insert(bytes.end(), code.begin(), code.end());
        if (!code.empty() || globalTerminated) bytes.push_back(std::byte{0});
        i32(slotTerminated.has_value() ? 1U : 0U);
        if (slotTerminated) {
            i32(*slotTerminated ? 1U : 0U);
            if (*slotTerminated) bytes.push_back(std::byte{0});
        }
        return bytes;
    };
    auto emptySlot = Empty(catalog);
    auto emptyPlan = Take(TriggerCodeEditor::PrepareNewCustom(emptySlot, catalog,
        emptySlot.categories.front().categoryId, "Empty"));
    Take(TriggerCodeEditor::Apply(emptySlot, catalog, emptyPlan));
    emptySlot.triggers.front().customText = {};
    const auto emptyWire = Wire(emptySlot, catalog);
    for (const bool globalTerminated : {false, true}) for (const bool slotTerminated : {false, true}) {
        const auto originalWct = wct({}, globalTerminated, slotTerminated);
        auto parsed = Take(WtgWctCodec{}.Parse(emptyWire.wtg, originalWct, catalog));
        const auto roundtrip = Wire(parsed, catalog);
        Require(roundtrip.wtg == emptyWire.wtg && roundtrip.wct == originalWct,
            "真实 Parse 来源 seal 必须保全局/触发器空 WCT 的四种 0/1 终止组合。");
    }

    const OracleByteVector gb{std::byte{0xd6}, std::byte{0xd0}};
    const OracleByteVector latin{std::byte{0xff}};
    const std::string bom("\xef\xbb\xbf", 3);
    std::vector<RawText> cases{
        DecodeSourceText(RawOracleBytes("UTF8中文😀")), DecodeSourceText(gb), DecodeSourceText(latin),
        {bom + "BOM", TextEncoding::Utf8Bom, RawOracleBytes(bom + "BOM")},
        {"ASCII", static_cast<TextEncoding>(255), RawOracleBytes("ASCII")},
        {"edited", TextEncoding::Utf8, RawOracleBytes("original")},
        {std::string("\xc0\xaf", 2), TextEncoding::Utf8, RawOracleBytes(std::string("\xc0\xaf", 2))},
        {"changed", static_cast<TextEncoding>(255), RawOracleBytes("old")},
        {"中文", TextEncoding::Latin1, latin},
        {std::string("a\0b", 3), TextEncoding::Utf8, RawOracleBytes(std::string("a\0b", 3))}
    };
    for (const std::size_t size : {std::size_t{32767}, std::size_t{32768}, std::size_t{32769}}) {
        cases.push_back(DecodeSourceText(RawOracleBytes(std::string(size, 'A'))));
    }
    const auto single = Empty(catalog);
    const auto singleWire = Wire(single, catalog);
    for (const auto& value : cases) {
        auto current = single;
        current.wct.customCode = value;
        const auto expected = CheckRawOracle(value);
        const auto encoded = WtgWctCodec{}.Serialize(current, catalog);
        if (!expected.Succeeded()) {
            Require(!encoded.Succeeded() && !encoded.value, "私有写出不能借用旧编码门失败的正文。");
            continue;
        }
        Require(encoded.Succeeded() && encoded.value->wtg == singleWire.wtg
            && encoded.value->wct == wct(*expected.value, false, std::nullopt),
            "真实 WCT 写出必须保持 UTF8/GB/Latin/BOM/未知枚举原保及编辑路径的旧字节。");
        if (value.text.find('\0') != std::string::npos) {
            Require(!WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog).Succeeded(),
                "SizedText 的旧写入行为不授来源 Parse 内嵌 NUL 资格。");
        } else {
            auto parsed = Take(WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog));
            Same(*encoded.value, Wire(parsed, catalog));
        }
        current.categories.front().name = value;
        if (value.text.find('\0') != std::string::npos) {
            const auto rejected = WtgWctCodec{}.Serialize(current, catalog);
            Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
                && rejected.diagnostics.back().code == "wtg.serialize-failed"
                && rejected.diagnostics.back().message == "WTG/WCT string contains an embedded NUL",
                "合法 UTF8 借用仍必须在真实 CString writer 拒绝 NUL，诊断不变。");
        } else {
            const auto cstringWire = Wire(current, catalog);
            auto parsed = Take(WtgWctCodec{}.Parse(cstringWire.wtg, cstringWire.wct, catalog));
            Require(parsed.categories.front().name.originalBytes == *expected.value,
                "实际 CString 的回读原字节须等于独立旧编码结果，包括 GB/Latin/BOM/未知枚举与长度边界。");
            Same(cstringWire, Wire(parsed, catalog));
        }
    }
}
#include "TriggerCodecDiagnosticPathTests.inl"
#include "TriggerGraphPathMemoTests.inl"
#include "TriggerCodecIntegerAppendTests.inl"
#include "TriggerCodecDefinitionLookupMemoTests.inl"
}
int main() {
    // 本测试默认在完整核验模式下检查“计划/收据绑定整张图”的契约；默认局部模式由 Conversion(catalog, false) 单独覆盖。
    TriggerCodeEditor::SetFullReplayVerification(true);
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-CodeEditing-" + std::to_string(GetCurrentProcessId()));
    try {
        Require(!std::filesystem::exists(root), "原创目录已存在，拒绝覆盖。");
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nstring=0,1,1,String,string\n");
        Write(root / "ui/base/event.txt", "[MapInitializationEvent]\ntitle=\"Map init\"\n");
        Write(root / "ui/base/action.txt", "[DoNothing]\ntitle=\"Nothing\"\n[SetVariable]\ntitle=\"Set\"\n"
            "[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
        Write(root / "ui/base/call.txt", "[Identity]\ntitle=\"Identity\"\nreturns=integer\n[[.args]]\ntype=integer\n");
        auto catalog = Take(UiCatalog::Load(root));
        TextEdits(catalog); Rejections(catalog); CreateAndCustom(catalog); Conversion(catalog, true); Conversion(catalog, false); DisabledConversion(catalog);
        EqualPayloadIdentityGuard(catalog);
        CreateCategoryAndCustom(catalog);
        GraphStreamBudgetAndMetadata(catalog);
        BatchedMetadataLegacyLayoutBoundaries(catalog);
        RawUtf8LegacyDifferential(catalog);
        GraphPathMemoLegacyEquivalence(catalog);
        CodecBorrowedUtf8WireAndPublicOwnership(catalog);
        CodecLazyDiagnosticPathsPreserveAllGraphErrorsAndWire(catalog);
        CodecSingleAppendIntegersPreserveLegacyWireAndActualCapacity(catalog);
        CodecDefinitionMemoKeepsActualKeysCountsAndFullGraph(root / "definition-memo");
        std::filesystem::remove_all(root);
        std::cout << "sealed code editing, encoding, atomic replay and production conversion passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
