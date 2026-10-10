#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Trigger {
class TriggerDocument;
class UiCatalog;
enum class UiDefinitionKind : std::uint8_t;
} // namespace War3Map::Trigger

// UI 函数库的两层启用（架构文档 §4.2–4.4）。
// 本层只做发现、指纹、比对与有效层序计算，不做持久化：全局设置由编辑器本机设置保存，
// 地图记录由工程文件保存。任何情况下都不修改数据根中的 config，也不执行包内程序。
namespace War3Map::UiLibrary {

// 数据根中发现的一个包。id 为相对 ui 根的路径（正斜杠），例如 "9527/SJWE"。
struct PackageInfo {
    std::string id;
    bool listedInConfig = false;
    std::size_t configOrder = 0;   // 在 config 中的位置；未列出时无意义
    std::string fingerprint;       // 包内 .txt/.j/.cfg 的相对路径与内容 SHA-256 的聚合指纹
    std::size_t fileCount = 0;
    std::uint64_t byteCount = 0;
};

// 扫描 ui 根：config 列出的包（按 config 顺序）在前，目录中含定义文件但未列出的包在后。
// 包的判定：目录内含 define/event/condition/action/call.txt 之一，或旧式 ui/TriggerData.txt。
[[nodiscard]] Result<std::vector<PackageInfo>> DiscoverPackages(const std::filesystem::path& toolRoot);

// ── 全局（本机）层 ──
struct GlobalEntry {
    std::string id;
    bool enabled = false;
    std::string fingerprint;   // 上次确认时的内容指纹
};

// 顺序即加载顺序（后加载覆盖前者）。
struct GlobalSettings {
    std::vector<GlobalEntry> packages;
};

// 把已保存设置与本次发现合并：保留用户的启用状态与顺序；新发现且列在 config 中的包启用，
// 未列出的包关闭；本机已不存在的包从设置中移除。首次使用（空设置）即等价于 config。
[[nodiscard]] GlobalSettings ReconcileGlobal(
    const GlobalSettings& saved, std::span<const PackageInfo> discovered);

// ── 地图（工程）层 ──
struct MapUsageEntry {
    std::string id;
    std::string fingerprint;
    std::size_t functionCount = 0;   // 本图引用的该包定义数（去重后的函数名）
};

struct MapUsage {
    std::vector<MapUsageEntry> packages;   // 按有效加载顺序
};

// 从已解析的触发器文档统计本图用到的包（ECA 节点记录了其定义来源层）。
[[nodiscard]] MapUsage ComputeMapUsage(
    const Trigger::TriggerDocument& document, std::span<const PackageInfo> discovered);

enum class PackageStatus : std::uint8_t {
    Same,             // 本机可用且内容一致
    Changed,          // 本机可用，内容指纹与地图记录不同
    Missing,          // 本机没有这个包
    DisabledLocally,  // 本机有，但全局设置中已关闭
};

struct PackageCheck {
    std::string id;
    PackageStatus status = PackageStatus::Same;
    std::string recordedFingerprint;
    std::string localFingerprint;
};

// 打开地图时逐包比对地图记录与本机状态。
[[nodiscard]] std::vector<PackageCheck> CheckMapUsage(
    const MapUsage& usage, const GlobalSettings& global, std::span<const PackageInfo> discovered);

// 有效层序：全局启用的包按全局顺序；mapOnly 中的包（地图用到但本机关闭，用户选择“仅对此
// 地图启用”）按其在全局设置中的位置插入，不改变全局设置。本机不存在的包不会出现。
[[nodiscard]] std::vector<std::string> EffectiveLayers(
    const GlobalSettings& global, std::span<const std::string> mapOnly,
    std::span<const PackageInfo> discovered);

// ── 冲突 ──
struct DefinitionConflict {
    Trigger::UiDefinitionKind kind;
    std::string group;
    std::string name;
    std::vector<std::string> layers;   // 提供该定义的层，按加载顺序
    std::string activeLayer;           // 最终生效的层
};

// 同名定义由多个层提供时列出，标明最终生效的来源。
[[nodiscard]] std::vector<DefinitionConflict> FindConflicts(const Trigger::UiCatalog& catalog);

[[nodiscard]] std::string_view ToString(PackageStatus status) noexcept;

} // namespace War3Map::UiLibrary
