#pragma once

#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <functional>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script {

// 需要物编 / 游戏数据才能回答的问题。未提供时使用保守的默认规则（见各字段注释）。
struct ObjectQueries {
    // 放置物是否为物品；默认：rawcode 不是单位 / 建筑即视为单位（不产生物品）。
    std::function<bool(std::string_view rawcode)> isItem;
    // 单位是否为建筑（进入 CreateBuildingsForPlayer）；默认否。
    std::function<bool(std::string_view rawcode)> isBuilding;
    // 单位是否为英雄；默认：rawcode 首字母为大写字母。
    std::function<bool(std::string_view rawcode)> isHero;
    // 技能信息：是否存在、是否英雄技能、是否可自动施法及开 / 关命令串。
    // 未提供时：技能都存在且按英雄技能处理，仅对标记了自动施法的技能写空命令串。
    struct AbilityInfo {
        bool known = true;
        bool hero = true;
        bool autocast = false;
        bool custom = false;
        std::string orderOn;
        std::string orderOff;
    };
    std::function<AbilityInfo(std::string_view abilityRawcode)> ability;
    // 单位类型自带的队伍颜色（-1 表示跟随所有者）；未提供时视为 -1。
    std::function<int(std::string_view unitRawcode)> unitTeamColor;
    // 游戏声音标签与时长（毫秒）；未提供时省略 SetSoundParamsFromLabel / SetSoundDuration。
    std::function<std::optional<std::string>(std::string_view path)> soundLabel;
    std::function<std::optional<int>(std::string_view path)> soundDuration;
};

// 地形范围（war3map.w3e 头部）：顶点数与左下角坐标。WE 用它和 w3i 的镜头边距计算 SetCameraBounds。
struct TerrainExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

// 与经典地形读取范围一致：2..2049 顶点，有限的世界偏移和终点，绝对值不超过 2^24。
[[nodiscard]] bool ValidTerrainExtent(const TerrainExtent& extent) noexcept;
// 只读取经典 v11 w3e 头部并验证范围；不证明后续顶点数据完整。
[[nodiscard]] std::optional<TerrainExtent> ReadTerrainExtent(std::span<const std::byte> w3e);

// 生成整图脚本所需的地图数据。缺失的可选成员按“地图没有这类对象”处理；
// 触发器与函数库目录必须提供。所有指针只在生成期间使用。
struct MapScriptInputs {
    ObjectQueries queries;
    const Trigger::TriggerDocument* triggers = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const MapData::MapSettings* settings = nullptr;
    const MapData::UnitPlacements* units = nullptr;
    // 最终 war3map.doo；脚本化可破坏物的全局变量必须绑定此成员中的实例。
    const MapData::DoodadPlacements* doodads = nullptr;
    const MapData::RegionsDocument* regions = nullptr;
    const MapData::CamerasDocument* cameras = nullptr;
    const MapData::SoundsDocument* sounds = nullptr;
    const MapData::StringTableDocument* strings = nullptr;
    // 缺失时镜头范围退回 w3i 中记录的镜头边界。
    std::optional<TerrainExtent> terrain;
    // 头注释中的生成日期；为空时省略日期行（便于测试得到稳定输出）。
    std::string generatedDate;
    // 调试版（与工具链的 DEBUG 定义一致）：触发器初始化时写 YDWESaveTriggerName 的展开。
    bool debug = false;
    // 句柄类型的 YDTrigger 存储写法随 YDWE 版本不同：较新版本的 YDTrigger/SaveLoadSystem/HTSystem.j
    // 把它包装为库函数 YDTRIGGER_SET_<type>（存 null 时改为清除）；没有这个文件的版本（如 1.32.13）
    // 直接展开为 Save<Type>Handle。为真时使用前者。由 BuildMapScript 按工具根目录的实际头文件决定。
    bool nullSafeHandleSet = false;
};

// 生成结果：等价于 WE + YDTrigger 输出的脚本。YDTrigger 的存储 / 执行触发器 / 循环等宏已直接展开，
// 只剩开头引入 YDTrigger 库的两三行指令，以及值位置按 YDWE 规则写出、由函数库宏改名的定义名。
// 语法由后续预处理、vJass 与 JASS 检查阶段负责；本步保证结构与命名一致。
// 由实际写出位置记录作者正文；0 表示 WCT 全局头，其余使用触发器稳定身份。
// 范围只覆盖原正文，排除补换行、生成器横幅与初始化函数，不能通过字符串搜索重建。
struct MapScriptCodeSpan {
    StableId triggerId = 0;
    std::size_t byteOffset = 0;
    std::size_t byteLength = 0;
};

struct MapScriptResult {
    std::string text;
    std::vector<Diagnostic> diagnostics;
    std::size_t triggerCount = 0;
    std::size_t functionCount = 0;
    std::vector<MapScriptCodeSpan> authorCode;

    [[nodiscard]] bool Succeeded() const noexcept {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.severity == DiagnosticSeverity::Error) return false;
        }
        return true;
    }
};

[[nodiscard]] MapScriptResult GenerateMapScript(const MapScriptInputs& inputs);

struct GuiTriggerSource {
    std::string text;
    std::string triggerVariable;
    std::string initFunction;
    bool effectiveRunOnInit = false;
    bool usesStorage = false;
};
// 显式 GUI → 作者代码使用整图保存同一生产输出器，不使用基础对照 JASS 后端。
[[nodiscard]] Result<GuiTriggerSource> GenerateGuiTriggerSource(
    const MapScriptInputs& inputs, StableId triggerId);

// 代码转回 T 的候选。fromSnapshot：代码未改动，按转换前快照原样恢复（含禁用行）。
struct GuiTriggerRecovery {
    Trigger::GuiTriggerFragment fragment;
    bool fromSnapshot = false;
};
// 代码转回 T：先试转换前快照，再按生产生成器的写法解析当前正文；候选必须经同一生成器回生成、
// 与正文逐词一致（空白不计，辅助函数可整体改名）才返回，否则指出第一处不一致或无法识别的行。
[[nodiscard]] Result<GuiTriggerRecovery> RecoverGuiTrigger(const MapScriptInputs& inputs, StableId triggerId,
    const Trigger::GuiTriggerFragment* snapshot = nullptr);

// WE 对触发器 / 区域 / 声音等对象名称的标识符转换（非字母数字字节替换为 '_'，结尾补 'u'）。
[[nodiscard]] std::string ConvertObjectName(std::string_view name);

} // namespace War3Map::Script
