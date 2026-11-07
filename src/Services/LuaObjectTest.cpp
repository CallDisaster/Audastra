#include "War3MapCore/Services/LuaObjectSnapshot.h"
#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "War3MapCore/Runtime/LuaObjectSnapshot.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Game/GameDataSource.h"
#include <algorithm>

namespace War3Map::Services {
Result<MapTestArtifact> PrepareLuaAwareMapTest(const MapTestRequest& request,
    const MapTestSlkConverter& converter, const MapTestProgress& progress) {
    auto prepared = PrepareMapTest(request, converter, progress);
    if (!prepared.Succeeded() || (!request.useOwnedLuaRuntime && request.handwrittenLuaEngine != MapTestLuaEngine::Owned)) return prepared;
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
    auto saved = source.value->SaveCandidate(prepared.value->workspace / "lua-object-test.w3x", members);
    if (!saved.Succeeded()) return {std::nullopt, std::move(saved.diagnostics)};
    prepared.value->testMap = saved.value->path; prepared.value->testMapSha256 = saved.value->sha256;
    prepared.diagnostics.push_back({DiagnosticSeverity::Info, "Lua.ObjectSnapshot", "LUA_SLK_SNAPSHOT_READY",
        "jass.slk 使用本次测试图的冻结物编资料（" + std::to_string(size) + "字节），来源含地图原生覆盖与有效数据表。"});
    return prepared;
}
} // namespace War3Map::Services
