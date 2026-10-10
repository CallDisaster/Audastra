#pragma once

#include "War3MapCore/Script/MapTestToolchain.h"

namespace War3Map::Script::detail {

// 可信宿主内部执行入口：生产只允许BuildMapTestLaunchPlan成功后调用。
// 不注册CLI/MCP参数；原创console夹具只用已拥有的临时可执行文件验证Job生命周期。
[[nodiscard]] Result<MapTestLaunchReceipt> LaunchVerifiedMapTestPlan(const Services::MapTestLaunchPlan& plan);

} // namespace War3Map::Script::detail
