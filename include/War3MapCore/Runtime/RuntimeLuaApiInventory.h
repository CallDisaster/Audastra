#pragma once

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace War3Map::Runtime {

// 静态证明注册关系，不加载任何 DLL，也不执行 Lua 或地图代码。
struct RuntimeApiSource {
    std::string relativePath;
    std::string sha256;
    std::uint64_t size = 0;
};
struct RuntimeApiField {
    std::string name;
    std::optional<std::uint32_t> argumentCount;
    bool native = false; // 本机 common.j 的真实 native 声明。
    std::uint32_t registrationRva = 0;
    std::uint32_t functionRva = 0;
};
struct RuntimeApiModule {
    std::string name;
    std::uint32_t registrationRva = 0;
    std::uint32_t openFunctionRva = 0;
    bool fieldsComplete = false; // 惰性 __index / 外部注册不授予“字段不存在”。
    bool commonLookup = false; // 真实注册入口包含 C __index，native 签名来自绑定 common.j。
    std::vector<RuntimeApiField> fields;
};
struct RuntimeApiEvidence {
    std::string kind;
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    std::string sha256;
};
struct RuntimeApiInventory {
    bool trusted = false;
    bool modulesComplete = false; // 静态解析不证明无其它动态注册，未列出的模块始终未知。
    bool cancelled = false;
    std::string reason;
    std::string binding;
    std::string method = "static-pe-registration";
    std::vector<RuntimeApiSource> sources;
    std::vector<RuntimeApiModule> modules;
    std::vector<RuntimeApiEvidence> evidence;
    std::vector<std::string> commonGlobals;
};

// root 必须是用户配置的本机安装；读取原始两级路径，不复制或分发运行库。
// common为空时使用root/compiler/jass/24/common.j；六库和common全部有界读取、SHA绑定。
// 返回不可变快照；失败 reason 用于界面“目前只检查语法”。调用应在后台进行。
[[nodiscard]] std::shared_ptr<const RuntimeApiInventory> CaptureRuntimeLuaApiInventory(
    const std::filesystem::path& root, const std::filesystem::path& common = {},
    std::stop_token cancel = {}, std::chrono::steady_clock::time_point deadline =
        (std::chrono::steady_clock::time_point::max)());

// 原创 PE fixture 与文件工具共用同一纯字节解析；来源哈希由调用者的文件捕获入口绑定。
[[nodiscard]] RuntimeApiInventory InspectRuntimeLuaEngine(
    const std::vector<std::byte>& engineBytes, std::stop_token cancel = {},
    std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)());

} // namespace War3Map::Runtime
