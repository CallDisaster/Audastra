#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Services/MapScriptTargetSelection.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

class ObjectTransactionSnapshot;

inline constexpr std::size_t kMapSaveObjectTypeCount = 7;

// 可信宿主完成编辑命令、来源修订及 WPM 门禁后冻结的脚本输入。不得从 CLI/MCP
// 提供任意成员字节直接构造此任务；将来应由共用编辑事务生成同一份最终成员视图。
// 所有指针由任务拥有，MapScriptBuildRequest 仅在执行调用栈内按当前地址建立。
struct MapSaveScriptSnapshot {
    std::unique_ptr<Trigger::TriggerDocument> triggers;
    std::unique_ptr<Trigger::UiCatalog> catalog;
    std::unique_ptr<ObjectData::StandardObjectCatalog> standard;
    std::optional<Game::GameDataSource> game;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kMapSaveObjectTypeCount> editableInputs;
    std::vector<ObjectData::ObjectRecord> units;
    std::vector<ObjectData::ObjectRecord> items;
    std::vector<ObjectData::ObjectRecord> abilities;
    std::vector<std::string> layers;
    Script::ToolchainOptions toolchain;
    // 明确目标及选中W3I由任务拥有；编译成功前不发布语言/脚本成员变更。
    std::optional<PreparedLuaMapTarget> luaTarget;
    std::string scriptMember;
    std::uint64_t documentRevision = 0;
    Script::LuaTriggerBuildOptions luaTestOptions;
};

// 单次地图保存的冻结输入与可核验结果。保留源归档完整快照副本，后台任务不借用
// EditorProject 的归档、触发器、目录或成员 span。Run / Preflight 合计只允许执行一次。
// 这是可信宿主的内部执行任务，不得把可写成员列表直接暴露给 Agent/CLI；
// 宿主必须先完成编辑事务、来源修订、脚本及 W3E/WPM 保存门禁。
struct MapSaveTask {
    MapSaveTask() = default;
    MapSaveTask(const MapSaveTask&) = delete;
    MapSaveTask& operator=(const MapSaveTask&) = delete;

    [[nodiscard]] Status FreezeArchive(const MapArchiveSession& source);

    std::filesystem::path target;
    bool inPlace = false;
    // 宿主显式标记独立测试副本；热重载登记层仅可用于此任务或其只读预检。
    bool testCopyOnly = false;
    InPlaceSaveOptions inPlaceOptions;
    std::unique_ptr<MapArchiveSession> archive;
    std::filesystem::path sourcePath;
    std::string sourceSha256;
    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
    std::array<std::unique_ptr<ObjectData::NativeObjectDocument>, kMapSaveObjectTypeCount> nextBaselines;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kMapSaveObjectTypeCount> generatedObjectBaselines;
    bool triggerChanges = false;
    bool scriptSkipped = false;
    // 仅供地图数据类的窄事务：脚本模板可以运行，但不得顺带修改或新增七类物编。
    // 比较生成 native 与冻结源成员的完整字节，合并/写候选前失败。
    bool requireGeneratedNativeByteEquality = false;
    // 宿主保存的是作者已有地图时开启：改动不影响脚本就沿用源脚本。新建地图的种子脚本只是占位，不能开启。
    bool preserveUnaffectedScript = false;
    // 窄物编事务只允许模板重现这些封印候选；其余类别继续和源成员逐字节一致。
    std::vector<std::shared_ptr<const ObjectTransactionSnapshot>> objectTransactions;
    std::optional<MapSaveScriptSnapshot> script;
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjectReferences;
    std::filesystem::path mapObjectReferenceGameRoot;
    // 冻结后的有效函数库层序；脚本完成时 script 会释放，回执仍须能核对层序。
    std::vector<std::string> scriptLayers;
    // 工作目录在脚本快照释放后仍供最终提交点标记与有界清理使用。
    std::filesystem::path scriptWorkspace;

    // 宿主事务 lease 的最终复核：脚本完成后、归档发布前执行。失败保留全部
    // 冻结输入并拒绝写出；源归档本身仍由归档服务独立验证。
    std::function<Status()> beforePublish;

    std::vector<Diagnostic> diagnostics;
    bool executed = false;
    bool succeeded = false;
    bool scriptRebuilt = false;
    // 已重建并核对：本次改动不影响脚本，写出的是源地图原脚本（scriptRebuilt 同时为真）。
    bool scriptPreserved = false;
    double scriptSeconds = 0;
    std::size_t memberCount = 0;
    std::filesystem::path written;
    std::filesystem::path backup;
    std::size_t prunedBackups = 0;
    std::unique_ptr<MapArchiveSession> nextSession;
    // 实际本次编译的生成单元；整份脚本构建和保存检查通过后宿主才可作为本局基线。
    std::optional<Script::LuaTriggerUnit> generatedLuaTestUnit;
    // 经典Lua bootstrap实际安装的全图符号白名单；一局测试内不得随成功ACK扩大。
    std::optional<std::vector<std::string>> luaTestInstalledBindings;
    // 同一保存链实际生成或明确移除的脚本成员；其余作者脚本仍参与地图内容绑定。
    std::vector<std::string> generatedScriptMembers;
    std::string luaTestMapBinding;
};

class MapSaveService final {
public:
    using Progress = std::function<void(std::string_view stage)>;

    // 仅执行可信宿主已冻结的任务。源 SHA 由 MapArchiveSession 再验证；脚本、模板、
    // 物编替换预检、归档回读与 AfterSave 属于同一执行路径。成功前不切换调用方基线。
    static void Run(MapSaveTask& task, const Progress& progress = {});

    // 消费独立冻结任务，复用保存检查及脚本重建；不发布候选，不 AfterSave / 切换基线。
    // 允许 target 为空，此时只检查内容与来源。非空另存目标还检查现有路径状态。
    // 编译链仍可写独占临时目录，并非完全零磁盘写入；不证明输出、回读或原地备份成功。
    // 返回诊断，task.executed=true；task.succeeded 只代表 Run，预检成功也保持 false。
    [[nodiscard]] static Status Preflight(MapSaveTask& task, const Progress& progress = {});
};

} // namespace War3Map::Services
