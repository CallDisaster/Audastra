// 触发器转代码耗时测量（手动运行，不登记 CTest）：在给定 WTG/WCT 与 UI 根上分段计时
// 生成、计划、应用、撤销、重做，定位整图开销。用法：
//   war3-map-trigger-conversion-bench <ui-root> <war3map.wtg> <war3map.wct> [trigger-name]
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
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
template <class F> auto Time(const char* name, F&& body) {
    const auto start = std::chrono::steady_clock::now();
    auto result = body();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("%-28s %9.2f ms\n", name, ms);
    return result;
}
void Print(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& item : diagnostics) std::cerr << item.code << ": " << item.message << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::cerr << "usage: <ui-root> <wtg> <wct> [trigger-name [--verify]]\n"; return 2; }
    const bool verify = argc > 5 && std::string_view(argv[5]) == "--verify";
    Trigger::TriggerCodeEditor::SetFullReplayVerification(verify);
    auto catalog = Time("catalog-load", [&] { return Trigger::UiCatalog::Load(argv[1]); });
    if (!catalog.Succeeded()) { Print(catalog.diagnostics); return 1; }
    const auto wtg = Read(argv[2]);
    const auto wct = Read(argv[3]);
    Trigger::WtgWctCodec codec;
    auto parsed = Time("parse", [&] { return codec.Parse(wtg, wct, *catalog.value); });
    if (!parsed.Succeeded()) { Print(parsed.diagnostics); return 1; }
    auto& document = *parsed.value;
    std::printf("triggers=%zu ecas=%zu arguments=%zu\n", document.triggers.size(), document.ecaNodes.size(), document.argumentNodes.size());
    Time("validate", [&] { return codec.Validate(document, *catalog.value); });
    {
        std::size_t total = 0, utf8 = 0, ascii = 0, other = 0;
        const auto visit = [&](const Trigger::RawText& text) {
            ++total;
            const bool pureAscii = std::all_of(text.originalBytes.begin(), text.originalBytes.end(), [](std::byte b) { return b < std::byte{0x80}; });
            if (text.encoding == Trigger::TextEncoding::Utf8) ++utf8; else ++other;
            if (pureAscii) ++ascii;
        };
        for (const auto& eca : document.ecaNodes) visit(eca.name);
        for (const auto& argument : document.argumentNodes) visit(argument.value);
        for (const auto& trigger : document.triggers) { visit(trigger.name); visit(trigger.description); visit(trigger.customText); }
        std::printf("rawtext total=%zu utf8=%zu non-utf8=%zu ascii-original=%zu\n", total, utf8, other, ascii);
        Time("all-rawtext-tobytes", [&] {
            std::size_t bytes = 0;
            for (const auto& eca : document.ecaNodes) bytes += eca.name.ToBytes().value->size();
            for (const auto& argument : document.argumentNodes) bytes += argument.value.ToBytes().value->size();
            for (const auto& trigger : document.triggers) bytes += trigger.name.ToBytes().value->size() + trigger.customText.ToBytes().value->size();
            return bytes;
        });
    }    Time("serialize", [&] { return codec.Serialize(document, *catalog.value); });
    Time("document-copy", [&] { auto copy = document; return copy.triggers.size(); });
    Time("graph-identity", [&] { return Trigger::CaptureTriggerGraphReplayIdentity(document, *catalog.value); });

    const Trigger::TriggerRecord* target = nullptr;
    for (const auto& trigger : document.triggers) {
        if (trigger.type != 0 || trigger.customTextFlag != 0 || trigger.ecas.empty()) continue;
        if (argc > 4 && argv[4][0] ? trigger.name.text == argv[4] : (!target || trigger.ecas.size() > target->ecas.size())) target = &trigger;
    }
    if (!target) { std::cerr << "no GUI trigger\n"; return 1; }
    const auto id = target->id;
    std::printf("target=%s top-level-ecas=%zu\n", target->name.text.c_str(), target->ecas.size());

    for (int round = 0; round < 2; ++round)
        Time("toolchain-capability", [&] { return Services::QueryScriptRebuildCapability(argv[1], argv[1]).state; });
    Script::MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &*catalog.value;
    auto generated = Time("generate-source", [&] { return Script::GenerateGuiTriggerSource(inputs, id); });
    if (!generated.Succeeded() || !generated.value) { Print(generated.diagnostics); return 1; }
    auto plan = Time("prepare-conversion", [&] {
        return Trigger::TriggerCodeEditor::PrepareConversion(document, *catalog.value, id,
            generated.value->text, generated.value->effectiveRunOnInit);
    });
    if (!plan.Succeeded() || !plan.value) { Print(plan.diagnostics); return 1; }
    auto receipt = Time("apply", [&] { return Trigger::TriggerCodeEditor::Apply(document, *catalog.value, *plan.value); });
    if (!receipt.Succeeded() || !receipt.value) { Print(receipt.diagnostics); return 1; }
    for (int round = 0; round < 2; ++round) {
        const auto undo = Time("undo", [&] { return Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *receipt.value, false); });
        if (!undo.Succeeded()) { Print(undo.diagnostics); return 1; }
        const auto redo = Time("redo", [&] { return Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *receipt.value, true); });
        if (!redo.Succeeded()) { Print(redo.diagnostics); return 1; }
    }
    const auto final = Trigger::TriggerCodeEditor::Replay(document, *catalog.value, *receipt.value, false);
    if (!final.Succeeded()) { Print(final.diagnostics); return 1; }
    auto encoded = codec.Serialize(document, *catalog.value);
    const bool exact = encoded.Succeeded() && encoded.value->wtg == wtg && encoded.value->wct == wct;
    std::printf("final-undo-byte-exact=%d\n", exact ? 1 : 0);
    return exact ? 0 : 1;
}
