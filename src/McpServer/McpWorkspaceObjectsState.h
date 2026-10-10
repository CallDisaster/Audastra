#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <array>
#include <map>
#include <memory>
#include <optional>

namespace War3Map::Mcp {

// 来源快照只拥有实际读取的标准表、元数据与地图文档，不保留可变 MPQ 句柄。
struct McpObjectWorkspaceState {
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
    ObjectData::StandardObjectCatalog standard;
    ObjectData::ObjectFieldSchema schema;
    std::map<std::string, GameInput> gameInputs;
    std::array<MapInput, 7> mapInputs;
    std::unique_ptr<MapDocument> map;
    std::vector<std::vector<std::byte>> scriptBytes;
    std::vector<std::string> scriptNames;
};

} // namespace War3Map::Mcp
