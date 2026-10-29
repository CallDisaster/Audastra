#pragma once

#include "War3MapCore/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

enum class MapTestRuntimeLayout { Flat, YdweTree };
// 作者手写 Lua（脚本里 Cheat("exec-lua: \"模块\"")）的测试运行环境。
// Owned：Audastra 自带 Lua，可热重载；地图还需要原版没有的扩展接口时改用 YDWE 引擎并提示。
// Ydwe：始终使用本机 YDWE 的 Lua 引擎（不能热重载）。
enum class MapTestLuaEngine { Owned, Ydwe };
// 经典 1.27a 的 Game.dll；后台运行钩子只认这一版本。
inline constexpr std::string_view kMapTestGame127ModuleSha256 = "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a";

// 既有 UI 设置可指向安装根、ui 或 share/mpq；仅归一这两个已知子目录，不搜索其他父目录。
[[nodiscard]] std::filesystem::path MapTestYdweInstallRoot(const std::filesystem::path& configuredUiRoot);

struct MapTestRequest {
    std::filesystem::path inputMap;
    std::string expectedInputSha256;
    std::filesystem::path sourceMap; // 用于保护打开的原图；可与 inputMap 相同。
    std::filesystem::path gameRoot; // 启动游戏，SLK 优化也从这里读取原版物编表。
    // 必须尚不存在。宿主的冻结保存输入放在其父目录；本服务只拥有该子目录。
    std::filesystem::path workspace;
    bool optimizeSlk = false;
    std::uint64_t maxMapBytes = 1024ull * 1024 * 1024;
    std::filesystem::path runtimeRoot; // 可选的独立扩展插件目录。
    std::filesystem::path runtimeBridge;
    std::filesystem::path runtimeBootstrap;
    // runtimeRoot 留空且地图需要扩展接口时，才只读使用已配置的本机 YDWE 安装。
    std::filesystem::path runtimeYdweRoot;
    // 显式选择自有经典 Lua；不能由 YDWE 路径或热重载请求隐式改变运行时。
    bool useOwnedLuaRuntime = false;
    bool enableLuaHotReload = false;
    MapTestLuaEngine handwrittenLuaEngine = MapTestLuaEngine::Owned;
    bool handwrittenLuaHotReload = true;
};

struct MapTestArtifact {
    std::filesystem::path testMap;
    std::filesystem::path workspace;
    std::filesystem::path gameRoot;
    std::string inputSha256;
    std::string testMapSha256;
    bool optimizedSlk = false;
    std::filesystem::path sourceMap;
    std::filesystem::path inputMap;
    std::filesystem::path runtimeRoot;
    std::filesystem::path runtimeBridge;
    std::filesystem::path runtimeBootstrap;
    std::filesystem::path runtimeYdweRoot;
    MapTestRuntimeLayout runtimeLayout = MapTestRuntimeLayout::Flat;
    bool useOwnedLuaRuntime = false;
    bool enableLuaHotReload = false;
    MapTestLuaEngine handwrittenLuaEngine = MapTestLuaEngine::Owned;
    bool handwrittenLuaHotReload = true;
    // 游戏开在用户看不见的独立桌面上：不弹出、不抢焦点、不接收键鼠（Agent 测试默认）。
    bool hiddenDesktop = false;
    // 无人值守测试：失焦不暂停、后台不降速。只在 1.27a 上生效，其他版本照常启动。
    bool backgroundPlay = false;
    // 看不见但照常绘制：游戏留在用户桌面上，窗口隐身、不进任务栏、不激活，能截图。
    // 自动要求后台运行能力（1.27a）；身份不符则拒绝，不能静默切换到不可见桌面。
    bool cloakedWindow = false;
};

struct MapTestSlkRequest {
    std::filesystem::path inputMap;
    std::filesystem::path outputMap;
    std::filesystem::path gameRoot; // 原版物编表来源；不需要外部工具。
    std::filesystem::path workspace;
    std::string expectedInputSha256;
};

// 可信内部适配器：必须核验有效物编语义，输出只写给定独占目录，禁止降级到未优化副本。
using MapTestSlkConverter = std::function<Status(const MapTestSlkRequest&)>;
using MapTestProgress = std::function<void(std::string_view)>;

// 仅消费冻结后的磁盘输入。复制、优化失败均不改变原图、宿主文档、基线或撤销状态。
// 失败保留独占目录供诊断；成功测试地图须在游戏退出后由宿主安全清理，不能随编辑器析构删除。
[[nodiscard]] Result<MapTestArtifact> PrepareMapTest(
    const MapTestRequest& request, const MapTestSlkConverter& converter = {},
    const MapTestProgress& progress = {});

struct MapTestLaunchPlan {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
    std::filesystem::path workingDirectory;
    std::filesystem::path testMap;
    std::string testMapSha256;
    // 经典游戏需要短 -loadfile 参数；只使用本轮新建的 Maps/Audastra 子目录。
    std::filesystem::path gameMapCopy;
    std::string executableSha256;
    std::string gameExecutableSha256;
    std::string gameModuleSha256;
    std::filesystem::path runtimeRoot;
    std::filesystem::path runtimeReceipt;
    MapTestRuntimeLayout runtimeLayout = MapTestRuntimeLayout::Flat;
    bool useOwnedLuaRuntime = false;
    bool enableLuaHotReload = false;
    // 脚本中实际出现的 exec-lua 入口模块（按出现顺序）；为空表示地图不加载手写 Lua。
    std::vector<std::string> luaEntryModules;
    // 与启动器、Game.dll一同持有到进程终态，防止预检后替换注入库。
    std::filesystem::path runtimeBootstrap;
    std::string runtimeBootstrapSha256;
    // 启动入口及其子进程放在本轮新建的不可见桌面上。
    bool hiddenDesktop = false;
    // 启动器在游戏入口前挂上失焦不暂停与后台不降速；Game.dll 已核对为 1.27a。
    bool backgroundPlay = false;
    // 启动器在游戏建窗口前挂上窗口隐身；这时不再建不可见桌面。
    bool cloakedWindow = false;
};

// 核验所选游戏和本轮短路径复制目标；不启动进程、不写安装配置或注册表。
[[nodiscard]] Result<MapTestLaunchPlan> BuildMapTestLaunchPlan(const MapTestArtifact& artifact);

} // namespace War3Map::Services
