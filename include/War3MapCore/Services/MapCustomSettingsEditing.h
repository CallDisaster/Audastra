#pragma once

#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"

#include <functional>
#include <variant>

namespace War3Map::Services {

struct LuaMapTargetRequest;

enum class MapCustomSettingsSection { Upgrades, TechRestrictions, RandomUnits, RandomItems };
enum class MapCustomSettingsAction { Append, Update, Delete };
using MapCustomSettingsValue = std::variant<std::monostate, MapData::MapUpgradeAvailability,
    MapData::MapTechRestriction, MapData::MapRandomUnitTable, MapData::MapRandomItemTable>;

struct MapCustomSettingsCommand {
    MapCustomSettingsSection section = MapCustomSettingsSection::Upgrades;
    MapCustomSettingsAction action = MapCustomSettingsAction::Append;
    std::optional<std::uint32_t> expectedCount;
    std::optional<std::uint32_t> index;
    std::optional<std::int32_t> number;
    std::string expectedKey;
    MapCustomSettingsValue value;
    bool keepName = false;
};
struct MapCustomSettingsEditRequest {
    std::string expectedMemberSha256;
    std::vector<MapCustomSettingsCommand> operations;
};

// 只由宿主建立：对象来自真实层序和当前物编覆盖；verify 在准备及发布前复核来源。
// 协议不接受目录、物编字节、回调或“已验证”标记。
struct MapCustomSettingsValidation {
    std::function<Result<ObjectCommandContext>(ObjectData::ObjectType)> objectContext;
    std::function<Status()> verify;
    // 宿主捕获真实 T 与不透明来源；破坏性编辑不能把缺失覆盖当成无引用。
    std::function<AuxiliaryReferenceContext()> references;
    // 由可信宿主传入实际保存目标；经典Lua不能仅由v25的scriptLanguage字段识别。
    std::function<Status(const MapDocument&)> randomWriteQualification;
};

class MapCustomSettingsEditing;
class PreparedMapCustomSettingsEdit {
public:
    [[nodiscard]] const MapDocument& Before() const noexcept { return *before_; }
    [[nodiscard]] const MapDocument& After() const noexcept { return *after_; }
    [[nodiscard]] bool HasChanges() const noexcept { return changed_; }
    [[nodiscard]] bool CanCommit() const noexcept { return randomAvailable_ || (!randomChanges_ && !initializationBlocked_); }
    [[nodiscard]] bool InitializationBlocked() const noexcept { return initializationBlocked_ && !randomAvailable_; }
    [[nodiscard]] const std::vector<std::string>& StepKeys() const noexcept { return stepKeys_; }
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const noexcept { return diagnostics_; }
private:
    friend class MapCustomSettingsEditing;
    PreparedMapCustomSettingsEdit() = default;
    std::shared_ptr<const MapDocument> before_, after_;
    MapCustomSettingsEditRequest request_;
    std::vector<std::string> stepKeys_;
    std::vector<Diagnostic> diagnostics_;
    bool changed_ = false, randomChanges_ = false, initializationBlocked_ = false;
    bool randomAvailable_ = false;
};

class MapCustomSettingsEditing {
public:
    [[nodiscard]] static Result<PreparedMapCustomSettingsEdit> Prepare(
        std::shared_ptr<const MapDocument> current, const MapCustomSettingsEditRequest& request,
        const MapCustomSettingsValidation& validation);
    [[nodiscard]] static Status Apply(MapData::MapDataEditSession& session,
        const PreparedMapCustomSettingsEdit& plan, const MapCustomSettingsValidation& validation,
        const MapData::MapDataPublishValidator& validator = {});
    [[nodiscard]] static Status WriteQualification(const MapDocument& document);
    // 基础布局/保存目标资格；对象、引用及最终整图初始化仍在Prepare和正常保存中核对。
    [[nodiscard]] static Status RandomWriteQualification(const MapDocument& document,
        const LuaMapTargetRequest* selectedLuaTarget = nullptr);
    [[nodiscard]] static Result<std::string> MemberIdentity(const MapDocument& document);
    [[nodiscard]] static Result<std::string> RecordKey(MapCustomSettingsSection section,
        const MapCustomSettingsValue& record, std::int32_t sourceVersion);
};

// UTF-8 有界协议适配器由 CLI/MCP 复用；不读取主机路径，不发布修改。
[[nodiscard]] Result<MapCustomSettingsEditRequest> ParseMapCustomSettingsEdit(std::string_view json);
[[nodiscard]] Result<std::string> QueryMapCustomSettings(const MapDocument& document,
    const MapDataQueryBinding& binding, std::string_view queryJson);
[[nodiscard]] std::string MapCustomSettingsEditReceipt(const PreparedMapCustomSettingsEdit& plan);

} // namespace War3Map::Services
