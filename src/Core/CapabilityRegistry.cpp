#include "War3MapCore/CapabilityRegistry.h"

#include <array>

namespace War3Map {
namespace {
using enum CapabilityLevel;
constexpr std::array modules{
    // 2026-09-28 按 CURRENT_STATUS 的证据更新；“往返”指原版编辑器真保存或逐成员比对，游戏内确认另计。
    ModuleCapability{"project", "工程与保存", Supported, Partial, Partial, Partial, "跨进程撤销与崩溃原子替换尚未验证"},
    ModuleCapability{"objects", "物体", Supported, Supported, Partial, Partial, "七类复杂字段已有原版真保存；游戏内确认、模型预览与旧格式编辑待完成"},
    ModuleCapability{"triggers", "传统触发器", Supported, Partial, Partial, Partial, "复杂控制流覆盖与蓝图/代码三视图一致"},
    ModuleCapability{"variables", "全局变量", Partial, Partial, Partial, Partial, "完整初始值、数组与脚本引用重构"},
    ModuleCapability{"resources", "资源与输入", Partial, Partial, Partial, Unavailable, "资源预览、依赖、路径重构与导出"},
    ModuleCapability{"map-settings", "地图设置", Supported, Partial, Partial, Partial, "地图名、数据集可保存；其余字段待原版验证"},
    ModuleCapability{"sounds", "声音", Supported, Partial, Partial, Partial, "音量可保存；其余字段待原版验证，游戏内试听待确认"},
    ModuleCapability{"references", "物体管理", Partial, Unavailable, Unavailable, Unavailable, "跨模块引用索引待接通"},
    ModuleCapability{"blueprint", "蓝图", Partial, Partial, Unavailable, Unavailable, "节点交互与传统/蓝图/代码三视图一致"},
    ModuleCapability{"campaign", "战役", Unavailable, Unavailable, Unavailable, Unavailable, "章节、共享数据与运行"},
    ModuleCapability{"ai", "AI", Unavailable, Unavailable, Unavailable, Unavailable, "RTS AI 文档、编辑与编译"},
    ModuleCapability{"lua", "Lua", Unavailable, Unavailable, Unavailable, Unavailable, "目标版本适配和 Lua 后端"},
    ModuleCapability{"world", "世界与地形", Partial, Partial, Partial, Unavailable, "荒芜与纯高度可保存；贴图/悬崖/水面受通行性门禁，动画与粒子未完成"},
    ModuleCapability{"game-ui", "游戏界面", Partial, Unavailable, Unavailable, Unavailable, "地图自建界面、界面编辑，以及排行榜、多面板、计时器等对话框的游戏代码位置未完成"}
};
}
std::span<const ModuleCapability> CapabilityRegistry::Modules() noexcept { return modules; }
const ModuleCapability* CapabilityRegistry::Find(std::string_view id) noexcept {
    for (const auto& module : modules) if (module.id == id) return &module;
    return nullptr;
}
} // namespace War3Map
