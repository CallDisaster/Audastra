#include "BuildInternal.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/ObjectData/SlkOptimizer.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include <algorithm>
#include <set>

namespace War3Map::HLua {
namespace {
using namespace ObjectData;
using Output = Result<BuildReceipt>;
std::vector<std::byte> Bytes(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())}; }
InspectReceipt Inspected(const FrameworkSnapshot& framework, const ProjectSnapshot& project) {
    std::set<std::string> required(project.jassModules.begin(), project.jassModules.end());
    for (const auto& file : framework.runtime.files)
        for (const auto& module : Lua::ScanModuleRequires(LuaText(file)))
            if (!module.dynamic && module.name.starts_with("jass.")) required.insert(module.name);
    return {framework.sha256, project.source.sha256, project.runtimeFiles.size(), project.hslkFiles.size(), {required.begin(), required.end()}};
}
void Replace(Services::MapSaveTask& task, std::string name, std::vector<std::byte> bytes) {
    const auto key = Resources::CanonicalImportPath(name);
    for (auto& item : task.replacements)
        if (Resources::CanonicalImportPath(item.name).value == key.value) { item.data = std::move(bytes); return; }
    task.replacements.push_back({std::move(name), std::move(bytes)});
}
}
Result<InspectReceipt> InspectProject(const FrameworkLocation& location, const std::filesystem::path& root) {
    auto framework = HLuaFrameworkLocator::Capture(location);
    if (!framework.Succeeded()) return {std::nullopt, std::move(framework.diagnostics)};
    auto project = HLuaProjectScanner::Capture(root);
    if (!project.Succeeded()) return {std::nullopt, std::move(project.diagnostics)};
    return Result<InspectReceipt>::Success(Inspected(*framework.value, *project.value));
}

Output BuildProject(const BuildRequest& request, const Services::MapSaveService::Progress& progress) {
    try {
        if (request.idsOnly) {
            auto framework = HLuaFrameworkLocator::Capture(request.framework);
            if (!framework.Succeeded()) return {std::nullopt, std::move(framework.diagnostics)};
            auto project = HLuaProjectScanner::Capture(request.project);
            if (!project.Succeeded()) return {std::nullopt, std::move(project.diagnostics)};
            auto existing = Detail::CaptureExistingIds(request);
            if (!existing.Succeeded()) return {std::nullopt, std::move(existing.diagnostics)};
            auto hslk = EvaluateHslk(*framework.value, *project.value, *existing.value, request.limits);
            if (!hslk.Succeeded()) return {std::nullopt, std::move(hslk.diagnostics)};
            BuildReceipt receipt; receipt.inputs = Inspected(*framework.value, *project.value);
            receipt.objects = std::move(hslk.value->objects); receipt.console = std::move(hslk.value->console);
            receipt.idReplayPassed = hslk.value->replayPassed;
            receipt.peakLuaBytes = hslk.value->peakMemoryBytes; receipt.luaInstructions = hslk.value->instructions;
            return Output::Success(std::move(receipt));
        }
        if ((!request.idsOnly && (request.target.empty() || std::filesystem::exists(request.target)))
            || request.workspace.empty() || request.gameRoot.empty() || std::filesystem::exists(request.workspace))
            return Output::Failure("HLua", "HLUA_OUTPUT_PATH", "构建需要游戏数据、不存在的目标地图和独占工作目录。");
        auto framework = HLuaFrameworkLocator::Capture(request.framework);
        if (!framework.Succeeded()) return {std::nullopt, std::move(framework.diagnostics)};
        auto project = HLuaProjectScanner::Capture(request.project);
        if (!project.Succeeded()) return {std::nullopt, std::move(project.diagnostics)};
        if (!request.expectedProjectSha256.empty() && request.expectedProjectSha256 != project.value->source.sha256)
            return Output::Failure("HLua", "HLUA_PROJECT_CHANGED", "工程 Lua 身份已改变，请重新检查。");
        if (!std::filesystem::create_directory(request.workspace)) return Output::Failure("HLua", "HLUA_OUTPUT_PATH", "无法创建独占工作目录。");
        if (progress) progress("hlua-base");
        auto base = Detail::CaptureBase(request);
        if (!base.Succeeded()) return {std::nullopt, std::move(base.diagnostics)};
        auto game = Game::GameDataSource::MountStrict(request.gameRoot);
        if (!game.Succeeded()) return {std::nullopt, std::move(game.diagnostics)};
        auto schema = ObjectFieldSchema::Load(*game.value);
        if (!schema.Succeeded()) return {std::nullopt, std::move(schema.diagnostics)};
        Services::ObjectCatalogSources catalogSources; catalogSources.archive = &base.value->archive; catalogSources.gameRoot = request.gameRoot;
        auto catalog = Services::LoadEffectiveObjectCatalog(catalogSources);
        if (!catalog.Succeeded()) return {std::nullopt, std::move(catalog.diagnostics)};
        auto nativeInputs = Services::ReadMapNativeObjectInputs(base.value->archive);
        if (!nativeInputs.Succeeded()) return {std::nullopt, std::move(nativeInputs.diagnostics)};
        auto baselines = std::move(*nativeInputs.value);
        std::set<std::string> occupied;
        for (std::size_t i = 0; i < baselines.size(); ++i) {
            for (const auto& object : baselines[i].Objects()) occupied.insert(object.key.rawcode);
        }
        for (const auto& object : base.value->f6Objects) if (!occupied.insert(object.id).second)
            return Output::Failure("HLua", "HLUA_ID_CONFLICT", "LNI 与底图原生物编 ID 冲突：" + object.id);
        std::vector<std::string> existing(occupied.begin(), occupied.end());
        if (progress) progress("hlua-hslk");
        auto evaluated = EvaluateHslk(*framework.value, *project.value, existing, request.limits);
        if (!evaluated.Succeeded()) return {std::nullopt, std::move(evaluated.diagnostics)};
        auto& hslk = *evaluated.value;
        std::vector<HslkObject> all = base.value->f6Objects;
        all.insert(all.end(), hslk.objects.begin(), hslk.objects.end());
        Services::MapSaveTask task; task.target = request.target;
        task.diagnostics = base.value->diagnostics;
        auto status = task.FreezeArchive(base.value->archive);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        task.testCopyOnly = request.mode == BuildTarget::Test;
        for (std::size_t i = 0; i < baselines.size(); ++i) {
            const auto type = static_cast<ObjectType>(i);
            if (std::none_of(all.begin(), all.end(), [&](const auto& object) { return object.objectClass == ToString(type); })) continue;
            auto edited = ApplyHslkObjects(type, baselines[i], all, *schema.value, catalog.value->Catalog());
            if (!edited.Succeeded()) return {std::nullopt, std::move(edited.diagnostics)};
            std::optional<std::vector<std::byte>> original;
            if (task.archive->HasMember(NativeFileName(type))) {
                auto bytes = task.archive->ReadMember(NativeFileName(type));
                if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
                original = std::move(*bytes.value);
            }
            auto snapshot = Services::ObjectTransactionSnapshot::Freeze(*edited.value,
                original ? std::optional<std::span<const std::byte>>{*original} : std::nullopt,
                task.sourceSha256, &*schema.value, &catalog.value->Catalog());
            if (!snapshot.Succeeded()) return {std::nullopt, std::move(snapshot.diagnostics)};
            task.objectTransactions.push_back(*snapshot.value);
            Replace(task, std::string(NativeFileName(type)), {snapshot.value->get()->EncodedNative().begin(), snapshot.value->get()->EncodedNative().end()});
        }
        auto runtime = Detail::RuntimeMembers(*framework.value, *project.value, hslk, request.mode);
        if (!runtime.Succeeded()) return {std::nullopt, std::move(runtime.diagnostics)};
        auto imports = Resources::ImportTable::CreateEmpty();
        if (task.archive->HasMember("war3map.imp")) {
            auto bytes = task.archive->ReadMember("war3map.imp");
            if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
            auto parsed = Resources::ImportTable::Parse(*bytes.value);
            if (!parsed.Succeeded()) return {std::nullopt, std::move(parsed.diagnostics)};
            imports = std::move(*parsed.value);
        }
        const auto exists = [&](std::string_view name) {
            const auto wanted = Resources::CanonicalImportPath(name);
            return task.archive->HasMember(name) || std::any_of(task.replacements.begin(), task.replacements.end(),
                [&](const auto& member) { return Resources::CanonicalImportPath(member.name).value == wanted.value; });
        };
        for (auto& file : *runtime.value) {
            const auto name = file.name; Replace(task, name, std::move(file.data));
            status = imports.Upsert(name, exists);
            if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        }
        auto encodedImports = imports.Serialize();
        if (!encodedImports.Succeeded()) return {std::nullopt, std::move(encodedImports.diagnostics)};
        Replace(task, "war3map.imp", std::move(*encodedImports.value));
        const auto scriptMember = task.archive->HasMember("war3map.j") ? "war3map.j" : "Scripts/war3map.j";
        if (task.archive->HasMember("war3map.j") && task.archive->HasMember("Scripts/war3map.j"))
            return Output::Failure("HLua", "HLUA_SCRIPT_CONFLICT", "两处地图主脚本同时存在。");
        auto script = task.archive->ReadMember(scriptMember);
        if (!script.Succeeded()) return {std::nullopt, std::move(script.diagnostics)};
        auto entry = InsertLuaEntry({reinterpret_cast<const char*>(script.value->data()), script.value->size()});
        if (!entry.Succeeded()) return {std::nullopt, std::move(entry.diagnostics)};
        status = Detail::CheckScript(*task.archive, *entry.value, *game.value);
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        Replace(task, scriptMember, Bytes(*entry.value));
        if (request.mode == BuildTarget::Dist) {
            SlkOptimizerInput input;
            input.schema = &*schema.value;
            input.gameFile = [&](const std::string& name) { return game.value->ReadFile(name); };
            input.mapMember = [&](const std::string& name) -> std::optional<std::vector<std::byte>> {
                const auto wanted = Resources::CanonicalImportPath(name);
                for (const auto& member : task.replacements) if (Resources::CanonicalImportPath(member.name).value == wanted.value) return member.data;
                if (!task.archive->HasMember(name)) return std::nullopt;
                auto bytes = task.archive->ReadMember(name);
                if (!bytes.Succeeded()) throw std::runtime_error("无法读取 SLK 优化来源：" + name);
                return std::move(*bytes.value);
            };
            auto optimized = OptimizeObjectDataToSlk(input);
            if (!optimized.Succeeded()) return {std::nullopt, std::move(optimized.diagnostics)};
            for (auto& member : optimized.value->replacements) Replace(task, std::move(member.name), std::move(member.bytes));
        }
        task.beforePublish = [&]() -> Status {
            auto checked = HLuaFrameworkLocator::Verify(*framework.value); if (!checked.Succeeded()) return checked;
            checked = Services::VerifyLuaFolder(project.value->source); if (!checked.Succeeded()) return checked;
            checked = Detail::VerifyTree(base.value->files); if (!checked.Succeeded()) return checked;
            return catalog.value->VerifyCapturedSources();
        };
        Services::MapSaveService::Run(task, progress);
        if (!task.succeeded) return {std::nullopt, std::move(task.diagnostics)};
        auto hash = Sha256File(task.written);
        if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
        BuildReceipt receipt;
        receipt.inputs = Inspected(*framework.value, *project.value);
        receipt.baseSha256 = base.value->inputSha256; receipt.outputSha256 = *hash.value; receipt.output = task.written;
        receipt.objects = std::move(hslk.objects); receipt.console = std::move(hslk.console);
        receipt.idReplayPassed = hslk.replayPassed; receipt.archiveReadbackPassed = true;
        receipt.peakLuaBytes = hslk.peakMemoryBytes; receipt.luaInstructions = hslk.instructions;
        receipt.diagnostics = task.diagnostics;
        receipt.differences = {"所有目标把 Lua 放入地图，不写入本机 package.path。", "数值直接编码原生 float32，不保留两位小数。",
            "字符串和数组经领域文档编码，保留引号、反斜杠与换行。", "布尔、嵌套数组、ID 冲突/溢出明确拒绝。",
            "缺少真实 InitGlobals 调用时拒绝出图。", "不复制或删除游戏目录中的插件。", "构建使用有界 Lua 5.3；tostring 的数值采用最短往返文本。"};
        auto result = Output::Success(std::move(receipt)); result.diagnostics = std::move(task.diagnostics); return result;
    } catch (const std::exception& e) { return Output::Failure("HLua", "HLUA_BUILD_FAILED", e.what()); }
}
} // namespace War3Map::HLua
