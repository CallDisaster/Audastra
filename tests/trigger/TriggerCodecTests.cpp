#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerEditBatch.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/JassCompiler.h"
#include "War3MapCore/Trigger/ScriptBuildService.h"
#include "War3MapCore/MapDocument.h"
#include "../MapDataFixtures.h"
#include "War3MapCore/EditorSession.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using War3Map::StableId;
using War3Map::Trigger::EncodedTriggerDocument;
using War3Map::Trigger::UiCatalog;
using War3Map::Trigger::UiDefinition;
using War3Map::Trigger::UiDefinitionKind;
using War3Map::Trigger::UiTextDocument;
using War3Map::Trigger::WtgWctCodec;

using ByteVector = std::vector<std::byte>;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void Expect(bool condition, std::string_view message) {
    if (!condition) {
        throw TestFailure(std::string(message));
    }
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

    [[nodiscard]] ByteVector Take() { return std::move(data_); }

private:
    ByteVector data_;
};

void WriteFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw TestFailure("cannot create test fixture");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw TestFailure("cannot write test fixture");
    }
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    WriteFile(path, std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

ByteVector ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw TestFailure("cannot open external round-trip fixture");
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length < 0) {
        throw TestFailure("cannot determine external fixture size");
    }
    stream.seekg(0, std::ios::beg);
    ByteVector result(static_cast<std::size_t>(length));
    if (!result.empty()) {
        stream.read(reinterpret_cast<char*>(result.data()), length);
    }
    if (!stream) {
        throw TestFailure("cannot read complete external fixture");
    }
    return result;
}

std::filesystem::path CreateFixtureRoot() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("War3MapEditor64-TriggerTests-" + std::to_string(GetCurrentProcessId()));
    if (std::filesystem::exists(root)) {
        std::filesystem::remove_all(root);
    }
    const std::filesystem::path ui = root / "ui";
    // 数字命名的库层与其它库层没有区别；测试故意不用任何产品或第三方库的
    // 固定名称，并保留一个数字层来回归“不再按编号特判”。
    WriteText(ui / "config", "base\r\n314159/ext\r\n");
    WriteText(ui / "base" / "define.txt",
        "[TriggerTypes]\r\nSyntheticType=0,0,1,Synthetic,integer\r\n"
        "[TriggerParams]\r\nPresetFootman=0,unit,hfoo,步兵\r\n"
        "OperatorEqual=0,ComparisonOperator,\"==\",WESTRING_OPERATOR_EQUAL\r\n");
    WriteText(ui / "base" / "action.txt",
        "[DoNothing]\r\ntitle=\"Do nothing\"\r\n"
        "[SetLife]\r\ntitle=\"设置生命值\"\r\n"
        "description=\"设置 ${单位} 的生命值为 ${数值}\"\r\n"
        "[[.args]]\r\ntype=unit\r\n"
        "[[.args]]\r\ntype=real\r\n"
        "[KillUnit]\r\ntitle=\"杀死单位\"\r\n"
        "description=\"杀死 ${单位}\"\r\n"
        "[[.args]]\r\ntype=unit\r\n");
    WriteText(ui / "base" / "call.txt",
        "[GetLastCreatedUnit]\r\ntitle=\"最后创建的单位\"\r\n"
        "description=\"最后创建的单位\"\r\nreturns=unit\r\n");
    const std::string extra = "\xef\xbb\xbf# retained\r\n[Metadata]\r\nvalue=\"x\"\r\n";
    WriteText(ui / "base" / "extra.txt", extra);
    WriteText(ui / "314159" / "ext" / "action.txt",
        "[LibraryAction]\r\ntitle=\"Library\"\r\n"
        "[[.args]]\r\ntype=unit\r\n");
    WriteText(ui / "314159" / "ext" / "call.txt",
        "[LibraryCall]\r\ntitle=\"Library call\"\r\nreturns=unit\r\n");
    return root;
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
    writer.I32(0);
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
    writer.CString("synthetic");
    writer.SizedText("");
    writer.I32(2);
    writer.SizedText("slot-normal");
    writer.SizedText("slot-legacy");
    return writer.Take();
}

ByteVector MakeAttachmentWtg() {
    BinaryWriter writer;
    writer.Ascii("WTG!");
    writer.I32(7);
    writer.I32(1);
    writer.I32(1);
    writer.CString("Category");
    writer.I32(0);
    writer.I32(2);
    writer.I32(0);
    writer.I32(2);

    const auto writeTriggerHeader = [&](std::string_view name, std::int32_t rootCount) {
        writer.CString(name);
        writer.CString("");
        writer.I32(0);
        writer.I32(1);
        writer.I32(0);
        writer.I32(0);
        writer.I32(0);
        writer.I32(1);
        writer.I32(rootCount);
    };
    const auto writeScalarArgument = [&](std::int32_t type, std::string_view value) {
        writer.I32(type);
        writer.CString(value);
        writer.I32(0);
        writer.I32(0);
    };
    const auto writeLibraryAction = [&](bool child) {
        writer.I32(2);
        if (child) {
            writer.I32(0);
        }
        writer.CString("LibraryAction");
        writer.I32(1);
        writeScalarArgument(3, "hfoo");
        writer.I32(0);
    };
    const auto writeDoNothing = [&](bool withLibraryChild) {
        writer.I32(2);
        writer.CString("DoNothing");
        writer.I32(1);
        writer.I32(withLibraryChild ? 1 : 0);
        if (withLibraryChild) {
            writeLibraryAction(true);
        }
    };

    writeTriggerHeader("Normal", 1);
    writeDoNothing(false);

    writeTriggerHeader("LegacyAttachments", 5);
    // root 0：普通兄弟。
    writeDoNothing(false);
    // root 1：普通祖先下挂数字层 child。
    writeDoNothing(true);
    // root 2：LibraryCall 位于 SetLife 的第 0 个参数槽。
    writer.I32(2);
    writer.CString("SetLife");
    writer.I32(1);
    writer.I32(2);
    writer.CString("");
    writer.I32(1);
    writer.I32(3);
    writer.CString("LibraryCall");
    writer.I32(1);
    writer.I32(0);
    writer.I32(0);
    writeScalarArgument(3, "420");
    writer.I32(0);
    // root 3：数字层根；root 4：用于交换序号的普通根。
    writeLibraryAction(false);
    writeDoNothing(false);
    return writer.Take();
}

ByteVector MakeAttachmentWct() {
    BinaryWriter writer;
    writer.I32(1);
    writer.CString("attachment paths");
    writer.SizedText("");
    writer.I32(2);
    writer.SizedText("slot-normal");
    writer.SizedText("slot-attachments");
    return writer.Take();
}

ByteVector MakeUnknownFunctionWtg() {
    BinaryWriter writer;
    writer.Ascii("WTG!");
    writer.I32(7);
    writer.I32(1);
    writer.I32(1);
    writer.CString("Category");
    writer.I32(0);
    writer.I32(2);
    writer.I32(0);
    writer.I32(1);
    writer.CString("Missing library trigger");
    writer.CString("");
    writer.I32(0);
    writer.I32(1);
    writer.I32(0);
    writer.I32(0);
    writer.I32(0);
    writer.I32(1);
    writer.I32(1);
    writer.I32(2);
    writer.CString("UnavailableThirdPartyAction");
    writer.I32(1);
    writer.I32(0);
    return writer.Take();
}

ByteVector MakeSingleSlotWct() {
    BinaryWriter writer;
    writer.I32(1);
    writer.CString("");
    writer.SizedText("");
    writer.I32(1);
    writer.SizedText("");
    return writer.Take();
}

bool Equal(std::span<const std::byte> left, std::span<const std::byte> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

War3Map::Trigger::TriggerDocument CloneWithoutSourceSeal(
    const War3Map::Trigger::TriggerDocument& source) {
    War3Map::Trigger::TriggerDocument clone;
    clone.categories = source.categories;
    clone.variablesHeader = source.variablesHeader;
    clone.variables = source.variables;
    clone.triggers = source.triggers;
    clone.ecaNodes = source.ecaNodes;
    clone.argumentNodes = source.argumentNodes;
    clone.wct = source.wct;
    return clone;
}

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
    return std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void TestUiCatalog(const UiCatalog& catalog) {
    Expect(catalog.Layers().size() == 2, "config layers were not retained");
    Expect(catalog.PresentSourceCount() == 7, "manifest did not include config and every present TXT");
    Expect(catalog.ParsedSourceCount() == 7, "a present UI source was silently skipped");
    Expect(catalog.Documents().size() == 6, "unexpected parsed TXT document count");
    Expect(catalog.Manifest().size() == 12, "expected/present manifest shape changed");

    const auto* action = catalog.Find(2, "DoNothing");
    Expect(action != nullptr, "base action was not cataloged");
    Expect(action->ArgumentCount() == 0, "action argument count is wrong");
    Expect(catalog.Find(2, "LibraryAction") != nullptr,
        "numeric-layer action was not cataloged");
    Expect(catalog.Find(UiDefinitionKind::Call, "LibraryCall") != nullptr,
        "numeric-layer call was not cataloged");
    const auto* define = catalog.Find(UiDefinitionKind::Define, "SyntheticType", "TriggerTypes");
    Expect(define != nullptr, "define.txt entry was not cataloged");

    const auto extra = std::find_if(
        catalog.Documents().begin(), catalog.Documents().end(), [](const UiTextDocument& document) {
            return document.Source().filename() == "extra.txt";
        });
    Expect(extra != catalog.Documents().end(), "nonstandard enabled TXT was not scanned");
    Expect(Equal(extra->OriginalBytes(), extra->SerializeLossless()),
        "UiTextDocument did not preserve BOM/line endings byte-for-byte");
}

void TestNaturalLanguagePresentation(const UiCatalog& catalog) {
    using War3Map::Trigger::ArgumentNode;
    using War3Map::Trigger::EcaNode;
    using War3Map::Trigger::PresentArgument;
    using War3Map::Trigger::PresentEca;
    using War3Map::Trigger::TriggerDocument;

    TriggerDocument document;

    ArgumentNode unit;
    unit.type = 0;
    unit.value.text = "PresetFootman";
    document.argumentNodes.push_back(std::move(unit));

    ArgumentNode life;
    life.type = 3;
    life.value.text = "420";
    document.argumentNodes.push_back(std::move(life));

    ArgumentNode nestedCall;
    nestedCall.type = 2;
    nestedCall.insertCall = 1;
    nestedCall.callEca = 1;
    document.argumentNodes.push_back(std::move(nestedCall));

    ArgumentNode array;
    array.type = 1;
    array.value.text = "Scores";
    array.insertIndex = 1;
    array.indexArgument = 4;
    document.argumentNodes.push_back(std::move(array));

    ArgumentNode index;
    index.type = 3;
    index.value.text = "2";
    document.argumentNodes.push_back(std::move(index));

    ArgumentNode operatorPreset;
    operatorPreset.type = 0;
    operatorPreset.value.text = "OperatorEqual";
    document.argumentNodes.push_back(std::move(operatorPreset));

    EcaNode setLife;
    setLife.type = 2;
    setLife.name.text = "SetLife";
    setLife.arguments = {0, 1};
    document.ecaNodes.push_back(std::move(setLife));

    EcaNode lastUnit;
    lastUnit.type = 3;
    lastUnit.name.text = "GetLastCreatedUnit";
    document.ecaNodes.push_back(std::move(lastUnit));

    EcaNode killUnit;
    killUnit.type = 2;
    killUnit.name.text = "KillUnit";
    killUnit.arguments = {2};
    document.ecaNodes.push_back(std::move(killUnit));

    EcaNode missing;
    missing.type = 2;
    missing.name.text = "UnavailableThirdPartyAction";
    document.ecaNodes.push_back(std::move(missing));

    const auto natural = PresentEca(document, 0, catalog);
    Expect(natural.definitionAvailable, "known UI definition was not used for presentation");
    Expect(natural.templateComplete, "valid UI template was reported incomplete");
    Expect(natural.text == "设置 步兵 的生命值为 420",
        "UI description was not rendered positionally");

    const auto nested = PresentEca(document, 2, catalog);
    Expect(nested.text == "杀死 最后创建的单位",
        "nested call was not rendered as natural language");
    Expect(PresentArgument(document, 3, catalog) == "Scores[2]",
        "array index was not included in the argument presentation");
    Expect(PresentArgument(document, 5, catalog) == "==",
        "unresolved WESTRING preset did not fall back to its dynamic script value");

    const auto unavailable = PresentEca(document, 3, catalog);
    Expect(!unavailable.definitionAvailable,
        "missing optional UI function was unexpectedly invented");
    Expect(unavailable.text.find("UnavailableThirdPartyAction") != std::string::npos,
        "missing definition did not fall back to the raw WTG function name");
}

void TestTriggerCodec(const UiCatalog& catalog) {
    const ByteVector wtg = MakeWtg();
    const ByteVector wct = MakeWct();
    WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, catalog);
    Expect(parsed.Succeeded(), "synthetic WTG/WCT did not parse");
    auto& document = *parsed.value;
    const auto inspection = codec.Inspect(document);
    Expect(inspection.triggerCount == 2, "trigger count is wrong");
    Expect(inspection.ecaCount == 2, "ECA count is wrong");
    Expect(inspection.customTextSlotCount == 2, "WCT slot count is wrong");

    auto encoded = codec.Serialize(document, catalog);
    Expect(encoded.Succeeded(), "unmodified document did not serialize");
    Expect(Equal(encoded.value->wtg, wtg), "WTG round-trip is not byte-identical");
    Expect(Equal(encoded.value->wct, wct), "WCT round-trip is not byte-identical");

    // global 与普通槽的两种空表示不能在原样写出时被规范化。
    BinaryWriter emptyWriter;
    emptyWriter.I32(1); emptyWriter.CString("");
    emptyWriter.I32(1); emptyWriter.CString(""); emptyWriter.I32(2);
    emptyWriter.I32(1); emptyWriter.CString(""); emptyWriter.I32(0);
    auto emptyBytes = emptyWriter.Take();
    auto emptyDoc = codec.Parse(wtg, emptyBytes, catalog);
    Expect(emptyDoc.Succeeded(), "terminated empty WCT did not parse");
    auto emptyOutput = codec.Serialize(*emptyDoc.value, catalog);
    Expect(emptyOutput.Succeeded() && emptyOutput.value->wct == emptyBytes, "empty WCT representation changed");

    // 早期版本 0：没有头注释与自定义代码段。原样往返；加了头代码后只能写成版本 1。
    BinaryWriter legacyWriter;
    legacyWriter.I32(0); legacyWriter.I32(2);
    legacyWriter.SizedText("slot-normal"); legacyWriter.SizedText("slot-legacy");
    const auto legacyBytes = legacyWriter.Take();
    auto legacy = codec.Parse(wtg, legacyBytes, catalog);
    Expect(legacy.Succeeded() && legacy.value->wct.version == War3Map::Trigger::kWctLegacyVersion, "version 0 WCT did not parse");
    auto legacyOutput = codec.Serialize(*legacy.value, catalog);
    Expect(legacyOutput.Succeeded() && legacyOutput.value->wct == legacyBytes, "version 0 WCT round-trip changed");
    legacy.value->wct.customCode.text = "// header";
    auto upgraded = codec.Serialize(*legacy.value, catalog);
    Expect(upgraded.Succeeded(), "version 0 WCT with header code did not serialize");
    auto upgradedDoc = codec.Parse(upgraded.value->wtg, upgraded.value->wct, catalog);
    Expect(upgradedDoc.Succeeded() && upgradedDoc.value->wct.version == War3Map::Trigger::kWctVersion
        && upgradedDoc.value->wct.customCode.text == "// header"
        && upgradedDoc.value->triggers[0].customText.text == "slot-normal",
        "header code upgrades the WCT to version 1 without losing trigger text");

    // 混乱之治格式（WTG 4）：分类无注释标记、变量无数组大小、触发器无类型、ECA 无子节点计数；
    // 游戏版本字段为 1。原样往返，不升级成冰封王座格式。
    BinaryWriter rocWriter;
    rocWriter.Ascii("WTG!");
    rocWriter.I32(4);
    rocWriter.I32(1); rocWriter.I32(0); rocWriter.CString("Category");
    rocWriter.I32(1); rocWriter.I32(0);
    rocWriter.I32(1);
    rocWriter.CString("RoC"); rocWriter.CString("");
    rocWriter.I32(1); rocWriter.I32(0); rocWriter.I32(0); rocWriter.I32(0); rocWriter.I32(0);
    rocWriter.I32(1); rocWriter.I32(2); rocWriter.CString("DoNothing"); rocWriter.I32(1);
    const auto rocWtg = rocWriter.Take();
    const auto rocWct = MakeSingleSlotWct();
    auto roc = codec.Parse(rocWtg, rocWct, catalog);
    Expect(roc.Succeeded() && roc.value->wtgVersion == 4 && roc.value->variablesHeader == 1, "RoC WTG did not parse");
    auto rocOutput = codec.Serialize(*roc.value, catalog);
    Expect(rocOutput.Succeeded() && rocOutput.value->wtg == rocWtg && rocOutput.value->wct == rocWct,
        "RoC WTG round-trip changed");

    std::swap(document.triggers[0], document.triggers[1]);
    auto moved = codec.Serialize(document, catalog);
    Expect(moved.Succeeded(), "moving a whole trigger failed");
    auto reparsed = codec.Parse(moved.value->wtg, moved.value->wct, catalog);
    Expect(reparsed.Succeeded(), "moved document did not reparse");
    Expect(reparsed.value->triggers[0].name.text == "Legacy", "trigger order did not move");
    Expect(reparsed.value->triggers[0].customText.text == "slot-legacy",
        "WCT custom text did not move with its trigger slot");

    // 数字层节点与其它库节点一样可局部修改，改动完整写回。
    const std::uint32_t libraryIndex = document.triggers[0].ecas[0];
    document.ecaNodes[libraryIndex].enabled = 0;
    auto modified = codec.Serialize(document, catalog);
    Expect(modified.Succeeded(), "editing a numeric-layer node was rejected");
    auto modifiedParsed = codec.Parse(modified.value->wtg, modified.value->wct, catalog);
    Expect(modifiedParsed.Succeeded() &&
        modifiedParsed.value->ecaNodes[modifiedParsed.value->triggers[0].ecas[0]].enabled == 0,
        "numeric-layer node edit did not persist");

    // 局部删除与整触发器删除都不需要额外授权。
    auto partialParsed = codec.Parse(wtg, wct, catalog);
    Expect(partialParsed.Succeeded(), "partial deletion fixture did not parse");
    auto& partial = *partialParsed.value;
    partial.triggers.back().ecas.clear();
    partial.ecaNodes.pop_back();
    partial.argumentNodes.clear();
    Expect(codec.Serialize(partial, catalog).Succeeded(),
        "deleting a numeric-layer node from its trigger was rejected");
    auto deletionParsed = codec.Parse(wtg, wct, catalog);
    Expect(deletionParsed.Succeeded(), "deletion fixture did not parse");
    auto& deletion = *deletionParsed.value;
    deletion.triggers.pop_back();
    deletion.ecaNodes.pop_back();
    deletion.argumentNodes.clear();
    auto deleted = codec.Serialize(deletion, catalog);
    Expect(deleted.Succeeded(), "whole trigger deletion was rejected");
    auto deletedParsed = codec.Parse(deleted.value->wtg, deleted.value->wct, catalog);
    Expect(deletedParsed.Succeeded() && deletedParsed.value->triggers.size() == 1,
        "deletion output did not preserve WTG/WCT slot alignment");

    // 复制公开 IR 不能复制私有 seal；即使节点 ID 与 origin 完全相同，
    // serializer 也不能信任调用方构造的无来源文档。
    auto sealedForClone = codec.Parse(wtg, wct, catalog);
    Expect(sealedForClone.Succeeded(), "unsealed-clone fixture did not parse");
    const auto noSeal = CloneWithoutSourceSeal(*sealedForClone.value);
    Expect(!noSeal.HasTrustedSourceSeal(), "unsealed clone unexpectedly retained provenance");
    const auto noSealResult = codec.Serialize(noSeal, catalog);
    Expect(!noSealResult.Succeeded(), "document without a trusted seal was serialized");
    Expect(HasDiagnostic(noSealResult, "wtg.source-seal.missing"),
        "missing seal did not report the fail-closed diagnostic");

    // 单触发器校验只检查目标记录及其子树；其他触发器的损坏仍由保存前的完整 Validate 报告。
    auto scopedParsed = codec.Parse(wtg, wct, catalog);
    Expect(scopedParsed.Succeeded(), "scoped validation fixture did not parse");
    auto& scoped = *scopedParsed.value;
    const StableId firstId = scoped.triggers[0].id, secondId = scoped.triggers[1].id;
    Expect(codec.ValidateTrigger(scoped, catalog, firstId).Succeeded() &&
        codec.ValidateTrigger(scoped, catalog, secondId).Succeeded(), "valid trigger failed scoped validation");
    scoped.ecaNodes[scoped.triggers[1].ecas[0]].name.text = "NoSuchUiFunction";
    Expect(!codec.Validate(scoped, catalog).Succeeded(), "full validation accepted an unknown UI function");
    Expect(codec.ValidateTrigger(scoped, catalog, firstId).Succeeded(),
        "scoped validation of an intact trigger was blocked by another trigger");
    Expect(HasDiagnostic(codec.ValidateTrigger(scoped, catalog, secondId), "wtg.eca.unknown-ui"),
        "scoped validation missed an unknown UI function in the target subtree");
    Expect(HasDiagnostic(codec.ValidateTrigger(scoped, catalog, 0x7777), "wtg.trigger.missing"),
        "scoped validation accepted a missing trigger");
    Expect(HasDiagnostic(codec.ValidateTrigger(noSeal, catalog, firstId), "wtg.source-seal.missing"),
        "scoped validation accepted a document without a trusted seal");
}

void TestLibraryNodesEditable(const UiCatalog& catalog) {
    using War3Map::Trigger::TriggerDocument;
    using War3Map::Trigger::TriggerDocumentEditor;

    const ByteVector wtg = MakeAttachmentWtg();
    const ByteVector wct = MakeAttachmentWct();
    WtgWctCodec codec;
    const auto parseFixture = [&]() -> TriggerDocument {
        auto parsed = codec.Parse(wtg, wct, catalog);
        Expect(parsed.Succeeded(), "library fixture did not parse");
        return std::move(*parsed.value);
    };
    const auto expectRoundTrip = [&](const TriggerDocument& document, std::string_view message) {
        const auto encoded = codec.Serialize(document, catalog);
        Expect(encoded.Succeeded(), message);
        Expect(codec.Parse(encoded.value->wtg, encoded.value->wct, catalog).Succeeded(), message);
    };

    auto source = parseFixture();
    Expect(source.HasTrustedSourceSeal(), "parsed document did not retain a trusted seal");
    expectRoundTrip(source, "unmodified library fixture did not round-trip");

    // 含数字层节点的祖先、子树与触发器元数据都按普通函数库编辑。
    auto ancestorEdit = parseFixture();
    const StableId ancestorId = ancestorEdit.ecaNodes[ancestorEdit.triggers[1].ecas[1]].id;
    Expect(TriggerDocumentEditor::SetEcaEnabled(ancestorEdit, ancestorId, false).Succeeded(),
        "SetEcaEnabled rejected an ancestor of a numeric-layer node");
    Expect(TriggerDocumentEditor::CopyEcaSubtree(ancestorEdit, ancestorId).Succeeded(),
        "CopyEcaSubtree rejected a subtree containing a numeric-layer node");
    expectRoundTrip(ancestorEdit, "ancestor edit did not round-trip");
    Expect(TriggerDocumentEditor::RemoveEcaSubtree(ancestorEdit, ancestorId).Succeeded(),
        "RemoveEcaSubtree rejected a subtree containing a numeric-layer node");
    expectRoundTrip(ancestorEdit, "subtree removal did not round-trip");

    auto triggerMetadataEdit = parseFixture();
    const StableId libraryTriggerId = triggerMetadataEdit.triggers[1].id;
    Expect(TriggerDocumentEditor::SetTriggerEnabled(
        triggerMetadataEdit, libraryTriggerId, false).Succeeded() &&
        TriggerDocumentEditor::SetTriggerRunOnInit(
            triggerMetadataEdit, libraryTriggerId, true).Succeeded() &&
        TriggerDocumentEditor::SetTriggerComment(
            triggerMetadataEdit, libraryTriggerId, "changed").Succeeded() &&
        TriggerDocumentEditor::RenameTrigger(
            triggerMetadataEdit, libraryTriggerId, "changed").Succeeded(),
        "trigger metadata edits were rejected for a trigger using a numeric-layer library");
    expectRoundTrip(triggerMetadataEdit, "trigger metadata edits did not round-trip");

    // 调整顶层顺序、在前面插入或删除兄弟节点都只改变结构，不再有挂载锁。
    auto rootSwap = parseFixture();
    std::swap(rootSwap.triggers[1].ecas[3], rootSwap.triggers[1].ecas[4]);
    std::swap(rootSwap.triggers[1].ecas[1], rootSwap.triggers[1].ecas[0]);
    expectRoundTrip(rootSwap, "reordering library roots did not round-trip");

    auto insertBefore = parseFixture();
    auto insertedBefore = TriggerDocumentEditor::AppendRootEca(
        insertBefore, catalog, insertBefore.triggers[1].id, UiDefinitionKind::Action, "DoNothing", 3);
    Expect(insertedBefore.Succeeded(), "ordinary sibling insertion failed");
    expectRoundTrip(insertBefore, "insertion before a library root did not round-trip");

    auto libraryAppend = parseFixture();
    Expect(TriggerDocumentEditor::AppendRootEca(
        libraryAppend, catalog, libraryAppend.triggers[0].id, UiDefinitionKind::Action,
        "LibraryAction").Succeeded(), "numeric-layer action could not be created");
    expectRoundTrip(libraryAppend, "created numeric-layer action did not round-trip");

    auto triggerReorder = parseFixture();
    std::swap(triggerReorder.triggers[0], triggerReorder.triggers[1]);
    const auto reordered = codec.Serialize(triggerReorder, catalog);
    Expect(reordered.Succeeded(), "whole-trigger reorder failed");
    auto reparsed = codec.Parse(reordered.value->wtg, reordered.value->wct, catalog);
    Expect(reparsed.Succeeded() &&
        reparsed.value->triggers[0].name.text == "LegacyAttachments" &&
        reparsed.value->triggers[0].customText.text == "slot-attachments",
        "whole-trigger reorder did not keep the WCT slot synchronized");
}

void TestTriggerEditingAndUndo(const UiCatalog& catalog) {
    using War3Map::Trigger::TriggerDocumentEditor;

    WtgWctCodec codec;
    auto parsed = codec.Parse(MakeWtg(), MakeWct(), catalog);
    Expect(parsed.Succeeded(), "editing fixture did not parse");
    auto& document = *parsed.value;
    document.triggers.front().customTextFlag = 0;
    document.triggers.front().customText = {};
    const StableId normalTriggerId = document.triggers.front().id;
    const std::size_t originalEcas = document.ecaNodes.size();
    const std::size_t originalArguments = document.argumentNodes.size();

    // 触发器属性编辑（U3）：启用/初始开启/初始化运行/注释。
    {
        auto* normalTrigger = TriggerDocumentEditor::FindTrigger(document, normalTriggerId);
        Expect(normalTrigger != nullptr, "FindTrigger did not locate the normal trigger");
        const std::int32_t savedEnabled = normalTrigger->enabled;
        const std::int32_t savedClosed = normalTrigger->initiallyClosed;
        const std::int32_t savedRun = normalTrigger->runOnInit;
        const std::string savedComment = normalTrigger->description.text;

        Expect(TriggerDocumentEditor::SetTriggerEnabled(
            document, normalTriggerId, false).Succeeded(), "SetTriggerEnabled failed");
        Expect(normalTrigger->enabled == 0, "enabled flag was not written");
        Expect(TriggerDocumentEditor::SetTriggerInitiallyOn(
            document, normalTriggerId, false).Succeeded(), "SetTriggerInitiallyOn failed");
        Expect(normalTrigger->initiallyClosed == 1,
            "initiallyClosed was not set for initially-off");
        Expect(TriggerDocumentEditor::SetTriggerInitiallyOn(
            document, normalTriggerId, true).Succeeded(), "SetTriggerInitiallyOn(true) failed");
        Expect(normalTrigger->initiallyClosed == 0,
            "initiallyClosed was not cleared for initially-on");
        Expect(TriggerDocumentEditor::SetTriggerRunOnInit(
            document, normalTriggerId, true).Succeeded(), "SetTriggerRunOnInit failed");
        Expect(normalTrigger->runOnInit == 1, "runOnInit flag was not written");
        Expect(TriggerDocumentEditor::SetTriggerComment(
            document, normalTriggerId, "automated comment").Succeeded(),
            "SetTriggerComment failed");
        Expect(normalTrigger->description.text == "automated comment",
            "comment text was not written");
        Expect(normalTrigger->description.originalBytes.empty(),
            "comment edit did not clear original bytes");
        Expect(!TriggerDocumentEditor::SetTriggerComment(
            document, normalTriggerId, std::string("bad\0value", 9)).Succeeded(),
            "comment with NUL was not rejected");
        Expect(!TriggerDocumentEditor::SetTriggerEnabled(
            document, normalTriggerId + 9999, true).Succeeded(),
            "property edit on a missing trigger did not fail");

        // 还原，避免影响后续断言。
        normalTrigger->enabled = savedEnabled;
        normalTrigger->initiallyClosed = savedClosed;
        normalTrigger->runOnInit = savedRun;
        normalTrigger->description.text = savedComment;
    }

    auto added = TriggerDocumentEditor::AppendRootEca(
        document,
        catalog,
        normalTriggerId,
        UiDefinitionKind::Action,
        "SetLife");
    Expect(added.Succeeded(), "ordinary action could not be appended");
    const auto* addedEca = TriggerDocumentEditor::FindEca(document, added.value->rootEcaId);
    Expect(addedEca != nullptr && addedEca->arguments.size() == 2,
        "new action did not create its UI-defined arguments");

    // 启用/禁用切换（U4）。
    const StableId addedEcaId = added.value->rootEcaId;
    Expect(TriggerDocumentEditor::SetEcaEnabled(document, addedEcaId, false).Succeeded(),
        "SetEcaEnabled(false) failed");
    Expect(addedEca->enabled == 0, "eca enabled flag was not cleared");
    Expect(TriggerDocumentEditor::SetEcaEnabled(document, addedEcaId, true).Succeeded(),
        "SetEcaEnabled(true) failed");
    Expect(addedEca->enabled == 1, "eca enabled flag was not restored");
    Expect(!TriggerDocumentEditor::SetEcaEnabled(document, addedEcaId + 9999, false).Succeeded(),
        "SetEcaEnabled on a missing node did not fail");
    const StableId unitArgument =
        document.argumentNodes[addedEca->arguments[0]].id;
    const StableId lifeArgument =
        document.argumentNodes[addedEca->arguments[1]].id;
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, unitArgument, 0, "PresetFootman").Succeeded(),
        "preset parameter edit failed");
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, lifeArgument, 3, "725").Succeeded(),
        "literal parameter edit failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "edited trigger document did not validate");
    auto encoded = codec.Serialize(document, catalog);
    Expect(encoded.Succeeded(), "edited trigger document did not serialize");
    auto sealedPatch = codec.BuildPatch(
        document,
        catalog,
        "map/war3map.wtg",
        "map/war3map.wct",
        "synthetic trigger edit");
    Expect(sealedPatch.Succeeded(),
        "edited WTG/WCT could not be sealed as one trigger transaction patch");
    auto reparsed = codec.Parse(encoded.value->wtg, encoded.value->wct, catalog);
    Expect(reparsed.Succeeded(), "edited trigger document did not reparse");

    // 撤销参数编辑后再撤销新增，模拟 QUndoStack 的严格逆序。
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, unitArgument, 3, "").Succeeded(),
        "parameter rollback fixture failed");
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, lifeArgument, 3, "").Succeeded(),
        "parameter rollback fixture failed");
    Expect(TriggerDocumentEditor::UndoAppend(document, *added.value).Succeeded(),
        "append undo failed");
    Expect(document.ecaNodes.size() == originalEcas &&
        document.argumentNodes.size() == originalArguments,
        "append undo did not restore graph sizes");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after append undo");
    auto redoConflict = document;
    redoConflict.triggers.front().id = normalTriggerId + 1;
    const std::size_t conflictEcas = redoConflict.ecaNodes.size();
    const std::size_t conflictArguments = redoConflict.argumentNodes.size();
    Expect(!TriggerDocumentEditor::RedoAppend(
        redoConflict, *added.value).Succeeded(),
        "redo unexpectedly accepted a missing attachment point");
    Expect(redoConflict.ecaNodes.size() == conflictEcas &&
        redoConflict.argumentNodes.size() == conflictArguments,
        "failed redo left a partial tail graph");
    Expect(TriggerDocumentEditor::RedoAppend(document, *added.value).Succeeded(),
        "append redo failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after append redo");

    auto kill = TriggerDocumentEditor::AppendRootEca(
        document,
        catalog,
        normalTriggerId,
        UiDefinitionKind::Action,
        "KillUnit");
    Expect(kill.Succeeded(), "call editing fixture action could not be appended");
    const auto* killEca = TriggerDocumentEditor::FindEca(document, kill.value->rootEcaId);
    const StableId callArgument =
        document.argumentNodes[killEca->arguments.front()].id;
    // 数字层 call 与其它库定义一样可插入；插入后撤销，保持后续断言的夹具形状。
    auto libraryCall = TriggerDocumentEditor::AppendArgumentCall(
        document, catalog, callArgument, "unit", "LibraryCall");
    Expect(libraryCall.Succeeded(), "AppendArgumentCall rejected a numeric-layer call");
    Expect(TriggerDocumentEditor::UndoAppend(document, *libraryCall.value).Succeeded(),
        "numeric-layer call append could not be undone");
    auto mismatchedCall = TriggerDocumentEditor::AppendArgumentCall(
        document,
        catalog,
        callArgument,
        "real",
        "GetLastCreatedUnit");
    Expect(!mismatchedCall.Succeeded(),
        "function picker accepted a mismatched return type");
    auto call = TriggerDocumentEditor::AppendArgumentCall(
        document,
        catalog,
        callArgument,
        "unit",
        "GetLastCreatedUnit");
    Expect(call.Succeeded(), "nested call could not be appended");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "nested call graph did not validate");
    Expect(TriggerDocumentEditor::UndoAppend(document, *call.value).Succeeded(),
        "nested call undo failed");
    Expect(TriggerDocumentEditor::RedoAppend(document, *call.value).Succeeded(),
        "nested call redo failed");

    // 删除子树（N1b）：KillUnit 动作带嵌套调用参数，覆盖 children/arguments/callEca 闭包。
    auto beforeRemoval = codec.Serialize(document, catalog);
    Expect(beforeRemoval.Succeeded(), "pre-removal document did not serialize");
    auto removal = TriggerDocumentEditor::RemoveEcaSubtree(document, kill.value->rootEcaId);
    Expect(removal.Succeeded(), "RemoveEcaSubtree failed on a removable subtree");
    Expect(!removal.value->removedEcas.empty() && !removal.value->removedArguments.empty(),
        "removal receipt did not snapshot the subtree");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "compacted document did not validate");
    auto compacted = codec.Serialize(document, catalog);
    Expect(compacted.Succeeded(), "compacted document did not serialize");
    Expect(compacted.value->wtg.size() < beforeRemoval.value->wtg.size(),
        "removal did not shrink the serialized WTG");
    Expect(TriggerDocumentEditor::UndoRemoveEcaSubtree(document, *removal.value).Succeeded(),
        "UndoRemoveEcaSubtree failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "restored document did not validate");
    auto restored = codec.Serialize(document, catalog);
    Expect(restored.Succeeded(), "restored document did not serialize");
    Expect(Equal(restored.value->wtg, beforeRemoval.value->wtg),
        "remove/undo round trip changed the serialized WTG");
    Expect(Equal(restored.value->wct, beforeRemoval.value->wct),
        "remove/undo round trip changed the serialized WCT");
    // 数字层根节点及其参数可按普通函数库编辑。
    const auto libraryNode = std::find_if(
        document.ecaNodes.begin(), document.ecaNodes.end(),
        [](const War3Map::Trigger::EcaNode& node) { return node.name.text == "LibraryAction"; });
    Expect(libraryNode != document.ecaNodes.end(), "fixture lost its numeric-layer node");
    const StableId libraryId = libraryNode->id; // 稳定 ID：后续压实/追加会使迭代器失效
    Expect(TriggerDocumentEditor::SetEcaEnabled(document, libraryId, false).Succeeded(),
        "SetEcaEnabled rejected a numeric-layer node");
    Expect(TriggerDocumentEditor::FindEca(document, libraryId)->enabled == 0,
        "numeric-layer enable edit did not apply");
    Expect(TriggerDocumentEditor::SetEcaEnabled(document, libraryId, true).Succeeded(),
        "numeric-layer node could not be re-enabled");
    const auto* libraryEca = TriggerDocumentEditor::FindEca(document, libraryId);
    Expect(libraryEca != nullptr && libraryEca->arguments.size() == 1,
        "numeric-layer argument fixture is invalid");
    const StableId libraryArgumentId =
        document.argumentNodes[libraryEca->arguments.front()].id;
    const std::string libraryValue =
        document.argumentNodes[libraryEca->arguments.front()].value.text;
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, libraryArgumentId, 3, "hrif").Succeeded(),
        "SetScalarArgument rejected an argument owned by a numeric-layer node");
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document, libraryArgumentId, 3, libraryValue).Succeeded(),
        "numeric-layer argument could not be restored");

    // 复制/粘贴（N6b）：克隆带嵌套调用的子树，新稳定 ID + 内部引用重映射。
    auto snapshot = TriggerDocumentEditor::CopyEcaSubtree(document, kill.value->rootEcaId);
    Expect(snapshot.Succeeded(), "CopyEcaSubtree failed");
    const auto expectAtomicPasteRejection = [&](
        const TriggerDocumentEditor::EcaSubtreeSnapshot& candidate,
        std::string_view message) {
        const std::size_t beforeEcas = document.ecaNodes.size();
        const std::size_t beforeArguments = document.argumentNodes.size();
        const auto* triggerBefore =
            TriggerDocumentEditor::FindTrigger(document, normalTriggerId);
        Expect(triggerBefore != nullptr, "atomic paste fixture lost its trigger");
        const std::vector<std::uint32_t> rootsBefore = triggerBefore->ecas;
        const auto result = TriggerDocumentEditor::PasteEcaSubtree(
            document, catalog, candidate, normalTriggerId, 0, 0);
        Expect(!result.Succeeded(), message);
        const auto* triggerAfter =
            TriggerDocumentEditor::FindTrigger(document, normalTriggerId);
        Expect(
            document.ecaNodes.size() == beforeEcas &&
                document.argumentNodes.size() == beforeArguments &&
                triggerAfter != nullptr &&
                triggerAfter->ecas == rootsBefore,
            "rejected paste changed the document");
    };

    auto missingRootSnapshot = *snapshot.value;
    missingRootSnapshot.rootOldIndex =
        War3Map::Trigger::kInvalidNodeIndex;
    expectAtomicPasteRejection(
        missingRootSnapshot,
        "PasteEcaSubtree accepted a snapshot whose root was missing");

    if (snapshot.value->ecaOldIndices.size() > 1) {
        auto duplicateIndexSnapshot = *snapshot.value;
        duplicateIndexSnapshot.ecaOldIndices[1] =
            duplicateIndexSnapshot.ecaOldIndices[0];
        expectAtomicPasteRejection(
            duplicateIndexSnapshot,
            "PasteEcaSubtree accepted duplicate old ECA indices");
    }

    auto missingReferenceSnapshot = *snapshot.value;
    Expect(!missingReferenceSnapshot.ecas.front().arguments.empty(),
        "atomic paste fixture has no argument reference");
    missingReferenceSnapshot.ecas.front().arguments.front() =
        War3Map::Trigger::kInvalidNodeIndex;
    expectAtomicPasteRejection(
        missingReferenceSnapshot,
        "PasteEcaSubtree accepted an out-of-snapshot argument reference");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "rejected malformed paste damaged the original graph");

    auto paste = TriggerDocumentEditor::PasteEcaSubtree(
        document, catalog, *snapshot.value, normalTriggerId, 0, 0);
    Expect(paste.Succeeded(), "PasteEcaSubtree failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after paste");
    Expect(paste.value->rootEcaId != kill.value->rootEcaId,
        "pasted root did not get a fresh stable id");
    // 删除原件后粘贴体仍独立可序列化（引用确实被重映射，不共享节点）。
    auto removeOriginal =
        TriggerDocumentEditor::RemoveEcaSubtree(document, kill.value->rootEcaId);
    Expect(removeOriginal.Succeeded(), "remove original after paste failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after removing original");
    Expect(codec.Serialize(document, catalog).Succeeded(),
        "pasted-only document did not serialize");
    auto pasteAgain = TriggerDocumentEditor::PasteEcaSubtree(
        document, catalog, *snapshot.value, normalTriggerId, 0, 0);
    Expect(pasteAgain.Succeeded(), "second paste failed");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after second paste");
    // 数字层子树可复制并粘贴为新节点。
    auto librarySnapshot = TriggerDocumentEditor::CopyEcaSubtree(document, libraryId);
    Expect(librarySnapshot.Succeeded(), "copy of a numeric-layer subtree was rejected");
    Expect(TriggerDocumentEditor::PasteEcaSubtree(
        document, catalog, *librarySnapshot.value, normalTriggerId, 0, 0).Succeeded(),
        "paste of a numeric-layer subtree was rejected");
    Expect(codec.Serialize(document, catalog).Succeeded(),
        "document with pasted numeric-layer subtree did not serialize");
}

void TestTriggerEditBatch(const UiCatalog& catalog) {
    using namespace War3Map::Trigger;
    const auto wtg = MakeAttachmentWtg(), wct = MakeAttachmentWct();
    const WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, catalog);
    Expect(parsed.Succeeded(), "parse batched trigger fixture");
    const auto source = *parsed.value;
    const auto targetFor = [&](StableId id) {
        auto doc = source;
        const auto* node = TriggerDocumentEditor::FindEca(doc, id);
        Expect(node != nullptr, "batch target exists");
        const auto mount = TriggerDocumentEditor::LocateEcaPlacement(source, id);
        const auto hash = War3Map::Sha256(std::span<const std::byte>(wtg).subspan(node->origin.offset, node->origin.length));
        Expect(hash.Succeeded(), "hash exact ECA source span");
        return TriggerEditEcaTarget{node->sourceTriggerId, id, mount.parentEcaId, node->type,
            node->name.text, node->enabled, node->childId, node->arguments.size(), node->children.size(), *hash.value};
    };
    StableId leaf = 0, root = 0, call = 0;
    for (const auto& node : source.ecaNodes) {
        if (node.name.text == "LibraryAction" && node.childId) leaf = node.id;
        if (node.name.text == "DoNothing" && !node.childId) root = node.id;
        if (node.type == 3) call = node.id;
    }
    Expect(leaf && root && call, "fixture contains child, root and argument call");
    TriggerEditBatch batch{std::string(source.SourceWtgSha256()), std::string(source.SourceWctSha256()), catalog.Sha256(),
        {TriggerRemoveEcaEdit{targetFor(leaf)}, TriggerSetEcaEnabledEdit{targetFor(root), false}}};
    auto candidate = source;
    auto stale = batch;
    std::get<TriggerSetEcaEnabledEdit>(stale.operations.back()).target.expectedFunction = "DifferentFunction";
    const auto rejected = ApplyTriggerEditBatch(candidate, catalog, wtg, stale);
    const auto untouched = codec.Serialize(candidate, catalog);
    Expect(!rejected.Succeeded() && untouched.Succeeded() && untouched.value->wtg == wtg && untouched.value->wct == wct,
        "late stale guard rolls back whole batch without changing WTG or WCT");
    const auto edited = ApplyTriggerEditBatch(candidate, catalog, wtg, batch);
    Expect(edited.Succeeded() && edited.value->operationCount == 2 && edited.value->removedEcaCount == 1
        && edited.value->removedArgumentCount == 1 && edited.value->enabledChangeCount == 1,
        "child deletion includes its argument and root flag shares one atomic batch");
    Expect(TriggerDocumentEditor::FindEca(candidate, leaf) == nullptr
        && TriggerDocumentEditor::FindEca(candidate, root)->enabled == 0, "batch modifies intended stable targets");
    const auto encoded = codec.Serialize(candidate, catalog);
    Expect(encoded.Succeeded() && encoded.value->wct == wct, "non-text batch preserves original WCT bytes");
    const auto reopened = codec.Parse(encoded.value->wtg, encoded.value->wct, catalog);
    Expect(reopened.Succeeded() && reopened.value->ecaNodes.size() + 1 == source.ecaNodes.size(),
        "paired candidate reopens with complete compacted graph");
    auto callBatch = batch;
    callBatch.operations = {TriggerRemoveEcaEdit{targetFor(call)}};
    candidate = source;
    Expect(!ApplyTriggerEditBatch(candidate, catalog, wtg, callBatch).Succeeded(),
        "argument.callEca cannot masquerade as children removal");
    auto wrongSpan = batch;
    std::get<TriggerRemoveEcaEdit>(wrongSpan.operations.front()).target.expectedSourceSpanSha256 = std::string(64, 'a');
    Expect(!ApplyTriggerEditBatch(candidate, catalog, wtg, wrongSpan).Succeeded(), "stale exact source span rejected");

    const auto textExpected = [&](const TriggerDocument& doc, TriggerTextSlot slot, StableId id = 0) {
        const auto page = QueryTriggerText(doc, catalog, {slot, id, 0, 65536});
        Expect(page.Succeeded(), "bounded public text query binds paired source");
        return TriggerTextExpectation{page.value->encoding, page.value->utf8Sha256, page.value->encodedSha256};
    };
    const auto guiTarget = [&](const TriggerRecord& trigger) {
        TriggerEditTriggerTarget target{trigger.id, {}, {}};
        for (const auto index : trigger.ecas) target.expectedRootIds.push_back(source.ecaNodes[index].id);
        const auto hash = War3Map::Sha256(std::span<const std::byte>(wtg).subspan(trigger.origin.offset, trigger.origin.length));
        Expect(hash.Succeeded(), "hash entire original GUI tree"); target.expectedSourceSpanSha256 = *hash.value;
        return target;
    };
    const auto life = std::find_if(source.ecaNodes.begin(), source.ecaNodes.end(), [](const auto& node) { return node.name.text == "SetLife"; });
    Expect(life != source.ecaNodes.end(), "real scalar fixture exists");
    const auto& amount = source.argumentNodes[life->arguments[1]];
    TriggerEditBatch mixed{std::string(source.SourceWtgSha256()), std::string(source.SourceWctSha256()), catalog.Sha256(), {
        TriggerSetArgumentEdit{life->sourceTriggerId, life->id, amount.id, 3, "420", "real", 3, "421.5"},
        TriggerSetGlobalCodeEdit{textExpected(source, TriggerTextSlot::GlobalCode), "//中文A\n"},
        TriggerConvertCustomTextEdit{guiTarget(source.triggers[0]), textExpected(source, TriggerTextSlot::TriggerCode, source.triggers[0].id),
            "function InitTrig_Normal takes nothing returns nothing\nendfunction\n"}}};
    candidate = source;
    const auto mixedReceipt = ApplyTriggerEditBatch(candidate, catalog, wtg, mixed);
    Expect(mixedReceipt.Succeeded() && mixedReceipt.value->argumentChangeCount == 1
        && mixedReceipt.value->convertedGuiCount == 1 && mixedReceipt.value->textChangeCount == 2,
        "argument, global WCT and explicit GUI conversion share one atomic batch");
    Expect(candidate.triggers[0].customTextFlag == 1 && candidate.triggers[0].ecas.empty()
        && candidate.triggers[1].customText.originalBytes == source.triggers[1].customText.originalBytes,
        "conversion removes the full GUI closure and preserves unrelated original WCT slot bytes");
    const auto mixedPair = codec.Serialize(candidate, catalog);
    Expect(mixedPair.Succeeded(), "mixed edits serialize through paired codec");
    const auto mixedReopen = codec.Parse(mixedPair.value->wtg, mixedPair.value->wct, catalog);
    Expect(mixedReopen.Succeeded(), "mixed pair reopens with WCT slot alignment");
    const auto firstPage = QueryTriggerText(*mixedReopen.value, catalog, {TriggerTextSlot::GlobalCode, 0, 0, 4});
    const auto nextPage = QueryTriggerText(*mixedReopen.value, catalog, {TriggerTextSlot::GlobalCode, 0, 2, 4});
    Expect(firstPage.Succeeded() && firstPage.value->text == "//" && firstPage.value->nextOffset == 2
        && nextPage.Succeeded() && nextPage.value->text == "中", "text paging never cuts a UTF-8 code point");
    Expect(!QueryTriggerText(*mixedReopen.value, catalog, {TriggerTextSlot::GlobalCode, 0, 3, 4}).Succeeded(),
        "UTF-8 continuation-byte offset refused");
    TriggerEditBatch custom{std::string(mixedReopen.value->SourceWtgSha256()), std::string(mixedReopen.value->SourceWctSha256()), catalog.Sha256(), {
        TriggerSetCustomTextEdit{mixedReopen.value->triggers[0].id,
            textExpected(*mixedReopen.value, TriggerTextSlot::TriggerCode, mixedReopen.value->triggers[0].id), "// custom edited\n"}}};
    auto customDoc = *mixedReopen.value;
    Expect(ApplyTriggerEditBatch(customDoc, catalog, mixedPair.value->wtg, custom).Succeeded(), "existing custom-text slot is editable");
    auto implicit = batch;
    implicit.operations = {TriggerSetCustomTextEdit{source.triggers[0].id,
        textExpected(source, TriggerTextSlot::TriggerCode, source.triggers[0].id), "// forbidden implicit conversion"}};
    candidate = source;
    Expect(!ApplyTriggerEditBatch(candidate, catalog, wtg, implicit).Succeeded()
        && codec.Serialize(candidate, catalog).value->wtg == wtg, "set_custom_text never silently discards a GUI tree");
    auto append = batch;
    append.operations = {TriggerAppendRootEdit{guiTarget(source.triggers[0]), UiDefinitionKind::Action, "DoNothing"}};
    candidate = source;
    Expect(ApplyTriggerEditBatch(candidate, catalog, wtg, append).Succeeded()
        && candidate.triggers[0].ecas.size() == 2 && codec.Serialize(candidate, catalog).value->wct == wct,
        "append root uses original full-tree guard and preserves WCT exactly");
    auto wrongType = mixed;
    std::get<TriggerSetArgumentEdit>(wrongType.operations[0]).expectedType = "UnknownType";
    candidate = source;
    Expect(!ApplyTriggerEditBatch(candidate, catalog, wtg, wrongType).Succeeded(), "unknown parameter type is not promoted to string");
    RawText narrow{"old", War3Map::Trigger::TextEncoding::Latin1, {}};
    candidate = source;
    candidate.wct.customCode = narrow;
    Expect(!TriggerDocumentEditor::SetGlobalCustomCode(candidate, "中文").Succeeded()
        && candidate.wct.customCode.text == "old", "unrepresentable original encoding leaves WCT untouched");
    const auto scriptRoot = std::filesystem::temp_directory_path() / ("Forge-TriggerBatch-ScriptType-" + std::to_string(GetCurrentProcessId()));
    WriteText(scriptRoot / "config", "base\n");
    WriteText(scriptRoot / "base" / "define.txt", "[TriggerTypes]\nscriptcode=0,0,0,Code,string\n");
    WriteText(scriptRoot / "base" / "action.txt", "[DoNothing]\n[SetLife]\n[[.args]]\ntype=unit\n[[.args]]\ntype=real\n"
        "[LibraryAction]\n[[.args]]\ntype=scriptcode\n");
    WriteText(scriptRoot / "base" / "call.txt", "[LibraryCall]\nreturns=unit\n");
    const auto scriptCatalog = UiCatalog::Load(scriptRoot);
    Expect(scriptCatalog.Succeeded(), "scriptcode catalog loads with explicit string base");
    auto scriptDoc = codec.Parse(wtg, wct, *scriptCatalog.value);
    Expect(scriptDoc.Succeeded(), "scriptcode pair parses with real catalog signature");
    const auto scriptOwner = std::find_if(scriptDoc.value->ecaNodes.begin(), scriptDoc.value->ecaNodes.end(),
        [](const auto& node) { return node.name.text == "LibraryAction"; });
    Expect(scriptOwner != scriptDoc.value->ecaNodes.end(), "scriptcode owner exists");
    const auto& scriptArg = scriptDoc.value->argumentNodes[scriptOwner->arguments[0]];
    TriggerEditBatch scriptBatch{std::string(scriptDoc.value->SourceWtgSha256()), std::string(scriptDoc.value->SourceWctSha256()), scriptCatalog.value->Sha256(), {
        TriggerSetArgumentEdit{scriptOwner->sourceTriggerId, scriptOwner->id, scriptArg.id, 3, "hfoo", "scriptcode", 3, "call DoNothing()"}}};
    Expect(ApplyTriggerEditBatch(*scriptDoc.value, *scriptCatalog.value, wtg, scriptBatch).Succeeded(),
        "declared scriptcode resolves through common type service without generic unknown-string fallback");
    std::filesystem::remove_all(scriptRoot);
}

void TestTriggerAddRenameRemove(const UiCatalog& catalog) {
    using War3Map::Trigger::TriggerDocumentEditor;

    WtgWctCodec codec;
    auto parsed = codec.Parse(MakeWtg(), MakeWct(), catalog);
    Expect(parsed.Succeeded(), "trigger management fixture did not parse");
    auto& document = *parsed.value;

    // 新建：生成域 ID + WCT 槽对齐（原触发器 customText 不动）。
    const std::int32_t categoryId = document.categories.front().categoryId;
    auto added = TriggerDocumentEditor::AddTrigger(document, categoryId, "新触发器");
    Expect(added.Succeeded(), "AddTrigger failed");
    Expect(document.triggers.size() == 3, "AddTrigger did not append a trigger");
    Expect(!TriggerDocumentEditor::AddTrigger(document, categoryId, "").Succeeded(),
        "empty trigger name was accepted");
    Expect(!TriggerDocumentEditor::AddTrigger(document, 0x7f7f7f7f, "x").Succeeded(),
        "trigger with a missing category was accepted");
    Expect(TriggerDocumentEditor::RenameTrigger(
        document, *added.value, "改名后").Succeeded(), "RenameTrigger failed");
    Expect(!TriggerDocumentEditor::RenameTrigger(document, *added.value, "").Succeeded(),
        "empty rename was accepted");
    auto afterAdd = codec.Serialize(document, catalog);
    if (!afterAdd.Succeeded()) {
        for (const auto& diagnostic : afterAdd.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(afterAdd.Succeeded(), "document with new trigger did not serialize");
    auto reparsed = codec.Parse(afterAdd.value->wtg, afterAdd.value->wct, catalog);
    Expect(reparsed.Succeeded() && reparsed.value->triggers.size() == 3,
        "new trigger did not survive serialize/reparse");
    Expect(reparsed.value->triggers.back().name.text == "改名后",
        "renamed trigger name did not round-trip");
    Expect(reparsed.value->triggers[0].customText.text == "slot-normal" &&
        reparsed.value->triggers[1].customText.text == "slot-legacy",
        "WCT custom text slots did not stay aligned after AddTrigger");

    // 删除新建触发器 + 撤销（字节级恢复）。
    auto removal = TriggerDocumentEditor::RemoveTrigger(document, *added.value);
    Expect(removal.Succeeded(), "RemoveTrigger failed on new trigger");
    Expect(document.triggers.size() == 2, "RemoveTrigger did not erase the record");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "document did not validate after RemoveTrigger");
    Expect(TriggerDocumentEditor::UndoRemoveTrigger(document, *removal.value).Succeeded(),
        "UndoRemoveTrigger failed");
    Expect(document.triggers.size() == 3, "undo did not restore trigger count");
    auto restoredAdd = codec.Serialize(document, catalog);
    Expect(restoredAdd.Succeeded() && Equal(restoredAdd.value->wtg, afterAdd.value->wtg) &&
        Equal(restoredAdd.value->wct, afterAdd.value->wct),
        "add/remove/undo round trip changed serialized bytes");

    // 删除使用数字层函数库的触发器：与普通触发器相同，直接可写出。
    const StableId legacyId = document.triggers[1].id;
    auto legacyRemoval = TriggerDocumentEditor::RemoveTrigger(document, legacyId);
    Expect(legacyRemoval.Succeeded(), "RemoveTrigger failed on library trigger");
    Expect(codec.Validate(document, catalog).Succeeded(),
        "Validate rejected a whole library trigger deletion");
    auto removedOutput = codec.Serialize(document, catalog);
    Expect(removedOutput.Succeeded(), "library trigger deletion did not serialize");
    auto removedParsed = codec.Parse(removedOutput.value->wtg, removedOutput.value->wct, catalog);
    Expect(removedParsed.Succeeded() &&
        removedParsed.value->triggers.size() == 2 &&
        removedParsed.value->triggers[0].customText.text == "slot-normal",
        "deletion output lost WTG/WCT slot alignment");
    Expect(TriggerDocumentEditor::UndoRemoveTrigger(
        document, *legacyRemoval.value).Succeeded(), "legacy undo failed");
    auto restoredLegacy = codec.Serialize(document, catalog);
    Expect(restoredLegacy.Succeeded() &&
        Equal(restoredLegacy.value->wtg, afterAdd.value->wtg) &&
        Equal(restoredLegacy.value->wct, afterAdd.value->wct),
        "legacy remove/undo did not restore serialized bytes");
}

void TestAtomicRename(const std::filesystem::path& fixture) {
    using namespace War3Map::Trigger;
    const auto root = fixture / "atomic-rename";
    WriteText(root / "ui/config", "base\n");
    WriteText(root / "ui/base/define.txt", "[TriggerTypes]\nunitcode=0,0,1,Unit,integer\n");
    WriteText(root / "ui/base/action.txt",
        "[UseInteger]\n[[.args]]\ntype=integer\n[UseTrigger]\n[[.args]]\ntype=trigger\n"
        "[UseString]\n[[.args]]\ntype=string\n[UseRawcode]\n[[.args]]\ntype=unitcode\n"
        "[UseUnit]\n[[.args]]\ntype=unit\n[CustomScriptCode]\n[[.args]]\ntype=string\n");
    WriteText(root / "ui/base/call.txt", "[PassInteger]\nreturns=integer\n[[.args]]\ntype=integer\n");
    const auto catalog = UiCatalog::Load(root);
    Expect(catalog.Succeeded(), "atomic rename catalog loads");
    WtgWctCodec codec;
    auto parsed = codec.CreateEmpty(*catalog.value);
    Expect(parsed.Succeeded(), "atomic rename source seal exists");
    auto& document = *parsed.value;
    CategoryRecord category; category.id = 10; category.categoryId = 1; category.name.text = "Category";
    document.categories.push_back(category);
    const auto trigger = TriggerDocumentEditor::AddTrigger(document, 1, "技能响应");
    Expect(trigger.Succeeded(), "atomic trigger created");
    Expect(TriggerDocumentEditor::AddVariable(document, "Counter", "integer").Succeeded()
        && TriggerDocumentEditor::AddVariable(document, "Scores", "integer", true).Succeeded(), "atomic variables created");
    const auto counterId = document.variables[0].id;
    const auto scoresId = document.variables[1].id;
    const auto add = [&](std::string name, std::int32_t type, std::string value) -> std::uint32_t {
        const auto appended = TriggerDocumentEditor::AppendRootEca(document, *catalog.value, *trigger.value,
            UiDefinitionKind::Action, name);
        Expect(appended.Succeeded(), "atomic fixture action appended");
        auto* node = TriggerDocumentEditor::FindEca(document, appended.value->rootEcaId);
        const auto index = node->arguments[0];
        Expect(TriggerDocumentEditor::SetScalarArgument(document, document.argumentNodes[index].id, type, value).Succeeded(),
            "atomic fixture value assigned");
        return index;
    };
    const auto array = add("UseInteger", 1, "Scores");
    ArgumentNode index; index.id = 11; index.type = 1; index.value.text = "Counter";
    document.argumentNodes[array].insertIndex = 1;
    document.argumentNodes[array].indexArgument = static_cast<std::uint32_t>(document.argumentNodes.size());
    document.argumentNodes.push_back(index);
    const auto nested = add("UseInteger", 3, "0");
    const auto call = TriggerDocumentEditor::AppendArgumentCall(document, *catalog.value,
        document.argumentNodes[nested].id, "integer", "PassInteger");
    Expect(call.Succeeded(), "nested atomic reference created");
    auto* nestedNode = TriggerDocumentEditor::FindEca(document, call.value->rootEcaId);
    const auto nestedValue = nestedNode->arguments[0];
    document.argumentNodes[nestedValue].type = 1; document.argumentNodes[nestedValue].value.text = "Counter";
    const auto alias = add("UseInteger", 1, "udg_Counter");
    const auto rawTrigger = add("UseTrigger", 1, "gg_trg_技能响应");
    const auto encodedTrigger = add("UseTrigger", 1, "gg_trg_" + TriggerDirectoryEditor::ScriptName("技能响应"));
    const auto string = add("UseString", 3, "udg_Counter gg_trg_技能响应");
    const auto rawcode = add("UseRawcode", 3, "C001");
    const auto preplaced = add("UseUnit", 1, "gg_unit_Counter_0001");
    document.argumentNodes[alias].value.originalBytes = ByteVector{std::byte{'u'}};
    auto ambiguous = document;
    Expect(TriggerDocumentEditor::AddVariable(ambiguous, "udg_Counter", "integer").Succeeded()
        && !TriggerDocumentEditor::RenameVariableAtomic(ambiguous, *catalog.value, counterId, "Renamed").Succeeded()
        && ambiguous.variables[0].name.text == "Counter" && ambiguous.argumentNodes[alias].value.text == "udg_Counter",
        "real declaration versus prefixed alias ambiguity refuses the entire mutation");
    const auto renamed = TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "计数器");
    Expect(renamed.Succeeded() && renamed.value->changedArgumentIds.size() == 3, "nested/index/alias references renamed together");
    Expect(document.variables[0].id == counterId && document.argumentNodes[alias].value.text == "计数器"
        && document.argumentNodes[alias].value.originalBytes.empty()
        && document.argumentNodes[nestedValue].value.text == "计数器"
        && document.argumentNodes[*document.argumentNodes[array].indexArgument].value.text == "计数器", "identity and physical array links retained");
    Expect(TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, scoresId, "Values").Succeeded()
        && document.argumentNodes[array].value.text == "Values", "array declaration and reference renamed");
    const auto renamedTrigger = TriggerDocumentEditor::RenameTriggerAtomic(document, *catalog.value, *trigger.value, "战斗入口");
    Expect(renamedTrigger.Succeeded() && renamedTrigger.value->changedArgumentIds.size() == 2
        && document.argumentNodes[rawTrigger].value.text == "gg_trg_战斗入口"
        && document.argumentNodes[encodedTrigger].value.text == "gg_trg_战斗入口", "actual shared Chinese symbol encoding resolves both forms");
    Expect(document.argumentNodes[string].value.text == "udg_Counter gg_trg_技能响应"
        && document.argumentNodes[rawcode].value.text == "C001"
        && document.argumentNodes[preplaced].value.text == "gg_unit_Counter_0001", "string/rawcode/other generated domain preserved");
    Expect(TriggerDocumentEditor::AddTrigger(document, 1, "Existing").Succeeded(), "trigger conflict fixture created");
    const auto before = codec.Serialize(document, *catalog.value);
    Expect(before.Succeeded() && !TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "Values").Succeeded()
        && !TriggerDocumentEditor::RenameTriggerAtomic(document, *catalog.value, *trigger.value, "Existing").Succeeded(), "declaration conflicts rejected");
    TriggerRecord colliding; colliding.id = 12; colliding.categoryId = 1; colliding.name.text = "全新入口";
    document.triggers.push_back(colliding);
    Expect(!TriggerDocumentEditor::RenameTriggerAtomic(document, *catalog.value, *trigger.value, "SafeName").Succeeded(),
        "pre-existing Chinese generated symbol collision cannot steal either reference");
    document.triggers.back().type = 1;
    Expect(TriggerDocumentEditor::RenameTriggerAtomic(document, *catalog.value, *trigger.value, "SafeName").Succeeded()
        && TriggerDocumentEditor::RenameTriggerAtomic(document, *catalog.value, *trigger.value, "战斗入口").Succeeded(),
        "metadata comments do not conflict with real trigger declarations");
    document.triggers.pop_back();
    document.argumentNodes[nestedValue].value.encoding = TextEncoding::Latin1;
    Expect(!TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "新计数器").Succeeded()
        && document.variables[0].name.text == "计数器" && document.argumentNodes[alias].value.text == "计数器", "late encoding failure leaves all earlier refs untouched");
    document.argumentNodes[nestedValue].value.encoding = TextEncoding::Utf8;
    const auto after = codec.Serialize(document, *catalog.value);
    Expect(after.Succeeded() && before.value->wtg == after.value->wtg && before.value->wct == after.value->wct, "failed atomic changes are byte identical");
    document.wct.customCode.text = "// udg_计数器 is a comment\ncall DoNothing()\n";
    Expect(!TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "FinalCounter").Succeeded(), "opaque code needs explicit audit even when apparently unrelated");
    AtomicRenameCodeAudit audit;
    audit.sourceSha256 = *War3Map::Sha256(*document.wct.customCode.ToBytes().value).value;
    audit.uiCatalogSha256 = catalog.value->Sha256();
    audit.oldName = "计数器"; audit.newName = "FinalCounter";
    audit.oldSymbol = "udg_计数器"; audit.newSymbol = "udg_FinalCounter";
    audit.reason = "Original fixture contains only an unrelated call; caller inspected it.";
    const std::string originalCode = document.wct.customCode.text;
    auto stale = audit; stale.sourceSha256[0] = stale.sourceSha256[0] == '0' ? '1' : '0';
    Expect(!TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "FinalCounter", {&stale, 1}).Succeeded(), "stale code audit rejected");
    const auto audited = TriggerDocumentEditor::RenameVariableAtomic(document, *catalog.value, counterId, "FinalCounter", {&audit, 1});
    Expect(audited.Succeeded() && audited.value->codeCoverage == AtomicRenameCodeCoverage::CallerAssertedNotRuntimeProven
        && !audited.value->externalDependenciesVerified && document.wct.customCode.text == originalCode, "explicit audit stays caller assertion; code never rewritten");
    auto codeDocument = document;
    codeDocument.wct.customCode = {};
    const auto code = TriggerDocumentEditor::AppendRootEca(codeDocument, *catalog.value, *trigger.value,
        UiDefinitionKind::Action, "CustomScriptCode");
    Expect(code.Succeeded(), "actual string-declared custom script slot added");
    const auto codeIndex = TriggerDocumentEditor::FindEca(codeDocument, code.value->rootEcaId)->arguments[0];
    codeDocument.argumentNodes[codeIndex].value.text = "call DoNothing()";
    Expect(!TriggerDocumentEditor::RenameVariableAtomic(codeDocument, *catalog.value, counterId, "AfterCode").Succeeded()
        && codeDocument.variables[0].name.text == "FinalCounter", "custom script cannot hide behind its string signature");
    auto comments = document;
    TriggerRecord note; note.id = 13; note.categoryId = 1; note.type = 1; note.name.text = "设定";
    note.name.originalBytes = *note.name.ToBytes().value; comments.triggers.push_back(note);
    note.id = 14; note.name = RawText{"设备", TextEncoding::Utf8, {}}; comments.triggers.push_back(note);
    const auto commentsBefore = codec.Serialize(comments, *catalog.value);
    const auto commentRename = TriggerDocumentEditor::RenameTriggerAtomic(comments, *catalog.value, 13, "0-7-2 说明：设定");
    const auto commentsAfter = codec.Serialize(comments, *catalog.value);
    Expect(commentRename.Succeeded() && commentRename.value->oldSymbol.empty() && commentRename.value->newSymbol.empty()
        && commentRename.value->changedArgumentIds.empty() && commentRename.value->externallyAuditedCode.empty()
        && commentsBefore.Succeeded() && commentsAfter.Succeeded() && commentsBefore.value->wct == commentsAfter.value->wct,
        "colliding Chinese comment names change only metadata and preserve all WCT bytes without code audits");
    for (const auto fault : {0, 1, 2, 3, 4}) {
        auto invalid = comments; auto* row = TriggerDocumentEditor::FindTrigger(invalid, 13);
        if (fault == 0) row->customText.text = "call DoNothing()";
        if (fault == 1) row->ecas = invalid.triggers.front().ecas;
        if (fault == 2 || fault == 4) invalid.argumentNodes[rawTrigger].value.text = "gg_trg_" +
            (fault == 2 ? row->name.text : TriggerDirectoryEditor::ScriptName(row->name.text));
        if (fault == 3) row->customTextFlag = 1;
        const auto refused = TriggerDocumentEditor::RenameTriggerAtomic(invalid, *catalog.value, 13, "Metadata");
        Expect(HasDiagnostic(refused, fault == 2 || fault == 4 ? "TRIGGER_RENAME_METADATA_REFERENCE" : "TRIGGER_RENAME_METADATA_SHAPE")
            && row->name.text == "0-7-2 说明：设定" && invalid.wct.customCode.text == originalCode,
            "mixed comments and typed incoming edges are rejected without editing metadata or global code");
    }
    auto disabled = comments; TriggerDocumentEditor::FindTrigger(disabled, *trigger.value)->enabled = 0;
    Expect(HasDiagnostic(TriggerDocumentEditor::RenameTriggerAtomic(disabled, *catalog.value, *trigger.value, "DisabledName"),
        "TRIGGER_RENAME_CODE_AUDIT_REQUIRED"), "disabled ordinary trigger still requires opaque code audit");
}

void TestJassCompiler() {
    using namespace War3Map::Trigger;
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "jass-fixture";
    fs::create_directories(root / "ui" / "base");
    WriteText(root / "ui" / "config", "base\r\n");
    WriteText(root / "ui" / "base" / "define.txt",
        "[TriggerTypes]\r\nunitcode=0,1,1,单位类型,integer\r\n"
        "[TriggerParams]\r\nFootman=0,unitcode,'hfoo',步兵\r\nPresetInteger=0,integer,7,七\r\n"
        "PresetPlayer=0,player,Player(0),玩家一\r\n");
    WriteText(root / "ui" / "base" / "event.txt",
        "[TimerEvent]\r\nscript_name=TriggerRegisterTimerEvent\r\n"
        "[[.args]]\r\ntype=real\r\n[[.args]]\r\ntype=boolean\r\n");
    WriteText(root / "ui" / "base" / "condition.txt",
        "[IsUnitAlive]\r\nreturns=boolean\r\n[[.args]]\r\ntype=unit\r\n"
        "[AndMultiple]\r\nreturns=boolean\r\n[OrMultiple]\r\nreturns=boolean\r\n");
    WriteText(root / "ui" / "base" / "action.txt",
        "[IfThenElseMultiple]\r\n[ForLoopMultiple]\r\n[ForGroupMultiple]\r\n"
        "[KillUnit]\r\n[[.args]]\r\ntype=unit\r\n"
        "[Display]\r\nscript_name=DisplayText\r\n[[.args]]\r\ntype=string\r\n"
        "[Spawn]\r\n[[.args]]\r\ntype=unitcode\r\n"
        "[Record]\r\n[[.args]]\r\ntype=integer\r\n"
        "[RecordReal]\r\n[[.args]]\r\ntype=real\r\n"
        "[CustomScriptCode]\r\n[[.args]]\r\ntype=string\r\n"
        "[ReturnAction]\r\n[WaitForCondition]\r\n[[.args]]\r\ntype=boolexpr\r\n[[.args]]\r\ntype=real\r\n");
    std::string callDefinitions =
        "[GetLastCreated]\r\nreturns=unit\r\nscript_name=GetLastCreatedUnit\r\n"
        "[GetMirrorUnit]\r\nreturns=unit\r\n[[.args]]\r\ntype=unit\r\n"
        "[OperatorString]\r\nreturns=string\r\n[[.args]]\r\ntype=string\r\n[[.args]]\r\ntype=string\r\n"
        "[YDWEOperatorString3]\r\nreturns=string\r\n[[.args]]\r\ntype=string\r\n[[.args]]\r\ntype=string\r\n[[.args]]\r\ntype=string\r\n";
    for (const std::string prefix : {"OperatorInteger", "OperatorReal", "OperatorDegree", "OperatorRadian"}) {
        const std::string type = prefix == "OperatorInteger" ? "integer" : "real";
        for (const std::string operation : {"Add", "Subtract", "Multiply", "Divide"}) {
            callDefinitions += "[" + prefix + operation + "]\r\nreturns=" + type +
                "\r\n[[.args]]\r\ntype=" + type + "\r\n[[.args]]\r\ntype=" + type + "\r\n";
        }
    }
    WriteText(root / "ui" / "base" / "call.txt", callDefinitions);
    auto loaded = UiCatalog::Load(root);
    Expect(loaded.Succeeded(), "jass fixture catalog did not load");
    TriggerDocument document;
    TriggerRecord trigger;
    trigger.id = 1;
    trigger.name.text = "Demo";
    trigger.ecas = {0, 1, 5, 6, 7, 9};
    trigger.initiallyClosed = 1;
    trigger.runOnInit = 1;
    document.triggers.push_back(trigger);
    const auto variable = [&](std::string name, std::string type, bool array = false) {
        VariableRecord value;
        value.name.text = std::move(name);
        value.typeName.text = std::move(type);
        value.isArray = array ? 1 : 0;
        document.variables.push_back(std::move(value));
    };
    variable("Unit", "unit");
    variable("Units", "unit", true);
    variable("Count", "integer");
    document.variables.back().hasDefault = 1;
    document.variables.back().value.text = "7";
    const auto node = [&](std::int32_t type, std::string name, std::vector<std::uint32_t> arguments = {},
                          std::vector<std::uint32_t> children = {}, std::optional<std::int32_t> branch = {}) {
        EcaNode value;
        value.id = 11 + document.ecaNodes.size();
        value.sourceTriggerId = 1;
        value.type = type;
        value.name.text = std::move(name);
        value.arguments = std::move(arguments);
        value.children = std::move(children);
        value.childId = branch;
        document.ecaNodes.push_back(std::move(value));
    };
    node(1, "IsUnitAlive", {0}); // 0
    node(2, "IfThenElseMultiple", {}, {2, 3, 4});
    node(1, "IsUnitAlive", {5}, {}, 0);
    node(2, "KillUnit", {3}, {}, 1);
    node(2, "KillUnit", {0}, {}, 2);
    node(0, "TimerEvent", {6, 7});
    node(2, "Display", {1});
    node(2, "Spawn", {4});
    node(3, "GetLastCreated");
    node(2, "Record", {2});
    const auto argument = [&](std::int32_t type, std::string text) {
        ArgumentNode value;
        value.id = 100 + document.argumentNodes.size();
        value.type = type;
        value.value.text = std::move(text);
        document.argumentNodes.push_back(std::move(value));
    };
    argument(1, "Unit");
    argument(3, "消息 \"hello\"\nC:\\Maps\\demo");
    argument(3, "7");
    argument(2, "GetLastCreated");
    document.argumentNodes.back().insertCall = 1;
    document.argumentNodes.back().callEca = 8;
    argument(3, "hfoo");
    argument(1, "Units");
    document.argumentNodes.back().insertIndex = 1;
    document.argumentNodes.back().indexArgument = 2;
    argument(3, "0.5");
    argument(3, "false");
    const auto compiled = CompileTriggersToJass(document, *loaded.value);
    for (const auto& diagnostic : compiled.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Expect(compiled.Succeeded() && !compiled.jassCode.empty(), "safe JASS fragment failed");
    const auto contains = [&](const std::string& text) { return compiled.jassCode.find(text) != std::string::npos; };
    Expect(contains("globals\n") && contains("endglobals\n"), "JASS globals were not delimited");
    Expect(contains("integer udg_Count = 7"), "typed global initializer missing");
    Expect(contains("if (IsUnitAlive(udg_Units[7])) then"), "typed array/condition rendering is wrong");
    Expect(contains("call KillUnit(GetLastCreatedUnit())"), "nested call script name missing");
    Expect(contains("call Spawn('hfoo')"), "rawcode was not quoted");
    Expect(contains("call DisplayText(\"消息 \\\"hello\\\"\\nC:\\\\Maps\\\\demo\")"), "string escaping is wrong");
    Expect(contains("call TriggerRegisterTimerEvent(gg_trg_Demo, 0.5, false)"), "event registration arguments wrong");
    Expect(contains("call DisableTrigger(gg_trg_Demo)") && contains("call ConditionalTriggerExecute(gg_trg_Demo)"),
        "trigger initial flags lost");
    Expect(contains("else\n") && contains("endif\n"), "if branches were not structured");
    auto leadingZero = document;
    leadingZero.argumentNodes[2].value.text = "008";
    const auto decimal = CompileTriggersToJass(leadingZero, *loaded.value);
    Expect(decimal.Succeeded() && decimal.jassCode.find("call Record(8)") != std::string::npos,
        "decimal integer input was emitted as ambiguous leading-zero JASS literal");

    const auto rejected = [&](const TriggerDocument& invalid, std::string_view code) {
        const auto result = CompileTriggersToJass(invalid, *loaded.value);
        Expect(!result.Succeeded() && result.jassCode.empty(), "invalid semantics leaked a successful or partial JASS fragment");
        Expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) {
            return item.code == code && item.severity == War3Map::DiagnosticSeverity::Error;
        }), std::string("expected diagnostic missing: ") + std::string(code));
    };
    auto invalid = document;
    invalid.ecaNodes[1].name.text = "ForLoopMultiple";
    rejected(invalid, "jass.structure-unsupported");
    invalid.ecaNodes[1].name.text = "ForGroupMultiple";
    invalid.ecaNodes[1].children.clear();
    rejected(invalid, "jass.structure-unsupported");
    for (const auto index : {0U, 5U, 8U}) {
        invalid = document;
        invalid.ecaNodes[index].name.text = "Unregistered";
        rejected(invalid, "jass.unknown-function");
    }
    invalid = document;
    invalid.triggers[0].ecas.push_back(999);
    rejected(invalid, "jass.eca-index-invalid");
    invalid = document;
    invalid.ecaNodes[6].arguments[0] = 999;
    rejected(invalid, "jass.arg-index-invalid");
    invalid = document;
    invalid.ecaNodes[9].arguments.clear();
    rejected(invalid, "jass.argument-count-invalid");
    invalid = document;
    invalid.argumentNodes[2].value.text = "7\ncall DoSomething()";
    rejected(invalid, "jass.literal-invalid");
    invalid = document;
    invalid.argumentNodes[0].value.text = "gg_unit_hfoo_0000";
    rejected(invalid, "jass.variable-unresolved");
    invalid = document;
    invalid.argumentNodes[3].callEca = 999;
    rejected(invalid, "jass.eca-index-invalid");
    invalid = document;
    invalid.argumentNodes[3].insertCall = 0;
    rejected(invalid, "jass.argument-shape-invalid");
    invalid = document;
    invalid.ecaNodes[8].name.text = "GetMirrorUnit";
    invalid.ecaNodes[8].arguments = {3};
    rejected(invalid, "jass.graph-cycle");
    invalid = document;
    invalid.variables[1].typeName.text = "integer";
    invalid.argumentNodes[2] = invalid.argumentNodes[5];
    rejected(invalid, "jass.graph-cycle");
    invalid = document;
    invalid.ecaNodes[2].childId.reset();
    rejected(invalid, "jass.branch-invalid");
    invalid = document;
    invalid.triggers[0].customTextFlag = 1;
    rejected(invalid, "jass.custom-code-unsupported");
    invalid = document;
    invalid.ecaNodes[6].name.text = "CustomScriptCode";
    rejected(invalid, "jass.custom-code-unsupported");
    invalid = document;
    invalid.ecaNodes[9].name.text = "ReturnAction";
    invalid.ecaNodes[9].arguments.clear();
    rejected(invalid, "jass.return-unsupported");
    invalid = document;
    invalid.triggers[0].ecas = {6};
    invalid.ecaNodes[6].arguments = {3};
    invalid.ecaNodes[8].name.text = "OperatorString";
    invalid.ecaNodes[8].arguments = {1, 1};
    rejected(invalid, "jass.operator-unsupported");
    invalid.ecaNodes[8].name.text = "YDWEOperatorString3";
    invalid.ecaNodes[8].arguments = {1, 1, 1};
    rejected(invalid, "jass.operator-unsupported");
    for (const std::string prefix : {"OperatorInteger", "OperatorReal", "OperatorDegree", "OperatorRadian"}) {
        for (const std::string operation : {"Add", "Subtract", "Multiply", "Divide"}) {
            invalid = document;
            invalid.triggers[0].ecas = {9};
            invalid.ecaNodes[9].name.text = prefix == "OperatorInteger" ? "Record" : "RecordReal";
            invalid.ecaNodes[9].arguments = {3};
            invalid.ecaNodes[8].name.text = prefix + operation;
            invalid.ecaNodes[8].arguments = {2, 2};
            rejected(invalid, "jass.operator-unsupported");
        }
    }
    invalid = document;
    invalid.triggers[0].ecas = {9};
    invalid.ecaNodes[9].name.text = "WaitForCondition";
    invalid.ecaNodes[9].arguments = {0, 6};
    invalid.argumentNodes[0].type = 3;
    invalid.argumentNodes[0].value.text = "null";
    rejected(invalid, "jass.structure-unsupported");
    invalid = document;
    invalid.variables[2].typeName.text.clear();
    rejected(invalid, "jass.variable-type-invalid");
    invalid = document;
    invalid.argumentNodes[4].value.text = "x'\nx";
    rejected(invalid, "jass.rawcode-invalid");
    invalid = document;
    invalid.argumentNodes[4].type = 0;
    invalid.argumentNodes[4].value.text = "PresetPlayer";
    rejected(invalid, "jass.preset-type-invalid");
    invalid = document;
    invalid.argumentNodes[4].type = 0;
    invalid.argumentNodes[4].value.text = "Footman";
    Expect(CompileTriggersToJass(invalid, *loaded.value).Succeeded(), "typed object preset did not resolve");
    invalid = document;
    invalid.ecaNodes[0].name.text = "OrMultiple";
    invalid.ecaNodes[0].arguments.clear();
    const auto emptyOr = CompileTriggersToJass(invalid, *loaded.value);
    Expect(emptyOr.Succeeded() && emptyOr.jassCode.find("return (false)") != std::string::npos,
        "empty OR condition was incorrectly compiled as true");
    invalid = document;
    invalid.argumentNodes[1].value.text = std::string("bad\0string", 10);
    rejected(invalid, "jass.string-control-character");
    invalid = document;
    invalid.ecaNodes[7].arguments = {3};
    rejected(invalid, "jass.return-type-invalid");
    invalid = document;
    invalid.variables[1].hasDefault = 1;
    invalid.variables[1].value.text = "null";
    rejected(invalid, "jass.array-initialization-unsupported");
    invalid = document;
    invalid.ecaNodes[5].enabled = 0;
    const auto disabledEvent = CompileTriggersToJass(invalid, *loaded.value);
    Expect(disabledEvent.Succeeded() && disabledEvent.jassCode.find("TriggerRegisterTimerEvent") == std::string::npos,
        "disabled event was registered");
    invalid = document;
    invalid.triggers[0].enabled = 0;
    const auto disabledTrigger = CompileTriggersToJass(invalid, *loaded.value);
    Expect(disabledTrigger.Succeeded() && disabledTrigger.jassCode.find("gg_trg_Demo") == std::string::npos,
        "disabled trigger was generated");
    for (const auto backend : {ScriptBackend::Jass, ScriptBackend::VJass, ScriptBackend::Lua}) {
        const auto check = ScriptBuildService::CheckGameBuild(document, *loaded.value, backend);
        Expect(!check.CanBuildGameMap() && !check.mapInitializationAvailable && !check.externalCompilerValidated,
            "fragment was incorrectly classified as game-map-ready");
        Expect(check.triggerFragmentAvailable == (backend != ScriptBackend::Lua), "backend fragment capability wrong");
        Expect(std::any_of(check.diagnostics.begin(), check.diagnostics.end(), [](const auto& item) {
            return item.code == "script.map-initialization-unavailable";
        }), "full map initialization blocker missing");
    }
    const std::string strings = "STRING 42\n{\nDescription\n}\n";
    const auto* text = reinterpret_cast<const std::byte*>(strings.data());
    std::vector<War3Map::MapArchiveMember> sources{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.wts", {text, text + strings.size()}}};
    auto map = War3Map::MapDocument::FromMembers(sources);
    auto check = ScriptBuildService::CheckGameBuild(document, *loaded.value, ScriptBackend::Jass, &map);
    Expect(check.mapSourceDataAvailable && !check.CanBuildGameMap() && !check.mapInitializationAvailable,
        "parsed map inputs must not be classified as generated initialization");
    sources.pop_back(); map = War3Map::MapDocument::FromMembers(sources);
    check = ScriptBuildService::CheckGameBuild(document, *loaded.value, ScriptBackend::Jass, &map);
    Expect(!check.mapSourceDataAvailable && std::any_of(check.diagnostics.begin(), check.diagnostics.end(),
        [](const auto& d) { return d.code == "script.map-string-unresolved"; }), "missing string reference not exposed by build service");
}
void TestTriggerCandidateTransaction(
    const std::filesystem::path& fixture,
    const UiCatalog& catalog) {
    using War3Map::Trigger::TriggerDocumentEditor;

    const std::filesystem::path workspace = fixture / "trigger-candidate-source";
    const std::filesystem::path candidate = fixture / "trigger-candidate-output";
    const ByteVector sourceWtg = MakeWtg();
    const ByteVector sourceWct = MakeWct();
    WriteFile(workspace / "map" / "war3map.wtg", sourceWtg);
    WriteFile(workspace / "map" / "war3map.wct", sourceWct);

    WtgWctCodec codec;
    auto parsed = codec.Parse(sourceWtg, sourceWct, catalog);
    Expect(parsed.Succeeded(), "candidate transaction fixture did not parse");
    auto& document = *parsed.value;
    document.triggers.front().customTextFlag = 0;
    document.triggers.front().customText = {};
    auto appended = TriggerDocumentEditor::AppendRootEca(
        document,
        catalog,
        document.triggers.front().id,
        UiDefinitionKind::Action,
        "SetLife");
    Expect(appended.Succeeded(), "candidate transaction action could not be appended");
    const auto* action = TriggerDocumentEditor::FindEca(
        document, appended.value->rootEcaId);
    Expect(action != nullptr && action->arguments.size() == 2,
        "candidate transaction action graph is incomplete");
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document,
        document.argumentNodes[action->arguments[0]].id,
        0,
        "PresetFootman").Succeeded(),
        "candidate transaction preset edit failed");
    Expect(TriggerDocumentEditor::SetScalarArgument(
        document,
        document.argumentNodes[action->arguments[1]].id,
        3,
        "880").Succeeded(),
        "candidate transaction literal edit failed");

    auto expected = codec.Serialize(document, catalog);
    Expect(expected.Succeeded(), "candidate transaction output did not serialize");
    auto patch = codec.BuildPatch(
        document,
        catalog,
        "map/war3map.wtg",
        "map/war3map.wct",
        "synthetic trigger candidate");
    Expect(patch.Succeeded(), "candidate transaction patch did not seal");

    auto opened = War3Map::EditorSession::Open({
        workspace,
        War3Map::CompatibilityProfile::Default(),
        catalog.Sha256(),
    });
    Expect(opened.Succeeded(), "candidate transaction workspace did not open");
    const War3Map::ProjectRevision revision = opened.value->Revision();
    auto transaction = opened.value->BeginEdit(revision);
    Expect(transaction.Succeeded(), "candidate trigger transaction did not begin");
    Expect(opened.value->ApplyTriggerPatch(
        *transaction.value, std::move(*patch.value)).Succeeded(),
        "candidate trigger patch was rejected");
    const auto preview = opened.value->PreviewEdit(*transaction.value);
    Expect(preview.Succeeded() && preview.value->changes.size() == 2,
        "candidate trigger preview did not contain the WTG/WCT pair");
    const auto validation = opened.value->Validate(
        *transaction.value,
        opened.value->Profile(),
        War3Map::CompatibilityLevel::Parsed);
    Expect(validation.satisfied, "candidate trigger transaction failed L0 validation");
    Expect(opened.value->CommitCandidate(
        *transaction.value, revision, candidate).Succeeded(),
        "candidate trigger transaction did not commit");

    Expect(Equal(ReadFile(workspace / "map" / "war3map.wtg"), sourceWtg) &&
        Equal(ReadFile(workspace / "map" / "war3map.wct"), sourceWct),
        "candidate trigger transaction modified the source workspace");
    Expect(Equal(ReadFile(candidate / "map" / "war3map.wtg"), expected.value->wtg) &&
        Equal(ReadFile(candidate / "map" / "war3map.wct"), expected.value->wct),
        "candidate trigger transaction did not write the validated WTG/WCT pair");
    Expect(std::filesystem::is_regular_file(
        fixture / "trigger-candidate-output.war3me-manifest.json"),
        "candidate trigger transaction did not create a manifest sidecar");
}

void TestOptionalUiLibrariesAndRawMissingName(
    const std::filesystem::path& fixture,
    const UiCatalog& catalog) {
    const std::filesystem::path optionalRoot = fixture / "optional-ui";
    WriteText(optionalRoot / "ui" / "config", "not-installed/third-party\r\n");
    auto optionalCatalog = UiCatalog::Load(optionalRoot);
    Expect(optionalCatalog.Succeeded(), "missing optional UI layer made the catalog unusable");
    Expect(optionalCatalog.value->Layers().size() == 1, "optional UI layer identity was not retained");
    Expect(optionalCatalog.value->PresentSourceCount() == 1,
        "missing optional UI layer unexpectedly invented source files");

    WtgWctCodec codec;
    auto parsed = codec.Parse(MakeUnknownFunctionWtg(), MakeSingleSlotWct(), catalog);
    Expect(!parsed.Succeeded(), "missing third-party function did not fail closed");
    const auto diagnostic = std::find_if(
        parsed.diagnostics.begin(),
        parsed.diagnostics.end(),
        [](const War3Map::Diagnostic& value) {
            return value.code == "wtg.missing-ui-definition";
        });
    Expect(diagnostic != parsed.diagnostics.end(),
        "missing third-party function did not produce a structured diagnostic");
    Expect(diagnostic->message.find("UnavailableThirdPartyAction") != std::string::npos,
        "missing function diagnostic did not preserve the raw WTG function name");
}

void TestLegacyTriggerDataCatalog(const std::filesystem::path& fixture) {
    // 1.31.x 作者工具把旧式 UI 放在 share/mpq/<layer>/ui/ 下，
    // config 本身位于 share/mpq 根目录；这里用同样布局验证可选导入器。
    const std::filesystem::path installRoot = fixture / "legacy-share-mpq";
    const std::filesystem::path root = installRoot / "share" / "mpq";
    WriteText(root / "config", "legacy\r\n");
    WriteText(
        root / "legacy" / "ui" / "TriggerData.txt",
        "[TriggerActions]\r\n"
        "LegacyAction=0,player,string\r\n"
        "_LegacyAction_Defaults=Player00,key\r\n"
        "_LegacyAction_Category=TC_LEGACY\r\n"
        "[TriggerConditions]\r\n"
        "SyntheticCompareInteger=0,integer,ComparisonOperator,integer\r\n"
        "_SyntheticCompareInteger_Defaults=4,SyntheticEqual,9\r\n"
        "SyntheticAlways=0,nothing\r\n"
        "[TriggerCalls]\r\n"
        "LegacyCall=0,0,string,player\r\n"
        "_LegacyCall_Defaults=Player00\r\n"
        "DzAPI_Map_GetMapConfig=0,0,string,string\r\n"
        "_DzAPI_Map_GetMapConfig_Defaults=key\r\n"
        "_DzAPI_Map_GetMapConfig_Category=TC_LEGACY\r\n");
    WriteText(
        root / "legacy" / "ui" / "TriggerStrings.txt",
        "[TriggerActionStrings]\r\n"
        "LegacyAction=旧动作\r\n"
        "LegacyAction=\"设置 \",~whichPlayer,\" 的 \",~key\r\n"
        "LegacyActionHint=旧版帮助\r\n"
        "[TriggerCallStrings]\r\n"
        "LegacyCall=旧调用\r\n"
        "LegacyCall=\"读取 \",~player\r\n"
        "DzAPI_Map_GetMapConfig=旧全局存档\r\n"
        "DzAPI_Map_GetMapConfig=\"读取 \",~key\r\n");

    auto loaded = UiCatalog::Load(installRoot);
    Expect(loaded.Succeeded(), "legacy TriggerData catalog did not load");
    auto direct = UiCatalog::Load(root);
    Expect(direct.Succeeded() && direct.value->Sha256() == loaded.value->Sha256(),
        "installation and direct legacy data roots changed catalog content or layer order");
    const UiCatalog& catalog = *loaded.value;
    const UiDefinition* action = catalog.Find(UiDefinitionKind::Action, "LegacyAction");
    const UiDefinition* call = catalog.Find(UiDefinitionKind::Call, "LegacyCall");
    const UiDefinition* condition = catalog.Find(UiDefinitionKind::Condition, "SyntheticCompareInteger");
    const UiDefinition* noArgumentCondition = catalog.Find(UiDefinitionKind::Condition, "SyntheticAlways");
    const UiDefinition* dzApiCall = catalog.Find(
        UiDefinitionKind::Call, "DzAPI_Map_GetMapConfig");
    Expect(action != nullptr && action->ArgumentCount() == 2,
        "legacy TriggerData action signature was not imported");
    Expect(call != nullptr && call->ArgumentCount() == 1,
        "legacy TriggerData call signature was not imported");
    Expect(condition != nullptr && condition->ArgumentCount() == 3,
        "legacy TriggerConditions lost the first two parameters by applying the Call layout");
    Expect(noArgumentCondition != nullptr && noArgumentCondition->ArgumentCount() == 0,
        "legacy TriggerConditions nothing sentinel was not retained");

    const auto field = [](const UiDefinition& definition, std::string_view key) {
        const auto& fields = definition.Active().fields;
        const auto found = std::find_if(
            fields.begin(), fields.end(),
            [key](const auto& value) { return value.first == key; });
        return found == fields.end() ? std::string{} : found->second;
    };
    Expect(field(*action, "category") == "TC_LEGACY",
        "legacy TriggerData category metadata was not retained");
    Expect(field(*action, "description").find("${whichPlayer}") != std::string::npos,
        "legacy TriggerStrings description was not converted to placeholders");
    Expect(field(*action, "description") == "设置 ${whichPlayer} 的 ${key}",
        "legacy TriggerStrings lost whitespace that was quoted inside CSV fragments");
    Expect(action->Active().arguments[0].defaultValue == "Player00" &&
        action->Active().arguments[1].defaultValue == "key",
        "legacy TriggerData defaults were not imported");
    Expect(field(*call, "returns") == "string",
        "legacy TriggerData call return type was not imported");
    Expect(field(*condition, "returns") == "boolean" && field(*noArgumentCondition, "returns") == "boolean",
        "legacy TriggerConditions implicit boolean return type was not retained");
    const auto& conditionArguments = condition->Active().arguments;
    Expect(conditionArguments[0].type == "integer" && conditionArguments[1].type == "ComparisonOperator" &&
        conditionArguments[2].type == "integer", "legacy TriggerConditions parameter order changed");
    Expect(conditionArguments[0].defaultValue == "4" && conditionArguments[1].defaultValue == "SyntheticEqual" &&
        conditionArguments[2].defaultValue == "9", "legacy TriggerConditions defaults no longer match parameter slots");
    Expect(dzApiCall != nullptr && dzApiCall->ArgumentCount() == 1 &&
        field(*dzApiCall, "category") == "TC_LEGACY" &&
        dzApiCall->Active().arguments[0].defaultValue == "key",
        "legacy metadata for underscored function names was not imported");
}

void RunExternalRoundTrip(
    const std::filesystem::path& toolRoot,
    const std::filesystem::path& wtgPath,
    const std::filesystem::path& wctPath) {
    auto loaded = UiCatalog::Load(toolRoot);
    if (!loaded.Succeeded()) {
        for (const auto& diagnostic : loaded.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(loaded.Succeeded(), "external UI catalog did not load");
    Expect(loaded.value->Documents().size() == 35,
        "external UI catalog did not parse all 35 enabled TXT files");

    const ByteVector wtg = ReadFile(wtgPath);
    const ByteVector wct = ReadFile(wctPath);
    WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, *loaded.value);
    if (!parsed.Succeeded()) {
        for (const auto& diagnostic : parsed.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(parsed.Succeeded(), "external WTG/WCT did not parse");
    auto encoded = codec.Serialize(*parsed.value, *loaded.value);
    if (!encoded.Succeeded()) {
        for (const auto& diagnostic : encoded.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(encoded.Succeeded(), "external WTG/WCT did not serialize");
    Expect(Equal(encoded.value->wtg, wtg), "external WTG round-trip is not byte-identical");
    Expect(Equal(encoded.value->wct, wct), "external WCT round-trip is not byte-identical");
    const auto inspection = codec.Inspect(*parsed.value);
    std::cout << "external round-trip passed: triggers=" << inspection.triggerCount
              << " ecas=" << inspection.ecaCount
              << " arguments=" << inspection.argumentCount
              << " uiTxt=" << loaded.value->Documents().size() << '\n';
}

} // namespace

namespace {

// 编辑后写回验证（对应 N1a 另存为地图的触发器写回路径）：
// 解析真实 WTG/WCT → 删除一个注释节点 + 禁用一个普通节点 → 序列化 →
// 重新解析输出，确认结构与禁用标志全部正确。
void RunExternalEditRoundTrip(
    const std::filesystem::path& toolRoot,
    const std::filesystem::path& wtgPath,
    const std::filesystem::path& wctPath) {
    auto loaded = UiCatalog::Load(toolRoot);
    Expect(loaded.Succeeded(), "external UI catalog did not load");
    const ByteVector wtg = ReadFile(wtgPath);
    const ByteVector wct = ReadFile(wctPath);
    WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, *loaded.value);
    if (!parsed.Succeeded()) {
        for (const auto& diagnostic : parsed.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(parsed.Succeeded(), "external WTG/WCT did not parse");
    auto& document = *parsed.value;

    // 找一个无子节点的注释动作：模拟 GUI 的删除编辑。
    const War3Map::Trigger::EcaNode* removalTarget = nullptr;
    const War3Map::Trigger::EcaNode* disableTarget = nullptr;
    for (const auto& node : document.ecaNodes) {
        if (removalTarget == nullptr &&
            node.children.empty() && node.name.text.starts_with("CommentString")) {
            removalTarget = &node;
        }
        if (disableTarget == nullptr && node.enabled != 0) {
            disableTarget = &node;
        }
    }
    Expect(removalTarget != nullptr, "no removable comment node in external map");
    Expect(disableTarget != nullptr, "no disable-able node in external map");
    const StableId removedId = removalTarget->id;
    const StableId disabledId = disableTarget->id;
    const auto before = codec.Inspect(document);

    Expect(War3Map::Trigger::TriggerDocumentEditor::RemoveEcaSubtree(document, removedId).Succeeded(),
        "RemoveEcaSubtree failed on comment node");
    Expect(War3Map::Trigger::TriggerDocumentEditor::SetEcaEnabled(document, disabledId, false).Succeeded(),
        "SetEcaEnabled failed on normal node");

    auto encoded = codec.Serialize(document, *loaded.value);
    if (!encoded.Succeeded()) {
        for (const auto& diagnostic : encoded.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    Expect(encoded.Succeeded(), "edited document did not serialize");
    auto reparsed = codec.Parse(encoded.value->wtg, encoded.value->wct, *loaded.value);
    Expect(reparsed.Succeeded(), "edited output did not reparse");
    const auto after = codec.Inspect(*reparsed.value);
    Expect(after.triggerCount == before.triggerCount,
        "trigger count changed after edit round-trip");
    Expect(after.ecaCount + 1 == before.ecaCount,
        "reachable ECA count did not drop by exactly one after removal");
    const War3Map::Trigger::EcaNode* disabled = nullptr;
    for (const auto& node : reparsed.value->ecaNodes) {
        if (node.id == disabledId) {
            disabled = &node;
            break;
        }
    }
    Expect(disabled != nullptr && disabled->enabled == 0,
        "disabled flag did not persist through serialize/reparse");
    std::cout << "external edit-round-trip passed: triggers=" << after.triggerCount
              << " ecas=" << after.ecaCount << '\n';
}

} // namespace

// TriggerDirectoryTests.cpp
void TestTriggerDirectory(const UiCatalog& catalog, std::span<const std::byte> wtg, std::span<const std::byte> wct);
void TestEcaMove(const UiCatalog& catalog, std::span<const std::byte> wtg, std::span<const std::byte> wct);
// TriggerTypesTests.cpp
void TestTriggerTypes();
// TriggerDataTextTests.cpp
void TestTriggerDataText();
void TestWtgSource(const UiCatalog&, std::span<const std::byte>, std::span<const std::byte>);

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path fixture;
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--roundtrip") {
            RunExternalRoundTrip(argv[2], argv[3], argv[4]);
            return 0;
        }
        if (argc == 5 && std::wstring_view(argv[1]) == L"--edit-roundtrip") {
            RunExternalEditRoundTrip(argv[2], argv[3], argv[4]);
            return 0;
        }
        fixture = CreateFixtureRoot();
        auto loaded = UiCatalog::Load(fixture);
        Expect(loaded.Succeeded(), "synthetic UI catalog did not load");
        TestUiCatalog(*loaded.value);
        TestNaturalLanguagePresentation(*loaded.value);
        TestTriggerCodec(*loaded.value);
        TestWtgSource(*loaded.value, MakeWtg(), MakeWct());
        TestLibraryNodesEditable(*loaded.value);
        TestTriggerEditingAndUndo(*loaded.value);
        TestTriggerEditBatch(*loaded.value);
        TestTriggerAddRenameRemove(*loaded.value);
        TestAtomicRename(fixture);
        TestTriggerDirectory(*loaded.value, MakeWtg(), MakeWct());
        TestEcaMove(*loaded.value, MakeWtg(), MakeWct());
        TestTriggerTypes();
        TestTriggerDataText();
        TestJassCompiler();
        TestTriggerCandidateTransaction(fixture, *loaded.value);
        TestOptionalUiLibrariesAndRawMissingName(fixture, *loaded.value);
        TestLegacyTriggerDataCatalog(fixture);
        std::filesystem::remove_all(fixture);
        std::cout << "War3MapTrigger synthetic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (!fixture.empty() && std::filesystem::exists(fixture)) {
            std::filesystem::remove_all(fixture);
        }
        std::cerr << "War3MapTrigger test failure: " << error.what() << '\n';
        return 1;
    }
}
