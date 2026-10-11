// 代码转回 T 的覆盖测量（手动运行，不登记 CTest）：对每个 GUI 触发器“转换为代码 → 不带快照按正文转回”，
// 统计实际可提交且可撤销的转回数量与拒绝原因，最后核对整图逐字节还原。解析耗时单独计量。用法：
//   war3-map-trigger-recovery-coverage <ui-root> <war3map.wtg> <war3map.wct>
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace War3Map;
namespace {
std::vector<std::byte> Read(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(stream)), {});
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) bytes[i] = static_cast<std::byte>(chars[i]);
    return bytes;
}
// 拒绝原因归类：去掉“第 N 行：”与引号内的具体名称。
std::string Reason(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "（无诊断）";
    std::string message = diagnostics.front().message;
    if (const auto colon = message.find("行："); colon != std::string::npos && message.rfind("第 ", 0) == 0) message = message.substr(colon + 3);
    for (const auto* marker : {"：", "“", " 的参数", "脚本名为 "}) {
        if (const auto at = message.find(marker); at != std::string::npos && at > 6) { message = message.substr(0, at + std::string(marker).size()); break; }
    }
    return message;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::cerr << "usage: <ui-root> <wtg> <wct>\n"; return 2; }
    Trigger::TriggerCodeEditor::SetFullReplayVerification(false);
    auto catalog = Trigger::UiCatalog::Load(argv[1]);
    if (!catalog.Succeeded()) return 1;
    const auto wtg = Read(argv[2]), wct = Read(argv[3]);
    Trigger::WtgWctCodec codec;
    auto parsed = codec.Parse(wtg, wct, *catalog.value);
    if (!parsed.Succeeded()) return 1;
    auto& document = *parsed.value;
    std::vector<StableId> ids;
    for (const auto& trigger : document.triggers)
        if (trigger.type == 0 && trigger.customTextFlag == 0 && !trigger.ecas.empty()) ids.push_back(trigger.id);
    std::size_t generatedFail = 0, parsed_ok = 0, parsedPartial = 0, snapshotOk = 0, undoMismatch = 0;
    std::size_t preservedLines = 0, preservedExpressions = 0;
    std::size_t parseAttempts = 0, parsedAccepted = 0, prepareFailed = 0, applyFailed = 0, snapshotFailed = 0, commitAttempts = 0;
    std::map<std::string, std::size_t> reasons;
    std::map<std::string, std::string> examples;
    double recoverMs = 0, maxMs = 0, commitMs = 0, maxCommitMs = 0;
    const auto recordFailure = [&](const std::string& stage, const std::vector<Diagnostic>& diagnostics) {
        const auto reason = stage + ": " + Reason(diagnostics);
        if (++reasons[reason] == 1) examples[reason] = diagnostics.empty() ? "（无诊断）" : diagnostics.front().message;
    };
    const auto commitAndUndo = [&](StableId id, const Trigger::GuiTriggerFragment& fragment, const std::string& mode) {
        const auto started = std::chrono::steady_clock::now();
        ++commitAttempts;
        const auto finish = [&](bool result) {
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            commitMs += elapsed; maxCommitMs = (std::max)(maxCommitMs, elapsed); return result;
        };
        auto recoveryPlan = Trigger::TriggerCodeEditor::PrepareRecovery(document, *catalog.value, id, fragment);
        if (!recoveryPlan.Succeeded() || !recoveryPlan.value) {
            ++prepareFailed; recordFailure(mode + " prepare", recoveryPlan.diagnostics); return finish(false);
        }
        auto applied = Trigger::TriggerCodeEditor::Apply(document, *catalog.value, *recoveryPlan.value);
        if (!applied.Succeeded() || !applied.value) {
            ++applyFailed; recordFailure(mode + " apply", applied.diagnostics); return finish(false);
        }
        const auto undone = Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *applied.value, false);
        if (!undone.Succeeded()) { ++undoMismatch; recordFailure(mode + " undo", undone.diagnostics); return finish(false); }
        return finish(true);
    };
    for (const auto id : ids) {
        Script::MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &*catalog.value;
        auto generated = Script::GenerateGuiTriggerSource(inputs, id);
        if (!generated.Succeeded() || !generated.value) { ++generatedFail; continue; }
        auto snapshot = Trigger::TriggerCodeEditor::CaptureGuiFragment(document, id);
        auto plan = Trigger::TriggerCodeEditor::PrepareConversion(document, *catalog.value, id, generated.value->text, generated.value->effectiveRunOnInit);
        if (!plan.Succeeded()) { ++generatedFail; continue; }
        auto receipt = Trigger::TriggerCodeEditor::Apply(document, *catalog.value, *plan.value);
        if (!receipt.Succeeded()) { ++generatedFail; continue; }
        const auto start = std::chrono::steady_clock::now();
        ++parseAttempts;
        auto recovered = Script::RecoverGuiTrigger(inputs, id);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        recoverMs += ms; maxMs = (std::max)(maxMs, ms);
        if (recovered.Succeeded() && recovered.value) {
            ++parsedAccepted;
            if (commitAndUndo(id, recovered.value->fragment, "parsed")) {
                if (recovered.value->preservedCodeLines || recovered.value->preservedCodeExpressions) ++parsedPartial;
                else ++parsed_ok;
                preservedLines += recovered.value->preservedCodeLines;
                preservedExpressions += recovered.value->preservedCodeExpressions;
            }
        }
        else {
            auto reason = Reason(recovered.diagnostics);
            // 无法识别的动作行：按该行开头两个词归类。
            const auto message = recovered.diagnostics.empty() ? std::string{} : recovered.diagnostics.front().message;
            if ((message.find("无法识别为 T 动作") != std::string::npos || message.find("无法切分") != std::string::npos)
                && message.rfind("第 ", 0) == 0) {
                const auto line = std::stoul(message.substr(std::string("第 ").size()));
                const auto* record = Trigger::TriggerDocumentEditor::FindTrigger(document, id);
                std::string text = record ? Trigger::TriggerCodeEditor::NormalizedText(record->customText) : std::string{};
                std::size_t start = 0;
                for (std::size_t n = 1; n < line && start != std::string::npos; ++n) { start = text.find('\n', start); if (start != std::string::npos) ++start; }
                if (start != std::string::npos) {
                    auto row = text.substr(start, text.find('\n', start) - start);
                    row.erase(0, row.find_first_not_of(" \t"));
                    const auto cut = row.find_first_of("(=", 0);
                    const bool lexical = message.find("无法切分") != std::string::npos;
                    reason += " " + row.substr(0, lexical ? 60 : cut == std::string::npos ? 40 : (std::min<std::size_t>)(cut, 40));
                }
            }
            if (++reasons[reason] == 1) examples[reason] = message;
        }
        if (undoMismatch) break; // 撤销链已断，禁止继续在不确定状态上累计覆盖数。
        if (snapshot.Succeeded() && snapshot.value) {
            const auto fromSnapshot = Script::RecoverGuiTrigger(inputs, id, &*snapshot.value);
            if (fromSnapshot.Succeeded() && fromSnapshot.value && fromSnapshot.value->fromSnapshot) {
                if (commitAndUndo(id, fromSnapshot.value->fragment, "snapshot")) ++snapshotOk;
            } else { ++snapshotFailed; recordFailure("snapshot recover", fromSnapshot.diagnostics); }
        } else { ++snapshotFailed; recordFailure("snapshot capture", snapshot.diagnostics); }
        if (undoMismatch) break;
        const auto conversionUndo = Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *receipt.value, false);
        if (!conversionUndo.Succeeded()) { ++undoMismatch; recordFailure("conversion undo", conversionUndo.diagnostics); break; }
    }
    auto encoded = codec.Serialize(document, *catalog.value);
    const bool exact = encoded.Succeeded() && encoded.value->wtg == wtg && encoded.value->wct == wct;
    std::printf("gui-triggers=%zu generate-or-convert-failed=%zu parsed-recovered=%zu snapshot-recovered=%zu undo-failed=%zu\n",
        ids.size(), generatedFail, parsed_ok, snapshotOk, undoMismatch);
    std::printf("parsed-partial=%zu preserved-code-lines=%zu preserved-code-expressions=%zu (not counted in parsed-recovered)\n",
        parsedPartial, preservedLines, preservedExpressions);
    std::printf("parsed-accepted-before-commit=%zu recovery-prepare-failed=%zu recovery-apply-failed=%zu snapshot-failed=%zu\n",
        parsedAccepted, prepareFailed, applyFailed, snapshotFailed);
    std::printf("recover-avg=%.2f ms recover-max=%.2f ms final-byte-exact=%d\n", ids.empty() ? 0.0 : recoverMs / ids.size(), maxMs, exact ? 1 : 0);
    std::printf("parse-attempts=%zu parse-attempt-avg=%.2f ms (recover-avg retains all-GUI denominator; both exclude commit/undo)\n",
        parseAttempts, parseAttempts ? recoverMs / parseAttempts : 0.0);
    std::printf("commit-undo-attempts=%zu commit-undo-avg=%.2f ms commit-undo-max=%.2f ms\n",
        commitAttempts, commitAttempts ? commitMs / commitAttempts : 0.0, maxCommitMs);
    std::vector<std::pair<std::size_t, std::string>> sorted;
    for (const auto& [reason, count] : reasons) sorted.emplace_back(count, reason);
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [count, reason] : sorted) std::printf("%5zu  %s\n       e.g. %s\n", count, reason.c_str(), examples[reason].c_str());
    return exact && !undoMismatch && !prepareFailed && !applyFailed ? 0 : 1;
}
