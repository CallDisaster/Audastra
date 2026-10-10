#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 触发器目录（类别 / 移动 / 复制粘贴）回归：夹具来自 TriggerCodecTests.cpp 的原创合成 WTG/WCT
// （类别 1 “Category”，触发器 Normal 与 Legacy，WCT 槽 slot-normal / slot-legacy）。
namespace {

using namespace War3Map::Trigger;
using Directory = TriggerDirectoryEditor;
using War3Map::StableId;
using War3Map::Trigger::UiDefinitionKind;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("trigger directory: " + std::string(message));
    }
}

const TriggerRecord& Named(const TriggerDocument& document, std::string_view name) {
    const auto found = std::find_if(document.triggers.begin(), document.triggers.end(),
        [name](const TriggerRecord& trigger) { return trigger.name.text == name; });
    Check(found != document.triggers.end(), "trigger not found: " + std::string(name));
    return *found;
}

std::vector<std::string> Order(const TriggerDocument& document) {
    std::vector<std::string> names;
    for (const auto& trigger : document.triggers) names.push_back(trigger.name.text);
    return names;
}

EncodedTriggerDocument Encode(const WtgWctCodec& codec, const TriggerDocument& document, const UiCatalog& catalog) {
    auto encoded = codec.Serialize(document, catalog);
    Check(encoded.Succeeded(), "document did not serialize");
    return std::move(*encoded.value);
}

bool Same(const EncodedTriggerDocument& left, const EncodedTriggerDocument& right) {
    return left.wtg == right.wtg && left.wct == right.wct;
}

} // namespace

void TestTriggerDirectory(const UiCatalog& catalog, std::span<const std::byte> wtg, std::span<const std::byte> wct) {
    WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, catalog);
    Check(parsed.Succeeded(), "fixture did not parse");
    TriggerDocument& document = *parsed.value;
    const EncodedTriggerDocument pristine = Encode(codec, document, catalog);
    const TriggerDirectoryLayout original = Directory::CaptureLayout(document);
    const std::int32_t first = document.categories.front().categoryId;

    // 原版命名与名称校验。
    Check(Directory::UniqueTriggerName(document, "未命名触发器") == "未命名触发器 001", "unique trigger name");
    Check(Directory::UniqueName({"A 001", "A 002"}, "A") == "A 003", "unique name skips existing");
    Check(!Directory::ValidateTriggerName(document, "Normal").Succeeded(), "duplicate trigger name accepted");
    Check(Directory::ValidateTriggerName(document, " Normal ", Named(document, "Normal").id).Succeeded(),
        "renaming a trigger to its own name rejected");
    Check(!Directory::ValidateTriggerName(document, "   ").Succeeded(), "blank trigger name accepted");

    // 脚本名：非字母数字的每个字节换成 '_'，结尾补 'u'；等长中文名得到同一 gg_trg_ 标识符，必须拒绝。
    Check(Directory::ScriptName("技能释放") == std::string(12, '_') + "u", "chinese script name");
    Check(Directory::ScriptName("Hero Init  ") == "Hero_Init", "trailing spaces trimmed in script name");
    auto chinese = TriggerDocumentEditor::AddTrigger(document, first, "测试甲");
    Check(chinese.Succeeded(), "chinese trigger name rejected");
    const auto collide = TriggerDocumentEditor::AddTrigger(document, first, "测试乙");
    Check(!collide.Succeeded() && collide.diagnostics.front().code == "TRIGGER_SCRIPT_NAME_DUPLICATE",
        "same-length chinese name must collide");
    Check(!Directory::ValidateTriggerName(document, "测试乙").Succeeded(), "script-name collision accepted by validation");
    Check(Directory::ValidateTriggerName(document, "测试乙", *chinese.value).Succeeded(), "self rename must not collide");
    Check(!TriggerDocumentEditor::RenameTrigger(document, Named(document, "Normal").id, "测试乙").Succeeded(),
        "rename into a colliding script name accepted");
    Check(Directory::UniqueTriggerName(document, "测试") == "测试 001", "unique name keeps first free number");
    Check(TriggerDocumentEditor::RemoveTrigger(document, *chinese.value).Succeeded(), "cleanup failed");
    Check(Same(Encode(codec, document, catalog), pristine), "script-name checks must leave the document unchanged");

    // 新建类别并移动触发器：跨类别、类别内排序。
    auto added = Directory::AddCategory(document, Directory::UniqueCategoryName(document, "未命名类别"), first);
    Check(added.Succeeded(), "AddCategory failed");
    const std::int32_t second = *added.value;
    Check(second != first && document.categories.size() == 2 && document.categories[1].categoryId == second &&
            document.categories[1].name.text == "未命名类别 001" && document.categories[1].id != 0,
        "new category not placed after the first one");
    Check(!Directory::ValidateCategoryName(document, "未命名类别 001").Succeeded(), "duplicate category name accepted");
    Check(Directory::RenameCategory(document, second, "  战斗  ").Succeeded() &&
            document.categories[1].name.text == "战斗", "RenameCategory did not trim");
    Check(Directory::MoveTrigger(document, Named(document, "Legacy").id, second).Succeeded(), "move across categories");
    Check(Directory::MoveTrigger(document, Named(document, "Normal").id, second, Named(document, "Legacy").id).Succeeded(),
        "move before a trigger");
    Check(Order(document) == std::vector<std::string>{"Normal", "Legacy"} &&
            Named(document, "Normal").categoryId == second, "trigger order after moves");
    Check(!Directory::MoveTrigger(document, Named(document, "Normal").id, 0x7f7f7f7f).Succeeded(),
        "move into a missing category accepted");
    Check(!Directory::RemoveEmptyCategory(document, second).Succeeded(), "non-empty category removed");
    Check(Directory::RemoveEmptyCategory(document, first).Succeeded() && document.categories.size() == 1,
        "empty category not removed");
    auto third = Directory::AddCategory(document, "末尾");
    Check(third.Succeeded() && Directory::MoveCategory(document, *third.value, second).Succeeded() &&
            document.categories.front().categoryId == *third.value, "MoveCategory before another category");

    // 写出再读回：类别表、归属与 WCT 槽对齐都保留。
    auto moved = Encode(codec, document, catalog);
    auto reparsed = codec.Parse(moved.wtg, moved.wct, catalog);
    Check(reparsed.Succeeded(), "moved document did not reparse");
    Check(reparsed.value->categories.size() == 2 && reparsed.value->categories[1].name.text == "战斗",
        "category table did not round-trip");
    Check(Named(*reparsed.value, "Normal").customText.text == "slot-normal" &&
            Named(*reparsed.value, "Legacy").customText.text == "slot-legacy",
        "WCT slots did not follow their triggers");

    // 布局撤销：恢复后逐字节等于原始写出。
    Directory::ApplyLayout(document, original);
    Check(Same(Encode(codec, document, catalog), pristine), "ApplyLayout did not restore the original bytes");

    // 复制粘贴：新 ID、同类别的源触发器之后、参数原样，删除后字节恢复。
    const StableId legacyId = Named(document, "Legacy").id;
    auto snapshot = Directory::CopyTrigger(document, legacyId);
    Check(snapshot.Succeeded() && snapshot.value->roots.size() == 1, "CopyTrigger failed");
    Check(!Directory::PasteTrigger(document, catalog, *snapshot.value, 0x7f7f7f7f, "X").Succeeded() &&
            document.triggers.size() == 2, "paste into a missing category changed the document");
    auto pasted = Directory::PasteTrigger(document, catalog, *snapshot.value, first, "Legacy 副本", Named(document, "Normal").id);
    Check(pasted.Succeeded(), "PasteTrigger failed");
    Check(Order(document) == std::vector<std::string>{"Normal", "Legacy 副本", "Legacy"}, "pasted trigger position");
    const TriggerRecord& copy = Named(document, "Legacy 副本");
    Check(copy.id != legacyId && copy.ecas.size() == 1 && copy.customText.text == "slot-legacy", "pasted trigger content");
    const EcaNode& copiedRoot = document.ecaNodes[copy.ecas.front()];
    const EcaNode& sourceRoot = document.ecaNodes[Named(document, "Legacy").ecas.front()];
    Check(copiedRoot.id != sourceRoot.id && copiedRoot.name.text == sourceRoot.name.text &&
            document.argumentNodes[copiedRoot.arguments.front()].value.text == "hfoo", "pasted ECA subtree");
    Check(codec.Validate(document, catalog).Succeeded(), "document with pasted trigger did not validate");
    auto withCopy = Encode(codec, document, catalog);
    Check(codec.Parse(withCopy.wtg, withCopy.wct, catalog).Succeeded(), "pasted trigger did not reparse");
    Check(TriggerDocumentEditor::RemoveTrigger(document, *pasted.value).Succeeded(), "removing the pasted trigger failed");
    Check(Same(Encode(codec, document, catalog), pristine), "paste + remove did not restore the original bytes");
}

// ECA 移动：同一触发器内重排，撤销后逐字节恢复；跨触发器与非法目标被拒绝。
void TestEcaMove(const UiCatalog& catalog, std::span<const std::byte> wtg, std::span<const std::byte> wct) {
    WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, catalog);
    Check(parsed.Succeeded(), "fixture did not parse");
    TriggerDocument& document = *parsed.value;
    const StableId normal = Named(document, "Normal").id;
    TriggerDocumentEditor::FindTrigger(document, normal)->customTextFlag = 0; // 夹具触发器带自定义文本标志；GUI 编辑需要普通触发器
    auto appended = TriggerDocumentEditor::AppendRootEca(document, catalog, normal, UiDefinitionKind::Action, "DoNothing");
    Check(appended.Succeeded(), "AppendRootEca failed: " + (appended.diagnostics.empty() ? std::string() : appended.diagnostics.front().code + " " + appended.diagnostics.front().message));
    const EncodedTriggerDocument before = Encode(codec, document, catalog);
    const auto& roots = Named(document, "Normal").ecas;
    const StableId first = document.ecaNodes[roots[0]].id;
    const StableId second = document.ecaNodes[roots[1]].id;

    auto moved = TriggerDocumentEditor::MoveEca(document, second, normal, 0, std::nullopt, first);
    Check(moved.Succeeded(), "MoveEca failed");
    Check(document.ecaNodes[Named(document, "Normal").ecas[0]].id == second, "MoveEca did not reorder");
    Check(codec.Validate(document, catalog).Succeeded(), "moved document did not validate");
    Check(TriggerDocumentEditor::UndoMoveEca(document, *moved.value).Succeeded(), "UndoMoveEca failed");
    Check(Same(Encode(codec, document, catalog), before), "move + undo did not restore the original bytes");
    Check(TriggerDocumentEditor::RedoMoveEca(document, *moved.value).Succeeded() &&
            document.ecaNodes[Named(document, "Normal").ecas[0]].id == second, "RedoMoveEca failed");

    Check(!TriggerDocumentEditor::MoveEca(document, second, Named(document, "Legacy").id, 0, std::nullopt).Succeeded(),
        "cross-trigger move accepted");
    Check(!TriggerDocumentEditor::MoveEca(document, second, 0, second, 1).Succeeded(), "move into itself accepted");
    Check(!TriggerDocumentEditor::MoveEca(document, second, normal, 0, 1).Succeeded(), "root move with a branch accepted");

    // 参数记号：定义外 / 模板外的参数同样给出在文字中的位置。
    const auto presented = PresentEca(document, Named(document, "Legacy").ecas.front(), catalog);
    Check(presented.arguments.size() == 1, "argument span missing");
    const auto& span = presented.arguments.front();
    Check(presented.text.substr(span.begin, span.length) == "hfoo" && span.position == 0 && span.uiType == "unit" &&
            !span.empty && !span.call, "argument span does not cover the rendered value: " + presented.text);
}
