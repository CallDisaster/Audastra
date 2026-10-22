#pragma once

#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/SlkDataset.h"
#include "War3MapCore/Script/SlkMapSourceSnapshot.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// YDWE/KKWE Lua 模板（<? 代码 ?> 与 <?= 表达式 ?>）的编辑器内执行。
// 与 YDWE 保存时一样：整份预处理脚本转换成一个 Lua chunk，只执行一次；块外文本原样输出。
// 运行在进程内的 Lua 5.3 沙盒：只有 base/string/table/math/utf8，没有 io/os/package/debug/load，
// 不读写磁盘（import 的路径形式除外，只读地图所在目录）、不启动进程、不加载 dll。
// require 只认 'slk'：原创实现的物编读取/生成库，数据来自 w3x2lni 的 prebuilt 默认值与本次保存的最终地图物编。
// 任何失败都不返回部分结果。
namespace War3Map { class MapArchiveSession; }

namespace War3Map::Script {

struct LuaTemplateSlkSources {
    // 含 plugin/w3x2lni 的根（YDWE 根，或 KKWE 的 share/w3x2lni）。
    std::filesystem::path w3x2lniRoot;
    const SlkMapSourceSnapshot* map = nullptr; // 本次保存的最终成员；须在执行期间存活
    SlkDatasetLayout layout = SlkDatasetLayout::DataSetPrebuilt; // 由 DetectSlkDatasetLayout 确定
};

// 在工具根下查找 w3x2lni 数据根；找不到返回空。
[[nodiscard]] std::optional<std::filesystem::path> FindTemplateW3x2lniRoot(const std::filesystem::path& toolRoot);

struct LuaTemplateLimits {
    std::uint64_t instructions = 4'000'000'000ull;
    std::size_t memoryBytes = 1024ull * 1024 * 1024;
    std::chrono::milliseconds timeout{180000};
    std::size_t outputBytes = 64ull * 1024 * 1024;
};

struct LuaTemplateRequest {
    std::string_view preprocessed;
    // abilityOrder(技能, 类型)；未知命令 ID 按 0 并给出 ABILITY_ORDER_UNKNOWN 警告（与内置求值相同）。
    std::function<std::optional<std::int64_t>(std::string_view ability, std::string_view type)> abilityOrder;
    std::optional<std::filesystem::path> importRoot;
    // 首次 require 'slk' 时调用；未提供或失败时 require 报错。
    std::function<Result<LuaTemplateSlkSources>()> slk;
    // 框架档案已开启的原创模板函数集 ID；未知 ID 拒绝执行。
    std::vector<std::string> libraries;
    LuaTemplateLimits limits;
};

[[nodiscard]] Result<TemplateTransactionOutput> RunLuaTemplate(const LuaTemplateRequest& request);

// 保存链与整图检查共用的整份执行入口（TemplateContext::wholeChunk）：slk 库首次 require 时才按
// archive + edits/removals 捕获最终成员，存入 slkCache 供调用方复用（保存的三方合并使用同一快照）。
// abilityOrder 与 importRoot 取自 templates；archive/edits/removals/slkCache 须在执行期间存活。
struct LuaWholeChunkSources {
    std::filesystem::path toolRoot;
    const MapArchiveSession* archive = nullptr;
    std::span<const MemberReplacement> edits;
    std::span<const std::string> removals;
    std::uint64_t revision = 0;
    std::function<void()> beforeRun; // 每次执行前调用（如报告阶段），可空
};
[[nodiscard]] std::function<Result<TemplateTransactionOutput>(std::string_view, const std::vector<std::string>&)>
    MakeLuaWholeChunk(const TemplateContext& templates, LuaWholeChunkSources sources, std::optional<SlkMapSourceSnapshot>& slkCache);

} // namespace War3Map::Script
