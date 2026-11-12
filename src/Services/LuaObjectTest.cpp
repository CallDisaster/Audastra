#include "War3MapCore/Services/LuaObjectSnapshot.h"
#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "War3MapCore/Lua/YdweRawcode.h"
#include "War3MapCore/Runtime/LuaObjectSnapshot.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Services/MapTestWorkspace.h"
#include "War3MapCore/VerifiedFile.h"
#include <Windows.h>
#include <algorithm>

namespace War3Map::Services {
namespace {
// 按测试副本合同替换成员：先在独占临时目录完成原保存与回读，再复制进测试工作区成为新的测试图。
// 测试目录由清理租约持有 DELETE 句柄，归档发布另取完整祖先租约，所以不直接在测试目录里保存。
Status ReplaceTestCopyMembers(MapTestArtifact& artifact, const MapArchiveSession& source,
    const std::vector<MemberReplacement>& members, const wchar_t* name, std::uint64_t maxMapBytes,
    std::string_view subsystem, std::string_view code) {
    const auto staging = std::filesystem::temp_directory_path() / (std::wstring(L"audastra-") + name + L"-"
        + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(staging))
        return Status::Failure(std::string(subsystem), std::string(code) + "_WORKSPACE", "无法创建独占测试副本候选目录。");
    auto saved = source.SaveCandidate(staging / "candidate.w3x", members);
    if (!saved.Succeeded()) return {std::move(saved.diagnostics)};
    auto lease = FreezeMapTestWorkspace(staging);
    if (!lease.Succeeded()) return {std::move(lease.diagnostics)};
    struct Cleanup { std::shared_ptr<MapTestWorkspaceLease> lease; ~Cleanup() { (void)lease->Cleanup(true); } } cleanup{*lease.value};
    const auto target = artifact.workspace / (std::wstring(name) + L".w3x");
    if (!CopyFileW(saved.value->path.c_str(), target.c_str(), TRUE))
        return Status::Failure(std::string(subsystem), std::string(code) + "_COPY_FAILED", "冻结测试副本复制失败。");
    auto copied = ReadVerifiedFile(target, saved.value->sha256, maxMapBytes);
    if (!copied.Succeeded()) return {std::move(copied.diagnostics)};
    artifact.testMap = target;
    artifact.testMapSha256 = saved.value->sha256;
    return Status::Ok();
}
}

Result<MapTestArtifact> PrepareLuaAwareMapTest(const MapTestRequest& request,
    const MapTestSlkConverter& converter, const MapTestProgress& progress) {
    auto prepared = PrepareMapTest(request, converter, progress);
    if (!prepared.Succeeded()) return prepared;
    // 手写 Lua：YDWE 的 Lua 引擎运行时不认 |xxxx| 四字码（2026-11-12 真实 YDWE 实测）。
    // 测试副本里按语法检查的同一规则换成整数，行号不变；地图原文不变。自带环境加载整数写法结果相同。
    if (!request.useOwnedLuaRuntime) {
        auto source = MapArchiveSession::Open(prepared.value->testMap);
        if (!source.Succeeded()) return {std::nullopt, std::move(source.diagnostics)};
        auto luaFiles = CaptureMapLuaFiles(*source.value);
        if (!luaFiles.Succeeded()) return {std::nullopt, std::move(luaFiles.diagnostics)};
        std::vector<MemberReplacement> translated;
        for (const auto& file : *luaFiles.value) {
            const std::string_view text(reinterpret_cast<const char*>(file.bytes->data()), file.bytes->size());
            auto rewritten = Lua::TranslateYdweRawcodes(text);
            if (!rewritten) continue;
            const auto* begin = reinterpret_cast<const std::byte*>(rewritten->data());
            translated.push_back({file.path, std::vector<std::byte>(begin, begin + rewritten->size())});
        }
        if (!translated.empty()) {
            if (progress) progress("lua-rawcodes");
            const auto count = translated.size();
            const auto replaced = ReplaceTestCopyMembers(*prepared.value, *source.value, translated, L"lua-rawcode-test",
                request.maxMapBytes, "Map.Test.Lua", "MAP_TEST_LUA_RAWCODE");
            if (!replaced.Succeeded()) return {std::nullopt, replaced.diagnostics};
            prepared.diagnostics.push_back({DiagnosticSeverity::Info, "Map.Test.Lua", "MAP_TEST_LUA_RAWCODE_TRANSLATED",
                "测试副本里 " + std::to_string(count) + " 个 Lua 文件的 |xxxx| 四字码已换成整数，YDWE 的 Lua 引擎运行时不认这种写法；地图原文不变。"});
        }
    }
    if (!request.useOwnedLuaRuntime && request.handwrittenLuaEngine != MapTestLuaEngine::Owned) return prepared;
    auto source = MapArchiveSession::Open(prepared.value->testMap);
    if (!source.Succeeded()) return {std::nullopt, std::move(source.diagnostics)};
    auto luaFiles = CaptureMapLuaFiles(*source.value);
    if (!luaFiles.Succeeded()) return {std::nullopt, std::move(luaFiles.diagnostics)};
    bool required = false;
    for (const auto& file : *luaFiles.value) {
        const std::string_view text(reinterpret_cast<const char*>(file.bytes->data()), file.bytes->size());
        for (const auto& module : Lua::ScanModuleRequires(text)) if (!module.dynamic && module.name == "jass.slk") required = true;
    }
    if (!required) return prepared;
    if (source.value->HasMember(Audastra::Runtime32::kLuaObjectSnapshotMember))
        return Result<MapTestArtifact>::Failure("Lua.ObjectSnapshot", "LUA_SLK_MEMBER_COLLISION", "测试资料成员已存在，不能覆盖未确认的作者内容。");
    if (progress) progress("lua-object-snapshot");
    auto game = Game::GameDataSource::MountStrict(request.gameRoot);
    if (!game.Succeeded()) return {std::nullopt, std::move(game.diagnostics)};
    auto schema = ObjectData::ObjectFieldSchema::Load(*game.value);
    if (!schema.Succeeded()) return {std::nullopt, std::move(schema.diagnostics)};
    ObjectCatalogSources selection; selection.archive = &*source.value; selection.gameRoot = request.gameRoot;
    auto catalog = LoadEffectiveObjectCatalog(selection);
    if (!catalog.Succeeded()) return {std::nullopt, std::move(catalog.diagnostics)};
    auto documents = ReadMapNativeObjectInputs(*source.value);
    if (!documents.Succeeded()) return {std::nullopt, std::move(documents.diagnostics)};
    LuaObjectSnapshotSources inputs; inputs.schema = &*schema.value; inputs.standard = &catalog.value->Catalog();
    for (unsigned i = 0; i < 7; ++i) inputs.documents[i] = &documents.value->at(i);
    auto misc = game.value->ReadFile("Units/MiscData.txt");
    if (!misc.Succeeded()) return {std::nullopt, std::move(misc.diagnostics)};
    const auto appendMisc = [&](std::span<const std::byte> bytes) {
        Text::VisitProfileFields({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, [&](auto section, auto key, auto value) {
            auto actualSection = std::find_if(inputs.misc.begin(), inputs.misc.end(), [&](const auto& old) { return Text::LowerAscii(old.first) == Text::LowerAscii(section); });
            const auto sectionKey = actualSection == inputs.misc.end() ? std::string(section) : actualSection->first;
            auto& fields = inputs.misc[sectionKey];
            auto actualField = std::find_if(fields.begin(), fields.end(), [&](const auto& old) { return Text::LowerAscii(old.first) == Text::LowerAscii(key); });
            fields[actualField == fields.end() ? std::string(key) : actualField->first] = value;
        }, false);
    };
    appendMisc(*misc.value);
    if (source.value->HasMember("war3mapMisc.txt")) {
        auto overlay = source.value->ReadMember("war3mapMisc.txt");
        if (!overlay.Succeeded()) return {std::nullopt, std::move(overlay.diagnostics)};
        appendMisc(*overlay.value);
    }
    auto data = BuildLuaObjectSnapshot(inputs);
    if (!data.Succeeded()) return {std::nullopt, std::move(data.diagnostics)};
    auto verified = catalog.value->VerifyCapturedSources();
    if (!verified.Succeeded()) return {std::nullopt, std::move(verified.diagnostics)};
    const auto size = data.value->size();
    const std::vector<MemberReplacement> members{{std::string(Audastra::Runtime32::kLuaObjectSnapshotMember), std::move(*data.value)}};
    const auto replaced = ReplaceTestCopyMembers(*prepared.value, *source.value, members, L"lua-object-test",
        request.maxMapBytes, "Lua.ObjectSnapshot", "LUA_SLK");
    if (!replaced.Succeeded()) return {std::nullopt, replaced.diagnostics};
    prepared.diagnostics.push_back({DiagnosticSeverity::Info, "Lua.ObjectSnapshot", "LUA_SLK_SNAPSHOT_READY",
        "jass.slk 使用本次测试图的冻结物编资料（" + std::to_string(size) + "字节），来源含地图原生覆盖与有效数据表。"});
    return prepared;
}
} // namespace War3Map::Services
