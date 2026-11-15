#pragma once

#include "War3MapCore/Types.h"
#include <span>
#include <string_view>
#include <vector>

namespace War3Map::Script {
// 从调用方已核验的同一份文件字节读取，避免散列与 PE 检查分开取样。
[[nodiscard]] Result<std::vector<std::string>> InspectExternalDependencyPe(
    std::span<const std::byte> bytes, std::string_view entry);
// 运行库模块（YDWE / KKWE 插件及其 bin 依赖）：x86 DLL 的导入 DLL 名（小写）与是否导出可执行的 Initialize。
// 原平台插件可以有延迟导入；延迟导入不参与装入排序。
struct RuntimeModuleImage {
    std::vector<std::string> imports;
    bool initialize = false;
};
[[nodiscard]] Result<RuntimeModuleImage> InspectRuntimeModulePe(std::span<const std::byte> bytes);
}
