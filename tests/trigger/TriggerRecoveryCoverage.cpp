// 代码转回 T 的覆盖测量（手动运行，不登记 CTest）：对每个 GUI 触发器“转换为代码 → 不带快照按正文转回”，
// 统计可转回数量与拒绝原因，并核对带快照时全部原样恢复、每次撤销后整图逐字节还原。用法：
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
    std::size_t generatedFail = 0, parsed_ok = 0, snapshotOk = 0, undoMismatch = 0;
    std::map<std::string, std::size_t> reasons;
    std::map<std::string, std::string> examples;
    double recoverMs = 0, maxMs = 0;
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
        auto recovered = Script::RecoverGuiTrigger(inputs, id);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        recoverMs += ms; maxMs = (std::max)(maxMs, ms);
        if (recovered.Succeeded()) ++parsed_ok;
        else {
            auto reason = Reason(recovered.diagnostics);
            // 无法识别的动作行：按该行开头两个词归类。
            const auto& message = recovered.diagnostics.front().message;
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
            if (++reasons[reason] == 1) examples[reason] = document.triggers.empty() ? "" : recovered.diagnostics.front().message;
        }
        if (snapshot.Succeeded() && Script::RecoverGuiTrigger(inputs, id, &*snapshot.value).Succeeded()) ++snapshotOk;
        if (!Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *receipt.value, false).Succeeded()) ++undoMismatch;
    }
    auto encoded = codec.Serialize(document, *catalog.value);
    const bool exact = encoded.Succeeded() && encoded.value->wtg == wtg && encoded.value->wct == wct;
    std::printf("gui-triggers=%zu generate-or-convert-failed=%zu parsed-recovered=%zu snapshot-recovered=%zu undo-failed=%zu\n",
        ids.size(), generatedFail, parsed_ok, snapshotOk, undoMismatch);
    std::printf("recover-avg=%.2f ms recover-max=%.2f ms final-byte-exact=%d\n", ids.empty() ? 0.0 : recoverMs / ids.size(), maxMs, exact ? 1 : 0);
    std::vector<std::pair<std::size_t, std::string>> sorted;
    for (const auto& [reason, count] : reasons) sorted.emplace_back(count, reason);
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [count, reason] : sorted) std::printf("%5zu  %s\n       e.g. %s\n", count, reason.c_str(), examples[reason].c_str());
    return exact ? 0 : 1;
}
