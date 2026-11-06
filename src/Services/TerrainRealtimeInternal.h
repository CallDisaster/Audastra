#pragma once

#include "War3MapCore/Services/TerrainPathingSave.h"

namespace War3Map::Services {

// Core只持有不透明共享值；原生对象覆盖与来源均不可变，不捕获Qt工程或回调控件。
struct TerrainRealtimeBinding {
    std::shared_ptr<const MapArchiveSession> archive;
    std::shared_ptr<const WorkspaceSnapshot> directory;
    std::filesystem::path gameRoot;
    std::shared_ptr<const std::vector<MemberReplacement>> members;
    std::size_t maximumBytes = 0;
};

} // namespace War3Map::Services
