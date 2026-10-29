#include "McpWorkspace.h"
#include "McpWorkspaceObjectsState.h"
#include "McpToolInternal.h"
#include "War3MapCore/Script/TriggerStorageCapabilities.h"

#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace War3Map::Mcp {
namespace {

constexpr std::string_view kWarVKManifestSha256 =
    "920872221b3836a5eff69d3ec721915b21e0c4b5399c0f09f05b028cf46d27bf";
constexpr std::string_view kWarVKWrapperSha256 =
    "e222e2044ecd3763292c576df34926a0cc6fe954618cc0e3f2fab7bdf31c2606";
constexpr std::string_view kWarVKAutoTestTriggerName = "WarVK_AutoTest";

constexpr std::string_view kWarVKAutoTestFooter = R"WARVK(

// War3MapEditor64 manifest-bound AutoTest footer. Test candidate only.
// It waits for the independently installed C++ carrier/backend before issuing
// the one-shot local-visual proof sequence.
function WarVK_AutoTest_Run takes nothing returns nothing
    local timer testTimer = GetExpiredTimer()
    local string version
    local integer protocol
    local integer flags
    local integer lightId
    local integer errorCode
    local boolean alive
    local string aliveText = "0"
    local real x
    local real y
    if not WarVKIsRuntimeReady() then
        set testTimer = null
        return
    endif
    call PauseTimer(testTimer)
    call DestroyTimer(testTimer)
    set version = WarVKGetVersion()
    set protocol = WarVKGetProtocolVersion()
    set flags = WarVKGetFeatureFlags()
    set x = GetCameraTargetPositionX()
    set y = GetCameraTargetPositionY()
    set lightId = WarVKCreatePointLight(x, y, 320.00, 1200.00, 0.20, 0.55, 1.00, 5.00)
    call WarVKSetPointLightEnabled(lightId, true)
    call WarVKSetPointLightPosition(lightId, x, y, 360.00)
    call WarVKSetPointLightColorIntensity(lightId, 0.20, 0.55, 1.00, 5.00)
    call WarVKSetPointLightRadius(lightId, 1200.00)
    call WarVKSetPointLightShadowConfig(lightId, 1024, 0.05)
    call WarVKSetPointLightShadowEnabled(lightId, true)
    set alive = WarVKIsPointLightAlive(lightId)
    if alive then
        set aliveText = "1"
    endif
    set errorCode = WarVKGetLastErrorCode()
    call DisplayTimedTextToPlayer(GetLocalPlayer(), 0.00, 0.00, 60.00, "WARVK_CPP_CALL version=" + version + " protocol=" + I2S(protocol) + " flags=" + I2S(flags) + " lightId=" + I2S(lightId) + " alive=" + aliveText + " error=" + I2S(errorCode))
    set testTimer = null
endfunction

function Trig_WarVK_AutoTest_Actions takes nothing returns nothing
    local timer testTimer = CreateTimer()
    call TimerStart(testTimer, 0.25, true, function WarVK_AutoTest_Run)
    set testTimer = null
endfunction

function InitTrig_WarVK_AutoTest takes nothing returns nothing
    set gg_trg_WarVK_AutoTest = CreateTrigger()
    call TriggerAddAction(gg_trg_WarVK_AutoTest, function Trig_WarVK_AutoTest_Actions)
endfunction
)WARVK";

Result<std::vector<std::byte>> ReadBytes(const std::filesystem::path& path) {
    auto result = IO::ReadFileBytes(path, std::numeric_limits<std::uint64_t>::max());
    if (result.Succeeded()) return result;
    const std::string& code = result.diagnostics.front().code;
    const bool size = code == "FILE_SIZE_FAILED" || code == "FILE_SIZE_LIMIT";
    const bool read = code == "FILE_SEEK_FAILED" || code == "FILE_READ_FAILED"
        || code == "FILE_CHANGED" || code == "FILE_READ_EXCEPTION";
    return Result<std::vector<std::byte>>::Failure("MCP",
        size ? "MCP_SOURCE_SIZE_FAILED" : read ? "MCP_SOURCE_READ_FAILED" : "MCP_SOURCE_OPEN_FAILED",
        (size ? "无法读取地图成员长度：" : read ? "无法完整读取地图成员：" : "无法只读打开地图成员：")
            + path.string());
}

template <typename T>
Result<T> FailureFrom(const Status& status) {
    Result<T> result;
    result.diagnostics = status.diagnostics;
    return result;
}

template <typename T, typename U>
Result<T> FailureFrom(const Result<U>& source) {
    Result<T> result;
    result.diagnostics = source.diagnostics;
    return result;
}

bool ArchiveExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".w3x" || extension == ".w3m";
}

std::optional<std::string_view> Field(
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::string_view name) {
    const auto found = std::find_if(
        fields.begin(), fields.end(),
        [name](const auto& field) { return field.first == name; });
    if (found == fields.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace

McpWorkspace::McpWorkspace(
    std::filesystem::path projectRoot,
    std::filesystem::path uiDataRoot,
    std::filesystem::path wtgRelativePath,
    std::filesystem::path wctRelativePath,
    Trigger::UiCatalog catalog,
    Trigger::TriggerDocument document,
    EditorSession session)
    : projectRoot_(std::move(projectRoot)),
      uiDataRoot_(std::move(uiDataRoot)),
      wtgRelativePath_(std::move(wtgRelativePath)),
      wctRelativePath_(std::move(wctRelativePath)),
      catalog_(std::move(catalog)),
      committedDocument_(std::move(document)),
      session_(std::move(session)) {
}

McpWorkspace::McpWorkspace(McpWorkspace&&) noexcept = default;
McpWorkspace& McpWorkspace::operator=(McpWorkspace&&) noexcept = default;
McpWorkspace::~McpWorkspace() = default;

Result<std::unique_ptr<McpWorkspace>> McpWorkspace::Open(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& uiDataRoot, const std::filesystem::path& gameRoot) {
    std::error_code error;
    if (!std::filesystem::is_directory(projectRoot, error) || error) {
        return Result<std::unique_ptr<McpWorkspace>>::Failure(
            "MCP", "MCP_WORKSPACE_NOT_DIRECTORY",
            "MCP 工作区必须是存在的展开地图目录。");
    }
    if (!std::filesystem::is_directory(uiDataRoot, error) || error) {
        return Result<std::unique_ptr<McpWorkspace>>::Failure(
            "MCP", "MCP_UI_ROOT_NOT_DIRECTORY",
            "MCP 写入模式需要存在的 Warcraft/YDWE UI 数据根目录。");
    }

    auto catalog = Trigger::UiCatalog::Load(uiDataRoot);
    if (!catalog.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(catalog);
    }

    std::filesystem::path wtgRelative = "map/war3map.wtg";
    std::filesystem::path wctRelative = "map/war3map.wct";
    if (!std::filesystem::is_regular_file(projectRoot / wtgRelative, error) || error ||
        !std::filesystem::is_regular_file(projectRoot / wctRelative, error) || error) {
        error.clear();
        wtgRelative = "war3map.wtg";
        wctRelative = "war3map.wct";
    }
    if (!std::filesystem::is_regular_file(projectRoot / wtgRelative, error) || error ||
        !std::filesystem::is_regular_file(projectRoot / wctRelative, error) || error) {
        return Result<std::unique_ptr<McpWorkspace>>::Failure(
            "MCP", "MCP_TRIGGER_PAIR_MISSING",
            "工作区必须同时包含 war3map.wtg 与 war3map.wct；禁止单文件编辑。");
    }

    auto wtg = ReadBytes(projectRoot / wtgRelative);
    auto wct = ReadBytes(projectRoot / wctRelative);
    if (!wtg.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(wtg);
    }
    if (!wct.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(wct);
    }

    Trigger::WtgWctCodec codec;
    auto parsed = codec.Parse(*wtg.value, *wct.value, *catalog.value);
    if (!parsed.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(parsed);
    }
    const Status triggerStatus = codec.Validate(*parsed.value, *catalog.value);
    if (!triggerStatus.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(triggerStatus);
    }

    auto session = EditorSession::Open(OpenRequest{
        projectRoot,
        CompatibilityProfile::Default(),
        catalog.value->Sha256(),
    });
    if (!session.Succeeded()) {
        return FailureFrom<std::unique_ptr<McpWorkspace>>(session);
    }

    auto workspace = std::unique_ptr<McpWorkspace>(new McpWorkspace(
            projectRoot,
            uiDataRoot,
            std::move(wtgRelative),
            std::move(wctRelative),
            std::move(*catalog.value),
            std::move(*parsed.value),
            std::move(*session.value)));
    workspace->gameRoot_ = gameRoot;
    // Open 是显式接受来源的入口；分页只借用这一有界准备，不按每个查询重建全目录类型系统。
    const auto typePreparation = workspace->PrepareProjectSearchTypesCache();
    auto result = Result<std::unique_ptr<McpWorkspace>>::Success(std::move(workspace));
    if (!typePreparation.Succeeded()) {
        result.diagnostics = typePreparation.diagnostics;
        for (auto& diagnostic : result.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
    }
    return result;
}

Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> McpWorkspace::MapObjectReferences() const {
    const auto environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) return {{}, environment.diagnostics};
    if (!mapObjectReferences_) {
        auto captured = Services::CaptureDirectoryMapObjectReferences(projectRoot_, gameRoot_);
        if (!captured.Succeeded() || !captured.value) return captured;
        mapObjectReferences_ = *captured.value;
    }
    auto result = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>::Success(mapObjectReferences_);
    result.diagnostics = mapObjectReferences_->Diagnostics();
    return result;
}

ProjectRevision McpWorkspace::Revision() const {
    return session_.Revision();
}

bool McpWorkspace::HasActiveTransaction() const noexcept {
    return activeEdit_.has_value();
}

TransactionId McpWorkspace::ActiveTransactionId() const noexcept {
    return activeEdit_.has_value() ? activeEdit_->id : 0;
}

const Trigger::TriggerDocument& McpWorkspace::VisibleDocument() const noexcept {
    return activeEdit_.has_value() ? activeEdit_->document : committedDocument_;
}

const Trigger::UiCatalog& McpWorkspace::Catalog() const noexcept {
    return catalog_;
}

EditorSession* McpWorkspace::Session() noexcept {
    return &session_;
}

Status McpWorkspace::EnvironmentStatus() const {
    return CheckEnvironmentUnchanged();
}

Result<TransactionId> McpWorkspace::BeginEdit(
    const ProjectRevision& expectedRevision) {
    if (publishing_) {
        return Result<TransactionId>::Failure("MCP", "MCP_PUBLISH_IN_PROGRESS",
            "归档候选正在发布；不能重入或变更活动事务。");
    }
    if (activeEdit_.has_value()) {
        return Result<TransactionId>::Failure(
            "MCP", "MCP_TRANSACTION_ALREADY_ACTIVE",
            "当前 MCP 工作区已有写事务；必须先导出或回滚。");
    }
    if (expectedRevision != session_.Revision()) {
        return Result<TransactionId>::Failure(
            "MCP", "MCP_REVISION_CONFLICT",
            "expected_revision 与当前项目 revision 不一致。");
    }
    const Status environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) {
        return FailureFrom<TransactionId>(environment);
    }
    // 大型 WTG 的工作副本可能分配失败。先在获取跨进程 writer lease 前完成
    // 全部可抛出的复制，避免异常留下无法由调用方回滚的隐形活动事务。
    ActiveEdit edit{
        0,
        expectedRevision,
        committedDocument_,
    };
    if (objectState_) {
        for (std::size_t i = 0; i < edit.objects.size(); ++i) {
            const auto& input = objectState_->mapInputs[i];
            if (!ObjectData::DiffObjects(input.initial->Objects(), input.committed->Objects()).Empty())
                edit.objects[i] = input.committed;
        }
    }
    auto started = session_.BeginEdit(expectedRevision);
    if (!started.Succeeded()) {
        return started;
    }
    const Status leasedCatalog = CheckCatalogUnchanged();
    if (!leasedCatalog.Succeeded()) {
        const Status rolledBack = session_.Rollback(*started.value);
        if (!rolledBack.Succeeded()) {
            Result<TransactionId> result;
            result.diagnostics = leasedCatalog.diagnostics;
            result.diagnostics.insert(
                result.diagnostics.end(),
                rolledBack.diagnostics.begin(),
                rolledBack.diagnostics.end());
            return result;
        }
        return FailureFrom<TransactionId>(leasedCatalog);
    }
    edit.id = *started.value;
    // 目录候选发布后仍以原来源打开；下一事务必须携带此前保留的完整物编编辑。
    for (std::size_t i = 0; i < edit.objects.size(); ++i) {
        if (!edit.objects[i]) continue;
        const auto seeded = ApplyObjectOverlay(static_cast<ObjectData::ObjectType>(i), *edit.objects[i], edit.id);
        if (!seeded.Succeeded()) {
            auto failure = FailureFrom<TransactionId>(seeded);
            const auto rolled = session_.Rollback(edit.id);
            failure.diagnostics.insert(failure.diagnostics.end(), rolled.diagnostics.begin(), rolled.diagnostics.end());
            return failure;
        }
        ++edit.generation;
    }
    const auto seededMapData = SeedMapDataOverlay(edit);
    if (!seededMapData.Succeeded()) {
        auto failure = FailureFrom<TransactionId>(seededMapData);
        const auto rolled = session_.Rollback(edit.id);
        failure.diagnostics.insert(failure.diagnostics.end(), rolled.diagnostics.begin(), rolled.diagnostics.end());
        return failure;
    }
    const auto seededTerrain = SeedTerrainOverlay(edit);
    if (!seededTerrain.Succeeded()) {
        auto failure = FailureFrom<TransactionId>(seededTerrain);
        const auto rolled = session_.Rollback(edit.id);
        failure.diagnostics.insert(failure.diagnostics.end(), rolled.diagnostics.begin(), rolled.diagnostics.end());
        return failure;
    }
    activeEdit_.emplace(std::move(edit));
    ++toolEpoch_;
    return started;
}

Status McpWorkspace::CheckActive(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision) const {
    const Status identity = CheckIdentity(transactionId, expectedRevision);
    if (!identity.Succeeded()) {
        return identity;
    }
    return CheckEnvironmentUnchanged();
}

Status McpWorkspace::CheckIdentity(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision) const {
    if (publishing_) {
        return Status::Failure("MCP", "MCP_PUBLISH_IN_PROGRESS",
            "归档候选正在发布；不能编辑、导出或回滚活动事务。");
    }
    if (!activeEdit_.has_value() || activeEdit_->id != transactionId) {
        return Status::Failure(
            "MCP", "MCP_TRANSACTION_NOT_ACTIVE",
            "transaction_id 不属于当前活动写事务。");
    }
    if (expectedRevision != activeEdit_->baseRevision ||
        expectedRevision != session_.Revision()) {
        return Status::Failure(
            "MCP", "MCP_REVISION_CONFLICT",
            "expected_revision 与活动事务或当前项目 revision 不一致。");
    }
    return Status::Ok();
}

Status McpWorkspace::CheckEnvironmentUnchanged() const {
    const Status source = session_.VerifySourceUnchanged();
    if (!source.Succeeded()) {
        return source;
    }
    if (mapObjectReferences_) {
        const auto objects = Services::VerifyDirectoryMapObjectReferenceClassificationSources(*mapObjectReferences_, projectRoot_, gameRoot_);
        if (!objects.Succeeded()) return objects;
    }
    const auto objectSources = CheckObjectSourcesUnchanged();
    if (!objectSources.Succeeded()) return objectSources;
    return CheckCatalogUnchanged();
}

Status McpWorkspace::CheckCatalogUnchanged() const {
    auto currentCatalog = Trigger::UiCatalog::Load(uiDataRoot_);
    if (!currentCatalog.Succeeded()) {
        Status status;
        status.diagnostics = std::move(currentCatalog.diagnostics);
        return status;
    }
    if (currentCatalog.value->Sha256() != catalog_.Sha256()) {
        return Status::Failure(
            "MCP", "MCP_UI_CATALOG_CHANGED",
            "UI Catalog 自打开后已变化；活动事务必须回滚，禁止继续写入或导出。");
    }
    return Status::Ok();
}

Result<std::string> McpWorkspace::ExpectedArgumentType(
    const Trigger::TriggerDocument& document,
    StableId argumentId) const {
    const Trigger::EcaNode* owner = nullptr;
    std::size_t ownerOrdinal = 0;
    std::unordered_set<std::uint32_t> visitedEcas;
    std::unordered_set<std::uint32_t> visitedArguments;
    std::function<Status(std::uint32_t, StableId)> visitEca;
    std::function<Status(std::uint32_t, StableId)> visitArgument;
    visitArgument = [&](std::uint32_t index, StableId triggerId) -> Status {
        if (index >= document.argumentNodes.size() ||
            !visitedArguments.insert(index).second) {
            return Status::Failure(
                "MCP", "MCP_TRIGGER_GRAPH_INVALID",
                "参数图包含越界、循环或多重引用，不能推导目标类型。");
        }
        const Trigger::ArgumentNode& argument = document.argumentNodes[index];
        if (argument.callEca.has_value()) {
            const Status nested = visitEca(*argument.callEca, triggerId);
            if (!nested.Succeeded()) return nested;
        }
        if (argument.indexArgument.has_value()) {
            const Status nested = visitArgument(*argument.indexArgument, triggerId);
            if (!nested.Succeeded()) return nested;
        }
        return Status::Ok();
    };
    visitEca = [&](std::uint32_t index, StableId triggerId) -> Status {
        if (index >= document.ecaNodes.size() || !visitedEcas.insert(index).second) {
            return Status::Failure(
                "MCP", "MCP_TRIGGER_GRAPH_INVALID",
                "ECA 图包含越界、循环或多重引用，不能推导目标类型。");
        }
        const Trigger::EcaNode& eca = document.ecaNodes[index];
        if (eca.sourceTriggerId != triggerId) {
            return Status::Failure(
                "MCP", "MCP_TRIGGER_GRAPH_INVALID",
                "ECA sourceTriggerId 与实际可达根不一致。");
        }
        for (std::size_t ordinal = 0; ordinal < eca.arguments.size(); ++ordinal) {
            const std::uint32_t argumentIndex = eca.arguments[ordinal];
            if (argumentIndex >= document.argumentNodes.size()) {
                return Status::Failure(
                    "MCP", "MCP_TRIGGER_GRAPH_INVALID",
                    "ECA 参数索引越界，不能推导目标参数类型。");
            }
            if (document.argumentNodes[argumentIndex].id == argumentId) {
                if (owner != nullptr) {
                    return Status::Failure(
                        "MCP", "MCP_ARGUMENT_OWNER_AMBIGUOUS",
                        "目标参数被多个可达 ECA 引用，不能安全推导类型。");
                }
                owner = &eca;
                ownerOrdinal = ordinal;
            }
            const Status argumentStatus = visitArgument(argumentIndex, triggerId);
            if (!argumentStatus.Succeeded()) return argumentStatus;
        }
        for (std::uint32_t child : eca.children) {
            const Status childStatus = visitEca(child, triggerId);
            if (!childStatus.Succeeded()) return childStatus;
        }
        return Status::Ok();
    };
    for (const Trigger::TriggerRecord& trigger : document.triggers) {
        for (std::uint32_t root : trigger.ecas) {
            const Status graph = visitEca(root, trigger.id);
            if (!graph.Succeeded()) {
                Result<std::string> result;
                result.diagnostics = graph.diagnostics;
                return result;
            }
        }
    }
    if (owner == nullptr) {
        return Result<std::string>::Failure(
            "MCP", "MCP_ARGUMENT_OWNER_MISSING",
            "目标参数没有可验证的 ECA/父 call 所有者。");
    }
    const Trigger::UiDefinitionKind kind =
        Trigger::UiDefinitionKindFromEcaType(owner->type);
    const Trigger::UiDefinition* definition = catalog_.Find(kind, owner->name.text);
    if (definition == nullptr) {
        return Result<std::string>::Failure(
            "MCP", "MCP_ARGUMENT_CATALOG_OWNER_INVALID",
            "目标参数的 owner 在当前 Catalog 中不存在。");
    }
    if (owner->uiSignature.empty() || owner->uiSignature != definition->Signature()) {
        return Result<std::string>::Failure(
            "MCP", "MCP_ARGUMENT_CATALOG_SIGNATURE_MISMATCH",
            "目标参数 owner 的密封签名与当前 Catalog 不一致。");
    }
    std::vector<std::string_view> materialTypes;
    for (const Trigger::UiArgumentDefinition& argument : definition->Active().arguments) {
        if (argument.type != "nothing") {
            materialTypes.push_back(argument.type);
        }
    }
    if (materialTypes.size() != owner->arguments.size() ||
        ownerOrdinal >= materialTypes.size() || materialTypes[ownerOrdinal].empty()) {
        return Result<std::string>::Failure(
            "MCP", "MCP_ARGUMENT_CATALOG_SHAPE_MISMATCH",
            "owner 参数数量或序号与当前 Catalog 不一致，已拒绝猜测类型。");
    }
    return Result<std::string>::Success(std::string(materialTypes[ownerOrdinal]));
}

Status McpWorkspace::ValidateScalarValue(
    const Trigger::TriggerDocument& document,
    std::string_view expectedType,
    std::int32_t valueKind,
    std::string_view value) const {
    if (valueKind == 0) {
        const Trigger::UiDefinition* preset = catalog_.Find(
            Trigger::UiDefinitionKind::Define, value, "TriggerParams");
        if (preset == nullptr) {
            return Status::Failure(
                "MCP", "MCP_PRESET_NOT_ALLOWED",
                "预设必须是当前 Catalog 的 TriggerParams 定义名；物编对象编号（如技能 A04W）、文本和自定义代码请用 value_kind=3 直接值。");
        }
        const auto raw = Field(preset->Active().fields, "value");
        // 引号不成对的定义不可信：拒绝，而不是按宽松切列猜测。
        const bool balanced = raw.has_value() && std::count(raw->begin(), raw->end(), '"') % 2 == 0;
        const std::vector<std::string> columns = balanced
            ? Trigger::SplitTriggerDataCsv(*raw) : std::vector<std::string>{};
        if (columns.size() < 4 || columns[1] != expectedType) {
            return Status::Failure(
                "MCP", "MCP_PRESET_TYPE_MISMATCH",
                "预设类型与目标参数类型不完全匹配。");
        }
        return Status::Ok();
    }
    if (valueKind == 1) {
        constexpr std::string_view kVariablePrefix = "udg_";
        if (!value.starts_with(kVariablePrefix) || value.size() == kVariablePrefix.size()) {
            return Status::Failure(
                "MCP", "MCP_VARIABLE_REFERENCE_INVALID",
                "变量引用必须是已声明用户变量的唯一 udg_<name> 形式。");
        }
        const std::string_view declaredName = value.substr(kVariablePrefix.size());
        const Trigger::VariableRecord* variable = nullptr;
        for (const Trigger::VariableRecord& candidate : document.variables) {
            if (candidate.name.text != declaredName) {
                continue;
            }
            if (variable != nullptr) {
                return Status::Failure(
                    "MCP", "MCP_VARIABLE_AMBIGUOUS",
                    "同名变量不唯一，不能安全绑定参数。");
            }
            variable = &candidate;
        }
        if (variable == nullptr) {
            return Status::Failure(
                "MCP", "MCP_VARIABLE_UNKNOWN",
                "当前事务里没有变量 " + std::string(declaredName) + "（引用写 udg_ 加变量名，变量名本身不含 udg_）。");
        }
        if (variable->isArray != 0) {
            return Status::Failure(
                "MCP", "MCP_VARIABLE_ARRAY_UNSUPPORTED",
                "数组变量 " + std::string(declaredName) + " 不能作为普通标量参数；请用数组取值函数。");
        }
        if (variable->typeName.text != expectedType) {
            return Status::Failure(
                "MCP", "MCP_VARIABLE_TYPE_MISMATCH",
                "变量 " + std::string(declaredName) + " 的类型是 " + variable->typeName.text +
                "，目标参数需要 " + std::string(expectedType) + "。");
        }
        return Status::Ok();
    }
    if (valueKind != 3) {
        return Status::Failure(
            "MCP", "MCP_ARGUMENT_KIND_INVALID",
            "标量参数类型只能是预设、变量或直接值。");
    }
    // 直接值与界面、typed 图共用同一套字面量规则：整数/实数/布尔/文本/自定义代码/物编对象编号（如技能 'A04W'
    // 写作 A04W）。没有固定值写法的类型（单位、点等句柄）仍拒绝。
    const Trigger::TriggerTypeSystem types(catalog_);
    if (types.LiteralKind(expectedType) == Trigger::TriggerTypeSystem::Literal::Other) {
        return Status::Failure(
            "MCP", "MCP_DIRECT_TYPE_UNSUPPORTED",
            "参数类型 " + std::string(expectedType) + " 没有固定值写法；请改用预设、变量或函数调用。");
    }
    return types.ValidateLiteral(expectedType, value);
}

Status McpWorkspace::SealDocument(
    Trigger::TriggerDocument candidate,
    std::string summary) {
    const auto terrain = CheckTerrainMixingAllowed();
    if (!terrain.Succeeded()) return terrain;
    const Status validation = codec_.Validate(candidate, catalog_);
    if (!validation.Succeeded()) {
        return validation;
    }
    auto patch = codec_.BuildPatch(
        candidate,
        catalog_,
        wtgRelativePath_,
        wctRelativePath_,
        std::move(summary));
    if (!patch.Succeeded()) {
        Status status;
        status.diagnostics = std::move(patch.diagnostics);
        return status;
    }
    Status applied = session_.ApplyTriggerPatch(
        activeEdit_->id,
        std::move(*patch.value));
    if (!applied.Succeeded()) {
        return applied;
    }
    activeEdit_->document = std::move(candidate);
    ++activeEdit_->generation;
    ++toolEpoch_;
    activeEdit_->previewedGeneration.reset();
    activeEdit_->validatedGeneration.reset();
    return Status::Ok();
}

Result<StableId> McpWorkspace::CreateTrigger(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    std::int32_t categoryId,
    std::string name) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return FailureFrom<StableId>(active);
    }
    Trigger::TriggerDocument candidate = activeEdit_->document;
    auto created = Trigger::TriggerDocumentEditor::AddTrigger(
        candidate, categoryId, std::move(name));
    if (!created.Succeeded()) {
        return created;
    }
    const Status sealed = SealDocument(
        std::move(candidate), "MCP 新建普通 GUI 触发器");
    if (!sealed.Succeeded()) {
        return FailureFrom<StableId>(sealed);
    }
    return created;
}

Result<StableId> McpWorkspace::InstallGeneratedWarVKModule(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    std::int32_t categoryId,
    VerifiedWarVKModule module) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return FailureFrom<StableId>(active);
    }
    if (module.sourceManifestSha256 != kWarVKManifestSha256 ||
        module.wrapperSha256 != kWarVKWrapperSha256) {
        return Result<StableId>::Failure(
            "MCP", "MCP_WARVK_GENERATION_UNSUPPORTED",
            "WarVK 生成物不属于编辑器固定支持的 v1 manifest 代际。");
    }
    if (module.wrapperText.empty() || module.wrapperText.size() > 64U * 1024U ||
        module.wrapperText.find('\0') != std::string::npos) {
        return Result<StableId>::Failure(
            "MCP", "MCP_WARVK_WRAPPER_INVALID",
            "WarVK wrapper 必须是非空、无 NUL 且不超过 64 KiB 的固定生成文本。");
    }
    const auto wrapperHash = Sha256Text(module.wrapperText);
    if (!wrapperHash.Succeeded() || *wrapperHash.value != module.wrapperSha256 ||
        !module.wrapperText.starts_with(
            "// Generated from the WarVK v1 manifest.")) {
        return Result<StableId>::Failure(
            "MCP", "MCP_WARVK_WRAPPER_HASH_MISMATCH",
            "WarVK wrapper 内容与固定生成物 SHA-256 不一致。");
    }
    for (const std::string_view requiredFunction : {
             "function WarVKGetVersion ",
             "function WarVKIsRuntimeReady ",
             "function WarVKCreatePointLight ",
             "function WarVKSetPointLightShadowEnabled ",
             "function WarVKIsPointLightAlive "}) {
        if (module.wrapperText.find(requiredFunction) == std::string::npos) {
            return Result<StableId>::Failure(
                "MCP", "MCP_WARVK_WRAPPER_CONTRACT_MISSING",
                "固定 WarVK wrapper 缺少 AutoTest 所需的公开函数。");
        }
    }

    Trigger::TriggerDocument candidate = activeEdit_->document;
    const auto duplicate = std::find_if(
        candidate.triggers.begin(), candidate.triggers.end(),
        [](const Trigger::TriggerRecord& trigger) {
            return trigger.name.text == kWarVKAutoTestTriggerName;
        });
    if (duplicate != candidate.triggers.end()) {
        return Result<StableId>::Failure(
            "MCP", "MCP_WARVK_TRIGGER_ALREADY_EXISTS",
            "当前地图已包含 WarVK_AutoTest；禁止重复安装或覆盖旧触发器。");
    }
    auto created = Trigger::TriggerDocumentEditor::AddTrigger(
        candidate, categoryId, std::string(kWarVKAutoTestTriggerName));
    if (!created.Succeeded()) {
        return created;
    }
    Trigger::TriggerRecord* const trigger =
        Trigger::TriggerDocumentEditor::FindTrigger(candidate, *created.value);
    if (trigger == nullptr) {
        return Result<StableId>::Failure(
            "MCP", "MCP_WARVK_TRIGGER_CREATE_INCOMPLETE",
            "WarVK AutoTest 触发器创建后无法按稳定 ID 找回。");
    }
    if (!module.wrapperText.ends_with('\n')) {
        module.wrapperText.push_back('\n');
    }
    module.wrapperText.append(kWarVKAutoTestFooter);
    trigger->enabled = 1;
    trigger->customTextFlag = 1;
    trigger->runOnInit = 1;
    trigger->ecas.clear();
    trigger->customText.text = std::move(module.wrapperText);
    trigger->customText.originalBytes.clear();

    const Status sealed = SealDocument(
        std::move(candidate),
        "MCP 安装固定 WarVK v1 wrapper 与本地视觉 AutoTest");
    if (!sealed.Succeeded()) {
        return FailureFrom<StableId>(sealed);
    }
    return created;
}

Result<Trigger::EcaAppendReceipt> McpWorkspace::AppendRootEca(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    StableId triggerId,
    Trigger::UiDefinitionKind kind,
    std::string functionName) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return FailureFrom<Trigger::EcaAppendReceipt>(active);
    }
    Trigger::TriggerDocument candidate = activeEdit_->document;
    const Trigger::UiDefinition* definition = catalog_.Find(kind, functionName);
    if (definition == nullptr) {
        return Result<Trigger::EcaAppendReceipt>::Failure(
            "MCP", "MCP_ROOT_ECA_NOT_CREATABLE",
            "根 ECA 必须来自当前 Catalog 的 event/condition/action 定义。");
    }
    auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(
        candidate,
        catalog_,
        triggerId,
        kind,
        functionName,
        std::nullopt);
    if (!appended.Succeeded()) {
        return appended;
    }
    const Status sealed = SealDocument(
        std::move(candidate), "MCP 追加 GUI ECA：" + functionName);
    if (!sealed.Succeeded()) {
        return FailureFrom<Trigger::EcaAppendReceipt>(sealed);
    }
    return appended;
}

Result<Trigger::EcaAppendReceipt> McpWorkspace::AppendArgumentCall(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    StableId argumentId,
    std::string functionName) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return FailureFrom<Trigger::EcaAppendReceipt>(active);
    }
    Trigger::TriggerDocument candidate = activeEdit_->document;
    auto expectedType = ExpectedArgumentType(candidate, argumentId);
    if (!expectedType.Succeeded()) {
        return FailureFrom<Trigger::EcaAppendReceipt>(expectedType);
    }
    auto appended = Trigger::TriggerDocumentEditor::AppendArgumentCall(
        candidate,
        catalog_,
        argumentId,
        *expectedType.value,
        functionName);
    if (!appended.Succeeded()) {
        return appended;
    }
    const Status sealed = SealDocument(
        std::move(candidate), "MCP 追加嵌套调用：" + functionName);
    if (!sealed.Succeeded()) {
        return FailureFrom<Trigger::EcaAppendReceipt>(sealed);
    }
    return appended;
}

Status McpWorkspace::SetScalarArgument(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    StableId argumentId,
    std::int32_t valueKind,
    std::string value) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return active;
    }
    Trigger::TriggerDocument candidate = activeEdit_->document;
    const auto preplacedName = [](std::int32_t kind, std::string_view name) {
        return kind == 1 && (name.starts_with("gg_unit_") || name.starts_with("gg_item_"));
    };
    const auto previous = std::find_if(candidate.argumentNodes.begin(), candidate.argumentNodes.end(),
        [&](const auto& argument) { return argument.id == argumentId; });
    // 旧标量入口也转发共用图事务；前缀只选择校验路径，不能提供对象存在/类型资格。
    if (preplacedName(valueKind, value) || (previous != candidate.argumentNodes.end()
        && preplacedName(previous->type, previous->value.text))) {
        if (previous == candidate.argumentNodes.end())
            return Status::Failure("MCP", "MCP_ARGUMENT_OWNER_MISSING", "目标参数不存在。");
        const auto references = MapObjectReferences();
        if (!references.Succeeded() || !references.value) return {references.diagnostics};
        const auto binding = Trigger::CaptureTriggerGraphBinding(candidate, catalog_, references.value->get());
        if (!binding.Succeeded() || !binding.value) return {binding.diagnostics};
        Trigger::TriggerGraphEditBatch batch;
        batch.expected = *binding.value;
        Trigger::GraphSet edit;
        edit.target = {Trigger::GraphRecordKind::Argument, argumentId, {}};
        edit.field = Trigger::GraphField::ArgumentScalar;
        edit.expected = Trigger::GraphScalarValue{previous->type, previous->value.text};
        edit.value = Trigger::GraphScalarValue{valueKind, std::move(value)};
        batch.operations.emplace_back(std::move(edit));
        const auto edited = ApplyGraphEdit(transactionId, expectedRevision, batch);
        return {edited.diagnostics};
    }
    auto expectedType = ExpectedArgumentType(candidate, argumentId);
    if (!expectedType.Succeeded()) {
        Status status;
        status.diagnostics = std::move(expectedType.diagnostics);
        return status;
    }
    const Status scalar = ValidateScalarValue(
        candidate, *expectedType.value, valueKind, value);
    if (!scalar.Succeeded()) {
        return scalar;
    }
    const Status changed = Trigger::TriggerDocumentEditor::SetScalarArgument(
        candidate, argumentId, valueKind, std::move(value));
    if (!changed.Succeeded()) {
        return changed;
    }
    return SealDocument(std::move(candidate), "MCP 修改 GUI 标量参数");
}

Result<Trigger::TriggerGraphEditReceipt> McpWorkspace::ApplyGraphEdit(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    const Trigger::TriggerGraphEditBatch& batch,
    const GraphEditPublishValidator& prepublish, bool dryRun) {
    const auto active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) return FailureFrom<Trigger::TriggerGraphEditReceipt>(active);
    Trigger::TriggerDocument candidate = activeEdit_->document;
    const auto objects = MapObjectReferences();
    if (!objects.Succeeded() && !batch.expected.mapObjectReferencesSha256.empty())
        return FailureFrom<Trigger::TriggerGraphEditReceipt>(objects);
    const auto* references = !batch.expected.mapObjectReferencesSha256.empty() && objects.Succeeded() && objects.value
        ? objects.value->get() : nullptr;
    Trigger::TriggerGraphEditLimits limits;
    limits.storageCapabilityProvider = [&](std::span<const std::string> names) {
        const auto root = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        if (!root.Succeeded()) return Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>>::Failure(
            "Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "宿主未配置实际工具链根，不能确认扩展存取支持。");
        return Script::CaptureMapTriggerStorageCapabilities(catalog_, *root.value, uiDataRoot_, projectRoot_, names);
    };
    auto edited = Trigger::ApplyTriggerGraphEditBatch(candidate, catalog_, batch, limits, references);
    if (!edited.Succeeded()) return edited;
    if (prepublish) {
        const auto checked = prepublish(activeEdit_->document, candidate, *edited.value);
        if (!checked.Succeeded()) return FailureFrom<Trigger::TriggerGraphEditReceipt>(checked);
    }
    if (dryRun || (edited.value->before.currentWtgSha256 == edited.value->after.currentWtgSha256
        && edited.value->before.currentWctSha256 == edited.value->after.currentWctSha256)) return edited;
    const auto sealed = SealDocument(std::move(candidate), "MCP 通用完整 GUI 图批次编辑");
    if (!sealed.Succeeded()) return FailureFrom<Trigger::TriggerGraphEditReceipt>(sealed);
    if (references) activeEdit_->usedMapObjectReferences = *objects.value;
    return edited;
}

Result<SemanticDiff> McpWorkspace::PreviewEdit(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return FailureFrom<SemanticDiff>(active);
    }
    auto preview = session_.PreviewEdit(transactionId);
    if (preview.Succeeded()) {
        activeEdit_->previewedGeneration = activeEdit_->generation;
    }
    return preview;
}

ValidationReport McpWorkspace::ValidateEdit(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision) {
    ValidationReport report;
    report.requestedLevel = CompatibilityLevel::Parsed;
    report.highestProvenLevel = CompatibilityLevel::Parsed;
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        report.diagnostics = active.diagnostics;
        return report;
    }
    if (activeEdit_->previewedGeneration != activeEdit_->generation) {
        report.diagnostics = Status::Failure(
            "MCP", "MCP_PREVIEW_REQUIRED",
            "必须先显式预览当前 edit generation，才能执行验证。").diagnostics;
        return report;
    }
    const Status codecStatus = codec_.Validate(activeEdit_->document, catalog_);
    if (!codecStatus.Succeeded()) {
        report.diagnostics = codecStatus.diagnostics;
        return report;
    }
    if (activeEdit_->mapData) {
        const auto checked = ValidateMapDataOverlay();
        if (!checked.Succeeded()) { report.diagnostics = checked.diagnostics; return report; }
    }
    const auto terrain = ValidateTerrainOverlay();
    if (!terrain.Succeeded()) { report.diagnostics = terrain.diagnostics; return report; }
    report = session_.Validate(
        transactionId,
        session_.Profile(),
        CompatibilityLevel::Parsed);
    if (report.satisfied) {
        activeEdit_->validatedGeneration = activeEdit_->generation;
    }
    return report;
}

Status McpWorkspace::ExportCandidate(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    const std::filesystem::path& outputPath) {
    const Status active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) {
        return active;
    }
    if (ArchiveExtension(outputPath)) {
        return Status::Failure(
            "MCP", "MCP_ARCHIVE_EXPORT_UNAVAILABLE",
            "此出口只提交展开目录；归档候选必须显式绑定源地图、SHA 和工具档案，"
            "通过 save_map_edit_candidate 携带同一事务身份走共用脚本保存服务。");
    }
    if (activeEdit_->previewedGeneration != activeEdit_->generation ||
        activeEdit_->validatedGeneration != activeEdit_->generation) {
        return Status::Failure(
            "MCP", "MCP_CURRENT_GENERATION_NOT_PROVEN",
            "导出要求当前 edit generation 已显式 preview_edit 并通过 validate_edit。");
    }
    const Status codecStatus = codec_.Validate(activeEdit_->document, catalog_);
    if (!codecStatus.Succeeded()) {
        return codecStatus;
    }
    if (activeEdit_->auxiliary) {
        const auto auxiliary = ValidateAuxiliaryOverlay();
        if (!auxiliary.Succeeded()) return auxiliary;
    }
    const auto terrain = ValidateTerrainOverlay();
    if (!terrain.Succeeded()) return terrain;
    const ValidationReport validation = session_.Validate(
        transactionId, session_.Profile(), CompatibilityLevel::Parsed);
    if (!validation.satisfied) {
        Status status;
        status.diagnostics = validation.diagnostics;
        if (status.diagnostics.empty()) {
            status = Status::Failure(
                "MCP", "MCP_VALIDATION_REQUIRED",
                "候选导出前必须通过当前可证明的 L0 验证。");
        }
        return status;
    }
    Trigger::TriggerDocument committed = activeEdit_->document;
    Status status = session_.CommitCandidate(
        transactionId,
        expectedRevision,
        outputPath,
        [this, expectedSourceSha256 = expectedRevision.sourceSha256]() {
            const auto catalog = CheckCatalogUnchanged();
            if (!catalog.Succeeded()) return catalog;
            // CommitCandidate 已持锁核对修订；回调使用该冻结身份，避免重入会话锁。
            return CheckObjectSourcesUnchanged(expectedSourceSha256);
        });
    if (!status.Succeeded()) {
        return status;
    }
    committedDocument_ = std::move(committed);
    CommitTerrainOverlay();
    if (activeEdit_->mapData) {
        committedMapData_ = activeEdit_->mapData;
        committedPlacementIntent_ = activeEdit_->placementIntent;
        committedAuxiliary_ = activeEdit_->auxiliary;
    }
    if (objectState_) for (std::size_t i = 0; i < activeEdit_->objects.size(); ++i)
        if (activeEdit_->objects[i]) objectState_->mapInputs[i].committed = activeEdit_->objects[i];
    activeEdit_.reset();
    ++toolEpoch_;
    projectSearchMapCache_.reset();
    return Status::Ok();
}

Status McpWorkspace::CheckArchivePublishingIdentity(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    std::uint64_t generation,
    const Trigger::TriggerTransactionSnapshot& snapshot,
    const std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>& objects) const {
    // 不调用 CheckActive：发布 lease 正在阻止它接受编辑和回滚重入。
    if (!publishing_ || !activeEdit_ || activeEdit_->id != transactionId ||
        activeEdit_->baseRevision != expectedRevision || session_.Revision() != expectedRevision ||
        activeEdit_->generation != generation || activeEdit_->previewedGeneration != generation ||
        activeEdit_->validatedGeneration != generation || snapshot.Binding().transactionId != transactionId ||
        snapshot.Binding().baseRevision != expectedRevision || snapshot.Binding().editGeneration != generation ||
        snapshot.Receipt().catalogSha256 != catalog_.Sha256() || snapshot.EffectiveLayers() != catalog_.Layers()) {
        return Status::Failure("MCP", "MCP_ARCHIVE_PUBLISH_STALE",
            "归档发布前的活动事务、generation、目录修订或 Catalog 身份已变化。");
    }
    const auto environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) return environment;
    // Session() 的内部调用者也不能绕过工作区 generation 注入额外 overlay。
    // 每个成员须属于同一冻结领域事务；不接受通过 Session() 注入的任意 overlay。
    const auto preview = session_.PreviewEdit(transactionId);
    if (!preview.Succeeded()) return {preview.diagnostics};
    if (preview.value->baseRevision != expectedRevision || preview.value->changes.size() > 2 + objects.size()) {
        return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH",
            "活动 overlay 不再属于冻结 WTG/WCT 与物编领域事务。");
    }
    const auto objectOverlay = CheckObjectOverlay(*preview.value, objects);
    if (!objectOverlay.Succeeded()) return objectOverlay;
    bool wtg = false, wct = false;
    const auto& receipt = snapshot.Receipt();
    for (const auto& change : preview.value->changes) {
        if (change.kind == "object") continue;
        if (change.kind == "trigger" && change.path == wtgRelativePath_ && !wtg &&
            change.beforeSha256 == receipt.originalWtgSha256 && change.afterSha256 == receipt.savedWtgSha256) {
            wtg = true;
        } else if (change.kind == "trigger" && change.path == wctRelativePath_ && !wct &&
            change.beforeSha256 == receipt.originalWctSha256 && change.afterSha256 == receipt.savedWctSha256) {
            wct = true;
        } else {
            return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH",
                "活动 overlay 的成员或 before/after SHA 不属于冻结 GUI 事务。");
        }
    }
    if ((!wtg && receipt.originalWtgSha256 != receipt.savedWtgSha256) ||
        (!wct && receipt.originalWctSha256 != receipt.savedWctSha256)) {
        return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH",
            "冻结事务中已改变的配对成员没有对应的活动 overlay；未改变成员允许省略。");
    }
    return Status::Ok();
}

Result<Services::MapEditSaveReceipt> McpWorkspace::ExportArchiveCandidate(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    Services::MapEditSaveRequest request) {
    const auto active = CheckActive(transactionId, expectedRevision);
    if (!active.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(active);
    if (activeEdit_->previewedGeneration != activeEdit_->generation ||
        activeEdit_->validatedGeneration != activeEdit_->generation) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_CURRENT_GENERATION_NOT_PROVEN",
            "归档导出要求当前 generation 已显式 preview_edit 并通过 validate_edit。");
    }
    if (!request.terrainStrokes.empty() || !request.fieldEdits.empty() ||
        !std::holds_alternative<std::monostate>(request.placementEdit) || request.triggerEdits ||
        request.triggerTransaction || request.auxiliaryTransaction || !request.objectTransactions.empty() || !request.expectedTerrainSha256.empty() || request.beforePublish) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_REQUEST_SCOPE_INVALID",
            "完整 GUI 事务出口不接受额外领域操作、调用者快照或发布回调。");
    }
    if (request.sourceMap.empty() || !ArchiveExtension(request.sourceMap) ||
        request.expectedSourceSha256.size() != 64 ||
        !std::all_of(request.expectedSourceSha256.begin(), request.expectedSourceSha256.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        })) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_SOURCE_REQUIRED",
            "必须显式绑定 .w3x/.w3m 源地图与完整 SHA-256，不能从展开目录修订推断整图身份。");
    }
    if (request.targetCandidate.empty() || !ArchiveExtension(request.targetCandidate)) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_CANDIDATE_REQUIRED",
            "归档候选必须明确指定独立的 .w3x/.w3m 路径。");
    }
    std::error_code error;
    if (!request.uiRoot.empty() && (!std::filesystem::equivalent(request.uiRoot, uiDataRoot_, error) || error)) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_UI_ROOT_MISMATCH",
            "归档保存不能切换活动工作区的实际 UI 数据根。");
    }
    if (request.effectiveLayers && *request.effectiveLayers != catalog_.Layers()) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_UI_LAYERS_MISMATCH",
            "归档保存的有效 UI 层序必须与活动 Catalog 完全一致。");
    }
    request.uiRoot = uiDataRoot_;
    request.effectiveLayers = catalog_.Layers();
    publishing_ = true;
    struct PublishLease {
        bool& publishing;
        ~PublishLease() { publishing = false; }
    } lease{publishing_};
    try {
        const auto validation = session_.Validate(transactionId, session_.Profile(), CompatibilityLevel::Parsed);
        if (!validation.satisfied) {
            if (!validation.diagnostics.empty()) return FailureFrom<Services::MapEditSaveReceipt>(Status{validation.diagnostics});
            return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_VALIDATION_REQUIRED",
                "归档候选冻结前的当前目录事务验证未通过。");
        }
        if (HasTerrainOverlay())
            return ExportTerrainArchiveCandidate(transactionId, expectedRevision, std::move(request));
        if (activeEdit_->mapData) {
            if (activeEdit_->auxiliary)
                return ExportAuxiliaryArchiveCandidate(transactionId, expectedRevision, std::move(request));
            if (activeEdit_->placementIntent)
                return ExportPlacementArchiveCandidate(transactionId, expectedRevision, std::move(request));
            const auto fields = MapData::FreezeOriginalMapDataFields(*initialMapData_, *activeEdit_->mapData);
            if (!fields.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(fields);
            if (!fields.value->Operations().empty())
                return ExportMapDataArchiveCandidate(transactionId, expectedRevision, std::move(request));
        }
        const Trigger::TriggerTransactionLimits limits;
        const auto wtg = IO::ReadFileBytes(projectRoot_ / wtgRelativePath_, limits.maxMemberBytes);
        if (!wtg.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(wtg);
        const auto wct = IO::ReadFileBytes(projectRoot_ / wctRelativePath_, limits.maxMemberBytes);
        if (!wct.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(wct);
        const std::uint64_t generation = activeEdit_->generation;
        auto objectSnapshots = FreezeObjectTransactions(request.sourceMap, request.expectedSourceSha256);
        if (!objectSnapshots.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(objectSnapshots);
        request.objectTransactions = std::move(*objectSnapshots.value);
        const Trigger::TriggerTransactionBinding binding{
            request.expectedSourceSha256, transactionId, expectedRevision, generation};
        auto frozen = Trigger::TriggerTransactionSnapshot::Freeze(activeEdit_->document, catalog_,
            *wtg.value, *wct.value, binding, limits);
        if (!frozen.Succeeded()) {
            // 只有已冻结的物编改动和唯一 NO_CHANGE 错误允许补原样配对。
            // 来源、编码、GUI、自定义代码等其它失败不能降级为 companion。
            const bool unchangedPair = frozen.diagnostics.size() == 1
                && frozen.diagnostics.front().severity == DiagnosticSeverity::Error
                && frozen.diagnostics.front().code == "TRIGGER_TRANSACTION_NO_CHANGE";
            if (request.objectTransactions.empty() || !unchangedPair)
                return FailureFrom<Services::MapEditSaveReceipt>(frozen);
            frozen = Trigger::TriggerTransactionSnapshot::FreezeSourceCompanion(activeEdit_->document, catalog_,
                *wtg.value, *wct.value, binding, limits);
            if (!frozen.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(frozen);
        }
        request.triggerTransaction = std::move(*frozen.value);
        request.mapObjectReferences = activeEdit_->usedMapObjectReferences;
        const auto bound = CheckArchivePublishingIdentity(transactionId, expectedRevision, generation, *request.triggerTransaction, request.objectTransactions);
        if (!bound.Succeeded()) return FailureFrom<Services::MapEditSaveReceipt>(bound);
        request.beforePublish = [this, transactionId, expectedRevision, generation, snapshot = request.triggerTransaction,
            objects = request.objectTransactions]() {
            return CheckArchivePublishingIdentity(transactionId, expectedRevision, generation, *snapshot, objects);
        };
        // 这里只发布独立归档。无论成功或失败，activeEdit_/session_ 都仍以原目录为基准。
        return Services::SaveMapEditCandidate(request);
    } catch (const std::exception&) {
        return Result<Services::MapEditSaveReceipt>::Failure("MCP", "MCP_ARCHIVE_EXPORT_FAILED",
            "归档候选导出失败；原目录与活动事务保留，可显式回滚或重试。");
    }
}

Status McpWorkspace::Rollback(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision) {
    const Status identity = CheckIdentity(transactionId, expectedRevision);
    if (!identity.Succeeded()) {
        return identity;
    }
    Status status = session_.Rollback(transactionId);
    if (status.Succeeded()) {
        activeEdit_.reset();
        ++toolEpoch_;
        projectSearchMapCache_.reset();
    }
    return status;
}

} // namespace War3Map::Mcp
