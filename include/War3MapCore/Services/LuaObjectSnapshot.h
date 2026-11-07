#pragma once
#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include <array>
#include <map>

namespace War3Map::Services {
struct LuaObjectSnapshotSources {
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    const ObjectData::StandardObjectCatalog* standard = nullptr;
    std::array<const ObjectData::LniObjectDocument*, 7> documents{};
    std::map<std::string, std::map<std::string, std::string>> misc;
};
// 只读冻结资料，不接受 Lua 求值，不为未知物编字段填默认值。
[[nodiscard]] Result<std::vector<std::byte>> BuildLuaObjectSnapshot(const LuaObjectSnapshotSources& sources);
// UI/CLI/MCP 的同一测试准备链；只在独立测试图需要 jass.slk 时追加资料。
[[nodiscard]] Result<MapTestArtifact> PrepareLuaAwareMapTest(const MapTestRequest& request,
    const MapTestSlkConverter& converter = {}, const MapTestProgress& progress = {});
} // namespace War3Map::Services
