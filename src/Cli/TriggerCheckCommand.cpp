#include "TriggerCheckCommand.h"
#include "War3MapCore/Script/TriggerStorageCapabilities.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

namespace {
using namespace War3Map;

std::string FirstMessage(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("(无诊断)") : diagnostics.front().code + " " + diagnostics.front().message;
}

const char* Severity(DiagnosticSeverity severity) {
    return severity == DiagnosticSeverity::Error ? "error" : severity == DiagnosticSeverity::Warning ? "warning" : "info";
}

// 在内存中用文本整体替换一个触发器；失败时列出全部诊断。
bool ReplaceTrigger(Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog, const MapArchiveSession& archive,
    const std::filesystem::path& gameRoot, const std::filesystem::path& toolRoot, std::string_view name, const std::filesystem::path& textFile) {
    std::ifstream stream(textFile, std::ios::binary);
    if (!stream) { std::cerr << "TriggerCheck: TEXT_FILE_READ_FAILED\n"; return false; }
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
        [&](const auto& row) { return row.name.text == name; });
    if (trigger == document.triggers.end()) { std::cerr << "TriggerCheck: TRIGGER_MISSING\n"; return false; }
    const auto objects = Services::CaptureArchiveMapObjectReferences(archive, gameRoot);
    if (!objects.Succeeded()) { std::cerr << FirstMessage(objects.diagnostics) << '\n'; return false; }
    const auto* references = objects.value->get();
    const auto binding = Trigger::CaptureTriggerGraphBinding(document, catalog, references);
    if (!binding.Succeeded()) { std::cerr << FirstMessage(binding.diagnostics) << '\n'; return false; }
    Trigger::TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(Trigger::GraphReplaceTriggerText{{Trigger::GraphRecordKind::Trigger, trigger->id, {}}, text});
    Trigger::TriggerGraphEditLimits limits;
    limits.storageCapabilityProvider = [&](std::span<const std::string> names) {
        return Script::CaptureMapTriggerStorageCapabilities(catalog, toolRoot, toolRoot, archive.SourcePath().parent_path(), names);
    };
    const auto applied = Trigger::ApplyTriggerGraphEditBatch(document, catalog, batch, limits, references);
    if (!applied.Succeeded()) {
        for (const auto& diagnostic : applied.diagnostics) std::cout << "edit " << diagnostic.code << " " << diagnostic.message << '\n';
        return false;
    }
    for (const auto& diagnostic : applied.diagnostics) std::cout << "edit " << diagnostic.code << " " << diagnostic.message << '\n';
    return true;
}
} // namespace

int RunTriggerCheck(const std::filesystem::path& ydweRoot, const std::filesystem::path& mapPath,
    const std::filesystem::path& gameRoot, std::string_view triggerName, const std::filesystem::path& textFile) {
    auto archive = MapArchiveSession::Open(mapPath);
    if (!archive.Succeeded() || !archive.value) { std::cerr << FirstMessage(archive.diagnostics) << '\n'; return 1; }
    const auto wtg = archive.value->ReadMember("war3map.wtg"), wct = archive.value->ReadMember("war3map.wct");
    if (!wtg.Succeeded() || !wct.Succeeded()) { std::cerr << "TriggerCheck: TRIGGER_MEMBER_READ_FAILED\n"; return 1; }
    auto loaded = Trigger::UiCatalog::Load(ydweRoot);
    if (!loaded.Succeeded()) { std::cerr << FirstMessage(loaded.diagnostics) << '\n'; return 1; }
    const auto catalog = std::make_shared<const Trigger::UiCatalog>(std::move(*loaded.value));
    auto parsed = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *catalog);
    if (!parsed.Succeeded()) { std::cerr << FirstMessage(parsed.diagnostics) << '\n'; return 1; }
    auto document = std::make_shared<Trigger::TriggerDocument>(std::move(*parsed.value));

    auto snapshot = std::make_shared<Services::CodeCheckProjectSnapshot>();
    snapshot->binding = "cli"; snapshot->revision = 1;
    snapshot->sourcePath = mapPath; snapshot->sourceSha256 = archive.value->SourceSha256();
    snapshot->toolRoot = ydweRoot; snapshot->uiRoot = ydweRoot; snapshot->gameRoot = gameRoot;
    snapshot->triggers = std::make_shared<const Trigger::TriggerDocument>(*document);
    snapshot->catalog = catalog;
    if (const auto terrain = archive.value->ReadMember("war3map.w3e"); terrain.Succeeded())
        snapshot->terrain = Script::ReadTerrainExtent(*terrain.value);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto type = static_cast<ObjectData::ObjectType>(index);
        const std::string name(ObjectData::NativeFileName(type));
        if (!archive.value->HasMember(name)) continue;
        const auto bytes = archive.value->ReadMember(name);
        if (!bytes.Succeeded()) { std::cerr << FirstMessage(bytes.diagnostics) << '\n'; return 1; }
        auto native = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
        auto* objects = native.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.get()) : nullptr;
        if (objects == nullptr) { std::cerr << FirstMessage(native.diagnostics) << '\n'; return 1; }
        native.document.release();
        snapshot->nativeObjects[index] = std::shared_ptr<const ObjectData::NativeObjectDocument>(objects);
    }

    if (!triggerName.empty() && !textFile.empty()
        && !ReplaceTrigger(*document, *catalog, *archive.value, gameRoot, ydweRoot, triggerName, textFile)) return 2;

    Services::CodeCheckProjectCache cache;
    const auto started = std::chrono::steady_clock::now();
    const auto result = Services::CheckProjectTriggers(snapshot, cache, document, 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::size_t located = 0;
    for (const auto& diagnostic : result.diagnostics) located += diagnostic.trigger ? 1 : 0;
    std::cout << "completed=" << result.completed << " context=" << result.contextComplete << " passed=" << result.Succeeded()
              << " diagnostics=" << result.diagnostics.size() << " located=" << located << " ms=" << elapsed << '\n';
    if (!result.reason.empty()) std::cout << "reason " << result.reason << '\n';
    std::size_t shown = 0;
    for (const auto& diagnostic : result.diagnostics) {
        if (++shown > 200) { std::cout << "...\n"; break; }
        std::cout << Severity(diagnostic.severity) << ' ' << diagnostic.code << ' ' << diagnostic.source << ':' << diagnostic.line
                  << ' ' << diagnostic.message << '\n';
        if (const auto& at = diagnostic.trigger) {
            std::cout << "    T " << at->triggerName << " 第" << at->line << "行第" << at->column << "列 " << at->function
                      << " | " << at->generated << '\n';
        }
    }
    return result.Succeeded() ? 0 : 3;
}
