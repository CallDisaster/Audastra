#pragma once

#include "War3MapCore/Types.h"
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    // 按名字导入的函数（导入 DLL 小写名 → 函数名）与按名字的导出（已排序）。
    // 表读不全或超过预算时对应的 Known 为 false，这个模块不参与版本核对，也不因此拒绝启动。
    std::vector<std::pair<std::string, std::vector<std::string>>> namedImports;
    bool namedImportsKnown = false;
    std::vector<std::string> exports;
    bool exportsKnown = false;
};
[[nodiscard]] Result<RuntimeModuleImage> InspectRuntimeModulePe(std::span<const std::byte> bytes);

// 一个计划装入的模块：相对运行库目录的显示路径（如 plugin/warcraft3/yd_loader.dll）与它的 PE。
struct PlannedRuntimeModule {
    std::string label;
    RuntimeModuleImage image;
};
// 计划内的模块之间按名字导入的函数必须在对方的导出表里；否则游戏里 LoadLibrary 以 Win32 127 失败。
// 常见于把不同版本的 YDWE / KKWE 文件混放在同一目录。只读字节，不装入。
[[nodiscard]] Status CheckRuntimeModuleVersions(
    const std::vector<PlannedRuntimeModule>& modules, const std::filesystem::path& root);
}
