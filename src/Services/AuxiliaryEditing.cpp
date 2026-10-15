#include "War3MapCore/Services/AuxiliaryEditing.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <type_traits>

namespace War3Map::Services {
namespace {
using namespace MapData;

Status Bad(const char* code, std::string message) {
    return Status::Failure("MapData.Auxiliary", code, std::move(message));
}
std::string CanonicalMember(std::string value) {
    value = Text::LowerAscii(value);
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.starts_with("map/")) value.erase(0, 4);
    return value;
}
bool SameMember(const MapMemberState& a, const MapMemberState& b) {
    if (a.state != b.state || CanonicalMember(a.memberName) != CanonicalMember(b.memberName)
        || a.sources.size() != b.sources.size()) return false;
    for (std::size_t i = 0; i < a.sources.size(); ++i)
        if (CanonicalMember(a.sources[i].name) != CanonicalMember(b.sources[i].name)
            || a.sources[i].data != b.sources[i].data) return false;
    return true;
}
bool TextValue(std::string_view value, bool required) {
    if ((required && Text::TrimLineWhitespace(value).empty()) || value.size() > 4096 || value.find('\0') != value.npos) return false;
    // 名称沿用原生字节规则，不把原有非 UTF-8 名称改成另一编码；新 UI/MCP 输入负责无损 UTF-8。
    return value.find('\r') == value.npos && value.find('\n') == value.npos;
}
bool Coordinate(float value) { return std::isfinite(value) && std::abs(value) <= 1e6f; }
std::string Symbol(MapDataKind kind, std::string_view name) {
    return std::string(kind == MapDataKind::Regions ? "gg_rct_" : "gg_cam_") + Text::ObjectScriptName(name);
}
Status ClassicCreationSource(const MapDocument& document) {
    const auto& state = document.Member(MapDataKind::Settings);
    if (state.state != MapDataState::Ready || state.sources.size() != 1 || !document.Settings()
        || !document.Settings()->HasClassicLayout())
        return Bad("AUX_CREATE_LAYOUT_UNKNOWN", "缺少唯一可解析的经典地图设置，不能为缺席成员推断区域/镜头布局。");
    return Status::Ok();
}
Status CheckRecord(const RegionRecord& record, const MapDocument& current, const RegionRecord* previous) {
    const bool changedSound = !previous || record.ambientSound != previous->ambientSound;
    if (!TextValue(record.name, true) || (changedSound && !TextValue(record.ambientSound, false)) || record.creationNumber < 0)
        return Bad("REGION_RECORD_INVALID", "区域名称、环境音或创建编号无效。");
    if (!Coordinate(record.left) || !Coordinate(record.bottom) || !Coordinate(record.right) || !Coordinate(record.top)
        || record.left >= record.right || record.bottom >= record.top)
        return Bad("REGION_BOUNDS_INVALID", "区域边界须为 ±1000000 内有限值，且左小于右、下小于上。");
    if (!previous || record.weatherId != previous->weatherId) {
        const auto weather = std::all_of(record.weatherId.begin(), record.weatherId.end(), [](auto b) { return b == std::byte{}; })
            ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(record.weatherId.data()), record.weatherId.size());
        if (!IsEditableRegionWeatherId(weather))
            return Bad("REGION_WEATHER_INVALID", "天气需为空或四字节可打印 rawcode，不能含引号或反斜杠。");
    }
    // 未能解释的原环境音只是保留，不能妨碍无关字段编辑，更不能静默改成空值。
    if (changedSound && !record.ambientSound.empty()) {
        const auto& state = current.Member(MapDataKind::Sounds);
        if (state.state != MapDataState::Ready || !current.Sounds())
            return Bad("REGION_SOUND_UNAVAILABLE", "区域环境音需要唯一且完整的声音定义表。");
        return ValidateRegionAmbientSound(record, &*current.Sounds());
    }
    return Status::Ok();
}
Status CheckRecord(const CameraRecord& record, const MapDocument&, const CameraRecord* previous) {
    const std::array values{record.targetX, record.targetY, record.zOffset, record.rotation, record.angleOfAttack,
        record.targetDistance, record.roll, record.fieldOfView, record.farClippingPlane};
    if (!TextValue(record.name, true) || !std::all_of(values.begin(), values.end(), Coordinate)
        || !std::isfinite(record.nearClippingPlane) || record.localRotation)
        return Bad("CAMERA_RECORD_INVALID", "经典镜头需要有效名称和有限参数，不能包含扩展局部旋转。");
    if (((!previous || previous->targetDistance != record.targetDistance) && record.targetDistance < 0)
        || ((!previous || previous->fieldOfView != record.fieldOfView) && (record.fieldOfView <= 0 || record.fieldOfView >= 180))
        || ((!previous || previous->farClippingPlane != record.farClippingPlane) && record.farClippingPlane <= 0))
        return Bad("CAMERA_RANGE_INVALID", "镜头距离不得为负，视野须在 0 与 180 度之间，远裁剪须为正数。");
    return Status::Ok();
}
Status CheckReferences(const MapDocument& current, const AuxiliaryEditRequest& request, std::string_view oldName,
    const AuxiliaryReferenceContext& context) {
    const std::string symbol = Symbol(request.kind, oldName);
    const std::string prefix = request.kind == MapDataKind::Regions ? "gg_rct_" : "gg_cam_";
    const std::vector<std::string> tokens{symbol, prefix + std::string(oldName)};
    if (context.triggers) {
        // 同时检查原参数池，不能把索引的深度/损坏边覆盖范围当作无引用证明。
        for (const auto& argument : context.triggers->argumentNodes) {
            const auto value = Text::TrimHorizontal(argument.value.text);
            if (std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) { return value == token; }))
                return Bad("AUX_REFERENCED", "触发器参数仍引用 " + symbol + "，请先修改引用后再删除或更名。");
        }
        ProjectReferenceInputs inputs; inputs.triggers = context.triggers; inputs.map = &current;
        const auto index = ProjectReferenceIndex::Build(inputs);
        const auto usages = index.UsagesOf(tokens);
        if (!usages.empty()) return Bad("AUX_REFERENCED", "仍有触发器或变量引用 " + symbol + "："
            + usages.front()->owner + "。请先修改这些引用，再删除或更名。");
    }
    if (request.kind == MapDataKind::Regions && request.action == AuxiliaryEditAction::Delete) {
        const auto creation = current.Regions()->entries[static_cast<std::size_t>(request.sourceIndex)].creationNumber;
        const auto state = current.Member(MapDataKind::Units).state;
        if (state != MapDataState::Missing && (state != MapDataState::Ready || !current.Units()))
            return Bad("AUX_REFERENCE_COVERAGE", "单位表未完整解析，无法核对传送门对区域的引用。");
        if (current.Units()) for (const auto& unit : current.Units()->units)
            if (unit.waygateRegionId == creation)
                return Bad("REGION_WAYGATE_REFERENCE", "区域仍被传送门目标引用（单位创建编号 "
                    + std::to_string(unit.creationId) + "），请先修改该目标。");
    }
    if (!context.completeTriggerSource || !context.triggers)
        return Bad("AUX_REFERENCE_COVERAGE", "触发器来源尚未完整读取，无法确认删除或更名的影响。");
    const auto& triggers = *context.triggers;
    const bool opaque = !Text::TrimLineWhitespace(triggers.wct.customCode.text).empty()
        || std::any_of(triggers.triggers.begin(), triggers.triggers.end(), [](const auto& trigger) {
            return !Text::TrimLineWhitespace(trigger.customText.text).empty();
        }) || std::any_of(triggers.ecaNodes.begin(), triggers.ecaNodes.end(), [](const auto& eca) {
            return eca.name.text == "CustomScriptCode";
        }) || !context.opaqueScriptSources.empty();
    if (opaque) return Bad("AUX_SCRIPT_REFERENCES_UNKNOWN", "存在自定义或外部脚本，静态 T 索引不能证明没有引用 "
        + symbol + "；请先消除或明确迁移脚本中的引用。范围和其它属性仍可编辑。");
    return Status::Ok();
}
Status CheckCreatedSymbol(const AuxiliaryReferenceContext& context, const std::string& symbol) {
    if (!context.completeTriggerSource || !context.triggers)
        return Bad("AUX_REFERENCE_COVERAGE", "触发器来源尚未完整读取，无法核对新增脚本符号 " + symbol + "。");
    if (!context.opaqueScriptSources.empty())
        return Bad("AUX_SCRIPT_REFERENCES_UNKNOWN", "外部或未命名脚本尚未建立完整符号索引，不能证明新符号 "
            + symbol + " 不冲突；范围和其它属性仍可编辑。");
    const auto mentions = [&](std::string_view text) { return text.find(symbol) != text.npos; };
    if (mentions(context.triggers->wct.customCode.text)
        || std::any_of(context.triggers->triggers.begin(), context.triggers->triggers.end(),
            [&](const auto& trigger) { return mentions(trigger.customText.text); }))
        return Bad("AUX_SCRIPT_SYMBOL_CONFLICT", "自定义代码已使用新增脚本符号 " + symbol + "，请先核对名称。");
    for (const auto& eca : context.triggers->ecaNodes) if (eca.name.text == "CustomScriptCode") {
        for (const auto argument : eca.arguments) {
            if (argument >= context.triggers->argumentNodes.size())
                return Bad("AUX_REFERENCE_COVERAGE", "自定义脚本参数来源损坏，无法核对新增符号。");
            if (mentions(context.triggers->argumentNodes[argument].value.text))
                return Bad("AUX_SCRIPT_SYMBOL_CONFLICT", "自定义脚本动作已使用新增符号 " + symbol + "，请先核对名称。");
        }
    }
    return Status::Ok();
}
Status CheckReferenceTransition(const MapDocument& current, const AuxiliaryEditRequest& request,
    const AuxiliaryReferenceContext& references) {
    const bool create = request.action == AuxiliaryEditAction::Create;
    const bool remove = request.action == AuxiliaryEditAction::Delete;
    const auto oldName = create ? std::string{} : request.kind == MapDataKind::Regions
        ? current.Regions()->entries[static_cast<std::size_t>(request.sourceIndex)].name
        : current.Cameras()->entries[static_cast<std::size_t>(request.sourceIndex)].name;
    const auto newName = remove ? std::string{} : request.kind == MapDataKind::Regions
        ? std::get<RegionRecord>(request.record).name : std::get<CameraRecord>(request.record).name;
    if (remove || (!create && oldName != newName)) {
        const auto checked = CheckReferences(current, request, oldName, references);
        if (!checked.Succeeded()) return checked;
    }
    if (create || (!remove && oldName != newName)) return CheckCreatedSymbol(references, Symbol(request.kind, newName));
    return Status::Ok();
}
} // namespace

bool AuxiliaryEditing::AreEquivalentMapSources(const MapDocument& left, const MapDocument& right) {
    for (std::size_t i = 0; i < left.Members().size(); ++i)
        if (!SameMember(left.Member(static_cast<MapDataKind>(i)), right.Member(static_cast<MapDataKind>(i)))) return false;
    return true;
}

Result<std::string> AuxiliaryEditing::MemberIdentity(const MapDocument& document, MapDataKind kind) {
    if (kind != MapDataKind::Regions && kind != MapDataKind::Cameras)
        return {{}, Bad("AUX_KIND_INVALID", "这里只支持区域与镜头。").diagnostics};
    const auto& member = document.Member(kind);
    if (member.state == MapDataState::Missing) return Result<std::string>::Success("missing");
    if (member.state != MapDataState::Ready || member.sources.size() != 1)
        return {{}, Bad("AUX_SOURCE_UNSAFE", "区域或镜头来源损坏、歧义、未知或未完整读取。").diagnostics};
    const auto bytes = SerializeMapFieldTable(document, kind);
    if (!bytes.Succeeded() || !bytes.value || *bytes.value != member.sources.front().data)
        return {{}, Bad("AUX_ROUNDTRIP_UNSAFE", "区域或镜头来源不能完整往返，不能重建成员。").diagnostics};
    return Sha256(member.sources.front().data);
}

Result<PreparedAuxiliaryEdit> AuxiliaryEditing::Prepare(std::shared_ptr<const MapDocument> current,
    AuxiliaryEditRequest request, const AuxiliaryReferenceContext& references) {
    const auto fail = [](const Status& status) { return Result<PreparedAuxiliaryEdit>{std::nullopt, status.diagnostics}; };
    if (!current) return fail(Bad("AUX_SOURCE_UNAVAILABLE", "当前地图数据不可用。"));
    const auto identity = MemberIdentity(*current, request.kind);
    if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
    if (*identity.value != request.expectedMemberSha256)
        return fail(Bad("AUX_MEMBER_STALE", "区域或镜头成员已变化，请重新查询。"));
    const bool create = request.action == AuxiliaryEditAction::Create;
    const bool remove = request.action == AuxiliaryEditAction::Delete;
    if (!create && !remove && request.action != AuxiliaryEditAction::Update)
        return fail(Bad("AUX_ACTION_INVALID", "未知区域或镜头编辑操作。"));
    if (remove ? !std::holds_alternative<std::monostate>(request.record)
        : request.kind == MapDataKind::Regions ? !std::holds_alternative<RegionRecord>(request.record)
        : !std::holds_alternative<CameraRecord>(request.record))
        return fail(Bad("AUX_RECORD_KIND", "完整记录类型与操作不匹配。"));
    if (*identity.value == "missing") {
        if (!create) return fail(Bad("AUX_ROW_MISSING", "成员缺席，不能修改或删除记录。"));
        const auto classic = ClassicCreationSource(*current);
        if (!classic.Succeeded()) return fail(classic);
    }
    if (!create) {
        const auto row = MapFieldRowKey(*current, request.kind, request.sourceIndex);
        if (!row.Succeeded() || !row.value || *row.value != request.expectedRowKey)
            return fail(Bad("AUX_ROW_STALE", "区域或镜头行身份已变化，请重新选择。"));
    } else if (!request.expectedRowKey.empty() || request.sourceIndex != 0)
        return fail(Bad("AUX_CREATE_IDENTITY", "新增记录不能借用已有行身份。"));

    std::vector<std::byte> bytes;
    const auto editTable = [&](auto table) -> Status {
        using Record = typename decltype(table.entries)::value_type;
        if constexpr (std::is_same_v<Record, RegionRecord>) {
            if (table.version != 5) return Bad("AUX_LAYOUT_UNSUPPORTED", "只支持经典 v5 区域表。");
        } else if (table.version != 0 || table.layout != CameraLayout::Classic)
            return Bad("AUX_LAYOUT_UNSUPPORTED", "只支持来源已证明为经典布局的镜头表。");
        if (!remove) {
            const auto& record = std::get<Record>(request.record);
            const auto* previous = create ? nullptr : &table.entries[static_cast<std::size_t>(request.sourceIndex)];
            const auto checked = CheckRecord(record, *current, previous);
            if (!checked.Succeeded()) return checked;
            if constexpr (std::is_same_v<Record, RegionRecord>) {
                if (!create && record.creationNumber != table.entries[static_cast<std::size_t>(request.sourceIndex)].creationNumber)
                    return Bad("REGION_CREATION_IMMUTABLE", "修改区域时必须保留创建编号，不能断开传送门引用。");
                if (!create && record.colorBgra[3] != table.entries[static_cast<std::size_t>(request.sourceIndex)].colorBgra[3])
                    return Bad("REGION_COLOR_RESERVED", "区域颜色的第四字节用途未证实，编辑颜色须保留该字节。");
            } else if (!create && std::bit_cast<std::uint32_t>(record.nearClippingPlane)
                != std::bit_cast<std::uint32_t>(table.entries[static_cast<std::size_t>(request.sourceIndex)].nearClippingPlane)) {
                return Bad("CAMERA_CLASSIC_SLOT_READ_ONLY", "经典镜头第十个数值槽用途尚未证实，编辑必须逐位保留来源。");
            }
            for (std::size_t i = 0; i < table.entries.size(); ++i) {
                if (!create && i == request.sourceIndex) continue;
                if (Symbol(request.kind, table.entries[i].name) == Symbol(request.kind, record.name))
                    return Bad("AUX_SYMBOL_DUPLICATE", "名称经原版规则转换后会生成重复脚本变量。");
                if constexpr (std::is_same_v<Record, RegionRecord>)
                    if (record.creationNumber == table.entries[i].creationNumber)
                        return Bad("REGION_ID_DUPLICATE", "区域创建编号已存在。");
            }
            if (create) {
                if (table.entries.size() >= 100000) return Bad("AUX_COUNT_LIMIT", "区域或镜头记录数量已达到上限。");
                table.entries.push_back(record);
            } else table.entries[static_cast<std::size_t>(request.sourceIndex)] = record;
        } else table.entries.erase(table.entries.begin() + static_cast<std::ptrdiff_t>(request.sourceIndex));
        const auto serialized = table.Serialize();
        if (!serialized.Succeeded()) return {serialized.diagnostics};
        bytes = *serialized.value;
        return Status::Ok();
    };
    const auto edited = request.kind == MapDataKind::Regions
        ? editTable(current->Regions().value_or(RegionsDocument{}))
        : editTable(current->Cameras().value_or(CamerasDocument{}));
    if (!edited.Succeeded()) return fail(edited);
    const auto referenced = CheckReferenceTransition(*current, request, references);
    if (!referenced.Succeeded()) return fail(referenced);
    const std::vector<MapArchiveMember> replacements{{request.kind == MapDataKind::Regions ? "war3map.w3r" : "war3map.w3c", bytes}};
    auto after = ProjectMapData(*current, replacements);
    if (!after.Succeeded()) return {std::nullopt, after.diagnostics};
    if (after.value->Member(request.kind).state != MapDataState::Ready)
        return fail(Bad("AUX_OUTPUT_UNSAFE", "修改后的区域或镜头无法完整读取。"));
    const bool changed = !SameMember(current->Member(request.kind), after.value->Member(request.kind));
    return Result<PreparedAuxiliaryEdit>::Success(PreparedAuxiliaryEdit(std::move(request), std::move(current),
        std::make_shared<const MapDocument>(std::move(*after.value)), changed));
}

Status AuxiliaryEditing::Apply(MapDataEditSession& session, const PreparedAuxiliaryEdit& plan,
    const AuxiliaryReferenceContext& references, const MapDataPublishValidator& validator) {
    const auto current = session.Snapshot();
    if (!AreEquivalentMapSources(current, plan.Before()))
        return Bad("AUX_PLAN_STALE", "区域/镜头命令的来源已变化，不能覆盖新编辑。");
    if (!plan.HasChanges()) return Status::Ok();
    const auto referenced = CheckReferenceTransition(current, plan.Request(), references);
    if (!referenced.Succeeded()) return referenced;
    MapDataEditSession::Command command;
    const auto prepared = session.Prepare(plan.Kind(), Result<std::vector<std::byte>>::Success(
        plan.After().Member(plan.Kind()).sources.front().data), command.changes);
    if (!prepared.Succeeded()) return prepared;
    return session.CommitCommand(std::move(command), [&](const auto& receipt) {
        return validator ? validator(plan.Before(), plan.After(), receipt) : Status::Ok();
    });
}

} // namespace War3Map::Services
