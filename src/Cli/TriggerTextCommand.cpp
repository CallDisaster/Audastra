#include "TriggerTextCommand.h"

#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;

constexpr std::size_t kChunk = 64;

std::optional<std::vector<std::byte>> Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), {}};
    std::vector<std::byte> output(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) output[i] = static_cast<std::byte>(bytes[i]);
    return output;
}

std::string FirstMessage(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("(无诊断)") : diagnostics.front().code + " " + diagnostics.front().message;
}

struct Rendered {
    StableId id = 0;
    std::string name, text;
};

// 一批触发器整体替换（同一次绑定）；失败时文档不变，返回首条诊断。
std::optional<std::string> Replace(TriggerDocument& document, const UiCatalog& catalog, const std::vector<Rendered>& rows,
    const MapObjectReferenceSnapshot* objects) {
    const auto binding = CaptureTriggerGraphBinding(document, catalog, objects);
    if (!binding.Succeeded()) return "绑定失败：" + FirstMessage(binding.diagnostics);
    TriggerGraphEditBatch batch{*binding.value, {}};
    for (const auto& row : rows) batch.operations.emplace_back(GraphReplaceTriggerText{{GraphRecordKind::Trigger, row.id, {}}, row.text});
    const auto applied = ApplyTriggerGraphEditBatch(document, catalog, batch, {}, objects);
    if (!applied.Succeeded()) return "写回失败：" + FirstMessage(applied.diagnostics);
    return std::nullopt;
}

std::optional<std::string> Compare(const TriggerDocument& document, const UiCatalog& catalog, const Rendered& row) {
    const auto again = RenderTriggerText(document, catalog, row.id);
    if (!again.Succeeded()) return "再导出失败：" + FirstMessage(again.diagnostics);
    if (*again.value != row.text) return std::string("再导出与第一次不同");
    return std::nullopt;
}
} // namespace

int RunTriggerText(const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory,
    const std::filesystem::path& gameRoot, std::string_view triggerName) {
    const auto wtg = Read(mapDirectory / "war3map.wtg"), wct = Read(mapDirectory / "war3map.wct");
    if (!wtg || !wct) { std::cerr << "TriggerText: FILE_READ_FAILED\n"; return 1; }
    auto catalog = UiCatalog::Load(uiRoot);
    if (!catalog.Succeeded()) { std::cerr << FirstMessage(catalog.diagnostics) << '\n'; return 1; }
    auto parsed = WtgWctCodec{}.Parse(*wtg, *wct, *catalog.value);
    if (!parsed.Succeeded()) { std::cerr << FirstMessage(parsed.diagnostics) << '\n'; return 1; }
    auto& document = *parsed.value;
    // 预放对象（gg_unit_ 等）的类型资格来自地图成员与游戏数据，与 MCP 工作区同一来源。
    const auto objects = Services::CaptureDirectoryMapObjectReferences(mapDirectory, gameRoot);
    if (!objects.Succeeded()) { std::cerr << FirstMessage(objects.diagnostics) << '\n'; return 1; }
    const auto* snapshot = objects.value->get();
    if (!triggerName.empty()) {
        for (const auto& trigger : document.triggers) {
            if (trigger.name.text != triggerName) continue;
            const auto text = RenderTriggerText(document, *catalog.value, trigger.id);
            if (!text.Succeeded()) { std::cerr << FirstMessage(text.diagnostics) << '\n'; return 1; }
            std::cout << *text.value;
            return 0;
        }
        std::cerr << "TriggerText: TRIGGER_MISSING\n";
        return 1;
    }
    std::size_t total = 0, custom = 0, renderFailed = 0, roundTripped = 0, failed = 0, samples = 0;
    const auto report = [&](const char* kind, const std::string& name, const std::string& problem) {
        if (samples++ < 40) std::cout << kind << '\t' << name << '\t' << problem << '\n';
    };
    std::vector<Rendered> rendered;
    std::set<StableId> rejected; // 写回被拒的触发器保持原样，不计入往返成功
    for (const auto& trigger : document.triggers) {
        if (trigger.type != 0) continue;
        ++total;
        if (trigger.customTextFlag != 0) { ++custom; continue; }
        auto text = RenderTriggerText(document, *catalog.value, trigger.id);
        if (!text.Succeeded()) { ++renderFailed; report("render", trigger.name.text, FirstMessage(text.diagnostics)); continue; }
        rendered.push_back({trigger.id, trigger.name.text, std::move(*text.value)});
    }
    // 先按批写回，整批失败时逐个定位；全部写回后逐个再导出比较。
    for (std::size_t start = 0; start < rendered.size(); start += kChunk) {
        const std::vector<Rendered> chunk(rendered.begin() + static_cast<std::ptrdiff_t>(start),
            rendered.begin() + static_cast<std::ptrdiff_t>(std::min(rendered.size(), start + kChunk)));
        if (!Replace(document, *catalog.value, chunk, snapshot)) continue;
        for (const auto& row : chunk) {
            if (const auto problem = Replace(document, *catalog.value, {row}, snapshot)) { report("roundtrip", row.name, *problem); ++failed; rejected.insert(row.id); }
        }
    }
    for (const auto& row : rendered) {
        if (rejected.contains(row.id)) continue;
        if (const auto problem = Compare(document, *catalog.value, row)) {
            report("roundtrip", row.name, *problem); ++failed;
        } else {
            ++roundTripped;
        }
    }
    std::cout << "triggers=" << total << "\ncustomText=" << custom << "\nrendered=" << rendered.size()
              << "\nrenderFailed=" << renderFailed << "\nroundTripped=" << roundTripped << "\nroundTripFailed=" << failed << '\n';
    return renderFailed == 0 && failed == 0 ? 0 : 1;
}
