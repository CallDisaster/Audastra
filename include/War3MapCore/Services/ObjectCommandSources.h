#pragma once

#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectCommands.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace War3Map::Services {

// 宿主当前物编文档的复制；空指针表示从同一物理来源捕获，不能伪造缺席。
using ObjectDocumentOverlay = std::array<std::shared_ptr<const ObjectData::LniObjectDocument>, 7>;

// 物编命令的完整来源：同一次捕获的原版目录、字段元数据与地图七类文档（宿主覆盖优先）。
// 结果自持有全部字节和来源租约，不借用请求中的会话或文档指针。
struct ObjectCommandSources {
    std::function<Result<ObjectCommandContext>(ObjectData::ObjectType)> context;
    std::function<Status()> verify; // 发布前复核捕获时的物理来源
};

[[nodiscard]] Result<std::vector<std::string>> ObjectCommandSourceNames();
[[nodiscard]] Result<ObjectCommandSources> CaptureObjectCommandSources(
    const ObjectCatalogSources& sources, ObjectDocumentOverlay overlay = {});

} // namespace War3Map::Services
