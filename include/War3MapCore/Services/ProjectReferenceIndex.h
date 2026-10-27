#pragma once

#include "War3MapCore/ObjectData/ObjectReferenceIndex.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map { class MapDocument; }
namespace War3Map::Trigger {
class TriggerDocument;
class UiCatalog;
} // namespace War3Map::Trigger

// 工程级静态引用索引（与 ObjectReferenceIndex 合并）：
// 一次遍历触发器语句、物编字段、放置实例与变量初值，建立“记号 → 引用位置”“函数 → 调用位置”
// 与“引用者 → 使用的记号”。物体管理器、物编静态引用与删除前检查共用同一份索引。
// 只收录工程数据中的直接引用；自定义脚本文本与动态拼接不在其中，零命中不能作为安全删除依据。
namespace War3Map::Services {

enum class UsageModule : std::uint8_t { Triggers, Objects, Variables, World };

enum class UsagePlace : std::uint8_t {
    Event,
    Condition,
    Action,
    ObjectParent,
    ObjectField,
    Placement,
    VariableInitial,
};

struct Usage {
    UsageModule module = UsageModule::Triggers;
    // 所在的触发器 / 物体 / 变量；列表按它分组。ownerId 形如 "trigger:<id>"、"object:<类型>:<rawcode>"、
    // "variable:<名称>"、"world"。
    std::string ownerId;
    std::string owner;
    UsagePlace place = UsagePlace::Action;
    // 触发器语句：ECA 节点下标（显示文字由界面按需生成，索引构建不渲染十万条语句）。
    std::optional<std::uint32_t> ecaIndex;
    // 物编字段 ID / 放置实例序号 / 变量初值等补充信息。
    std::string detail;
    std::optional<std::size_t> placementIndex;
    // 放置实例在建索引时的完整身份（文件、序号、类型码、创建编号），世界页按它选中；
    // 实例之后被删除或序号错位时身份不再吻合，由世界页拒绝而不是选到别的实例。特殊装饰物没有可编辑身份，为空。
    struct Placement {
        bool doodads = false; // war3map.doo；否则 war3mapUnits.doo
        std::uint32_t index = 0;
        std::string rawcode;
        std::int32_t creationId = 0;
    };
    std::optional<Placement> placement;
    std::optional<ObjectData::ObjectKey> ownerObject;
    // 所在触发器或语句已禁用：引用仍存在，但不会生成代码。
    bool disabled = false;
    // 目标类型不唯一（例如放置文件不区分单位与物品）：界面标为“可能引用”。
    bool possible = false;
};

struct ProjectReferenceInputs {
    const Trigger::TriggerDocument* triggers = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    std::span<const ObjectData::ObjectRecordSource> objects;
    const MapDocument* map = nullptr;
    const ObjectData::ObjectFieldSchema* schema = nullptr;
};

class ProjectReferenceIndex {
public:
    [[nodiscard]] static ProjectReferenceIndex Build(const ProjectReferenceInputs& inputs);

    [[nodiscard]] std::span<const Usage> Usages() const noexcept { return usages_; }
    // 一组记号（rawcode、变量名、触发器名…）的全部引用，去重；excludeOwnerId 为引用者自身。
    [[nodiscard]] std::vector<const Usage*> UsagesOf(
        std::span<const std::string> tokens, std::string_view excludeOwnerId = {}) const;
    // 某个 UI 函数（原始函数名）的全部调用位置。
    [[nodiscard]] std::vector<const Usage*> CallsOf(std::string_view function) const;
    // 触发器中出现过的全部函数名（函数页目录）。
    [[nodiscard]] std::vector<std::string> CalledFunctions() const;
    // 某个引用者使用到的记号（“使用了”列表）；未知引用者返回空集合。
    [[nodiscard]] const std::set<std::string>& Outgoing(std::string_view ownerId) const;
    // 物编与放置输入的覆盖情况（来自 ObjectReferenceIndex）。
    [[nodiscard]] std::span<const ObjectData::ReferenceInputCoverage> Coverage() const noexcept { return coverage_; }

private:
    std::size_t Add(Usage usage);
    void Link(std::size_t usage, const std::string& token);

    std::vector<Usage> usages_;
    std::unordered_map<std::string, std::vector<std::size_t>> tokens_;
    std::map<std::string, std::vector<std::size_t>> functions_;
    std::unordered_map<std::string, std::set<std::string>> outgoing_;
    std::vector<ObjectData::ReferenceInputCoverage> coverage_;
};

// 物编对象的引用者 ID，用于 UsagesOf 的 excludeOwnerId 与 Outgoing。
[[nodiscard]] std::string ObjectOwnerId(const ObjectData::ObjectKey& key);
[[nodiscard]] std::string TriggerOwnerId(std::uint64_t triggerId);

} // namespace War3Map::Services
