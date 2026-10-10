#pragma once

#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <array>
#include <span>

namespace War3Map::Services {

// 宿主在文档线程借用当前保存基线和可见 overlay；调用期间禁止修改它们。
// binding 覆盖工程身份、重新打开世代和全部当前编辑修订，不从磁盘重读旧来源。
struct ProjectChangesContext {
    const MapDocument* baseline = nullptr;
    const MapDocument* current = nullptr;
    MapDataQueryBinding binding;
};

enum class ProjectChangeKind : std::uint8_t { Member, Field, Added, Removed };
enum class ProjectChangeDomain : std::uint8_t { MapData, Objects, Triggers, Variables };

struct ProjectChangedMember {
    MapDataKind kind = MapDataKind::Settings;
    std::string memberName;
    MapDataState beforeState = MapDataState::Missing, afterState = MapDataState::Missing;
    std::optional<std::string> beforeSha256, afterSha256;
    std::optional<std::size_t> beforeCount, afterCount;
    bool changed = false, partial = false;
    std::string reason;
    ProjectChangeDomain domain = ProjectChangeDomain::MapData;
};

struct ProjectChangeRow {
    std::string id; // 确定性令牌；以成员双 SHA 与 binding 核对，不单独授予写入许可。
    MapDataKind member = MapDataKind::Settings;
    ProjectChangeKind kind = ProjectChangeKind::Member;
    MapDataQueryBinding binding;
    std::optional<std::string> beforeMemberSha256, afterMemberSha256;
    std::optional<std::size_t> baselineIndex, currentIndex;
    std::string rowKey, fieldId, label, detail;
    std::optional<MapData::MapFieldValue> before, after;
    bool revertible = false, textTruncated = false;
    std::string revertReason;
    ProjectChangeDomain domain = ProjectChangeDomain::MapData;
    std::uint64_t editorRevision = 0;
    std::optional<ObjectData::ObjectKey> object;
    std::optional<ObjectData::ModificationKey> modification;
    std::optional<ObjectData::Value> objectBefore, objectAfter;
    StableId entityId = 0;
    std::size_t changedCount = 1;
    std::vector<StableId> changedArguments;
    std::string beforeText, afterText; // 触发器/变量摘要，原始值有界；不作为反向编辑载荷。
};

struct ProjectChangesCursor {
    MapDataQueryBinding binding;
    MapDataKind member = MapDataKind::Settings;
    std::optional<std::string> beforeSha256, afterSha256;
    std::size_t nextOffset = 0;
    ProjectChangeDomain domain = ProjectChangeDomain::MapData;
    ObjectData::ObjectType objectType = ObjectData::ObjectType::Unit;
    std::uint64_t editorRevision = 0;
};
struct ProjectChangesOptions {
    std::size_t limit = 64; // 1..128；输出顺序固定，分页不隐式筛选。
    std::size_t maxRecordsPerMember = 4096; // 1..32768；超限只显示成员级提示，不假定键唯一。
    std::size_t maxLogicalBytes = 256 * 1024; // 1..256 KiB，元数据也计入。
    std::size_t maxTextBytes = 2048; // 1..4096；过长原值省略，并禁用该行反向操作。
    std::size_t maxContentNodes = 262144; // 1..1048576；内容节点/引用工作量，非地图实例上限。
};
struct ProjectChangesPage {
    MapDataQueryBinding binding;
    ProjectChangedMember member;
    std::vector<ProjectChangeRow> rows;
    std::optional<ProjectChangesCursor> next;
    std::size_t logicalBytes = 0, examinedBefore = 0, examinedAfter = 0;
    std::uint64_t editorRevision = 0;
    std::size_t totalRows = 0; // 有界扫描确认的行数；partial 时不冒称完整覆盖。
};

// 计划只是经前值核对的 typed 结果；宿主通过现有 Edit* / EditPlacements 提交为同一事务，
// 再推入已有撤销栈。这里不发布归档、不建立第二套 undo、不扩大保存或字段能力。
struct ProjectChangeReversePlan {
    MapDataQueryBinding binding;
    MapDataKind member = MapDataKind::Settings;
    std::string changeId;
    std::optional<std::string> beforeMemberSha256, afterMemberSha256;
    std::optional<MapData::MapFieldTables> fieldTables;
    std::optional<MapData::UnitPlacements> units;
    std::optional<MapData::DoodadPlacements> doodads;
    ProjectChangeDomain domain = ProjectChangeDomain::MapData;
    std::uint64_t editorRevision = 0;
    std::optional<ObjectData::ObjectType> objectType;
    std::optional<ObjectData::LniObjectDocument> objectDocument;
    struct ArgumentEdit { StableId argumentId; Trigger::ArgumentNode before, after; };
    std::vector<ArgumentEdit> arguments;
};

// 借用已载入的保存基线/可见 overlay；fingerprint 来自同一 EditRevision scopes 的缓存，
// 宿主每次提交前必须复核 binding/editorRevision。unknown 不能用 nullptr 冒充空表。
struct ProjectObjectChangeSource {
    const ObjectData::LniObjectDocument* baseline = nullptr;
    const ObjectData::LniObjectDocument* current = nullptr;
    bool baselineKnown = false, currentKnown = false;
    std::string baselineFingerprint, currentFingerprint;
};
struct ProjectContentChangesContext {
    std::array<ProjectObjectChangeSource, 7> objects;
    const Trigger::TriggerDocument* baselineTriggers = nullptr;
    const Trigger::TriggerDocument* currentTriggers = nullptr;
    bool baselineTriggersKnown = false, currentTriggersKnown = false;
    std::string baselineTriggerFingerprint, currentTriggerFingerprint;
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    MapDataQueryBinding binding;
    std::uint64_t editorRevision = 0;
};
// 与地图数据共用同一条目/分页/反向计划；只展开有界输入，复杂结构保留只读说明。
[[nodiscard]] Result<ProjectChangesPage> QueryProjectContentChanges(
    const ProjectContentChangesContext& context, const MapDataQueryBinding& expected,
    ProjectChangeDomain domain, ObjectData::ObjectType type = ObjectData::ObjectType::Unit,
    const std::optional<ProjectChangesCursor>& cursor = std::nullopt,
    const ProjectChangesOptions& options = {});
[[nodiscard]] Result<ProjectChangeReversePlan> BuildProjectContentReversePlan(
    const ProjectContentChangesContext& context, const ProjectChangeRow& selected,
    const ProjectChangesOptions& options = {});

[[nodiscard]] Result<ProjectChangesPage> QueryProjectChanges(const ProjectChangesContext& context,
    const MapDataQueryBinding& expected, MapDataKind member,
    const std::optional<ProjectChangesCursor>& cursor = std::nullopt,
    const ProjectChangesOptions& options = {});
// 重算所选行并核对当前修订、成员双 SHA、唯一身份与原值；不相信 UI 传回的值/可还原标志。
[[nodiscard]] Result<ProjectChangeReversePlan> BuildProjectChangeReversePlan(
    const ProjectChangesContext& context, const ProjectChangeRow& selected,
    const ProjectChangesOptions& options = {});

} // namespace War3Map::Services
