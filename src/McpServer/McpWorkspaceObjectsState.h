#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <array>
#include <map>
#include <memory>
#include <optional>

namespace War3Map::Mcp {

// 来源快照只拥有实际读取的有效表、游戏字段元数据与地图文档，不保留可变 MPQ 句柄。
struct McpObjectWorkspaceState {
    explicit McpObjectWorkspaceState(std::shared_ptr<const ObjectData::EffectiveObjectCatalog> catalog)
        : effectiveCatalog(std::move(catalog)), standard(effectiveCatalog->Catalog()) {}
    struct GameInput {
        std::optional<std::vector<std::byte>> bytes;
        std::string archive;
    };
    struct MapInput {
        std::filesystem::path relativePath;
        std::shared_ptr<const ObjectData::LniObjectDocument> initial;
        std::shared_ptr<const ObjectData::LniObjectDocument> committed;
        std::optional<ObjectData::NativeObjectDocument> native;
        std::optional<std::vector<std::byte>> nativeBytes;
        std::vector<std::byte> sourceBytes;
        bool nativeSource = false;
        bool sourcePresent = false;
    };
    std::shared_ptr<const ObjectData::EffectiveObjectCatalog> effectiveCatalog;
    // 既有查询/事务只借用同一有效目录，不再复制一份游戏目录。
    const ObjectData::StandardObjectCatalog& standard;
    ObjectData::ObjectFieldSchema schema;
    std::map<std::string, GameInput> gameInputs;
    std::array<MapInput, 7> mapInputs;
    std::unique_ptr<MapDocument> map;
    std::vector<std::vector<std::byte>> scriptBytes;
    std::vector<std::string> scriptNames;
};

} // namespace War3Map::Mcp
