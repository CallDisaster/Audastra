#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include "MapCustomSettingsReferences.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/Utf8Scalar.h"

#include <algorithm>
#include <limits>
#include <set>
#include <map>

namespace War3Map::Services {
namespace {
using namespace MapData;
using Section = MapCustomSettingsSection;
using Action = MapCustomSettingsAction;
Status Bad(const char* code, std::string message) {
    return Status::Failure("Map.CustomSettings", code, std::move(message));
}
std::string Id(const SettingsRawcode& id) { return {reinterpret_cast<const char*>(id.data()), id.size()}; }
bool Random(Section s) { return s == Section::RandomUnits || s == Section::RandomItems; }
bool Pseudo(const SettingsRawcode& id) {
    const auto value = Id(id);
    return value[0] == 'Y' && (value[2] == 'I' || value[2] == 'U' || value[2] == 'B');
}
bool None(const SettingsRawcode& id) { return id == SettingsRawcode{}; }
Result<std::vector<ObjectFieldRow>> Fields(const MapCustomSettingsValidation& sources,
    ObjectData::ObjectType type, const SettingsRawcode& id) {
    using R = Result<std::vector<ObjectFieldRow>>;
    if (!sources.objectContext) return R::Failure("Map.CustomSettings", "SETTINGS_OBJECT_SOURCE_REQUIRED", "修改需要真实物编目录及字段元数据。");
    auto context = sources.objectContext(type);
    if (!context.Succeeded()) return {{}, context.diagnostics};
    return ReadObjectFields(*context.value, Id(id));
}
std::optional<std::int32_t> Integer(const std::vector<ObjectFieldRow>& fields, std::string_view id) {
    for (const auto& field : fields) if (field.key.fieldId == id) {
        const auto& value = field.value ? field.value : field.defaultValue;
        if (value) {
            if (const auto* number = std::get_if<std::int32_t>(&*value)) return *number;
            if (const auto* boolean = std::get_if<bool>(&*value)) return *boolean ? 1 : 0;
        }
    }
    return {};
}
Status Object(const MapCustomSettingsValidation& sources, const SettingsRawcode& id, std::int32_t kind) {
    if (None(id)) return Status::Ok();
    if (Pseudo(id)) return Bad("SETTINGS_PSEUDO_ID_READ_ONLY", "类别/等级伪 ID 目前仅保留来源，不开放新写。");
    const auto type = kind == 2 ? ObjectData::ObjectType::Item : ObjectData::ObjectType::Unit;
    auto fields = Fields(sources, type, id);
    if (!fields.Succeeded()) return {fields.diagnostics};
    if (kind != 2) {
        const auto building = Integer(*fields.value, "ubdg");
        if (!building || (*building != 0 && *building != 1)) return Bad("SETTINGS_OBJECT_CLASS_UNKNOWN", "无法从实际物编证明该对象的建筑类别。");
        if ((kind == 1) != (*building == 1)) return Bad("SETTINGS_OBJECT_CLASS_MISMATCH", "随机表列的单位/建筑类型与实际对象不符。");
    }
    return Status::Ok();
}
Status Mask(std::uint32_t mask, const MapSettings& settings, std::optional<std::uint32_t> previous = {}) {
    std::uint32_t allowed = 0;
    for (const auto& player : settings.players) {
        if (player.number < 0 || player.number >= 12 || (allowed & (1u << player.number)))
            return Bad("SETTINGS_PLAYER_SOURCE_UNSUPPORTED", "玩家表含重复或经典目标不支持的槽位。");
        allowed |= 1u << player.number;
    }
    if ((!mask && (!previous || *previous != 0)) || (mask & ~(allowed | previous.value_or(0))))
        return Bad("SETTINGS_PLAYER_MASK_INVALID", "新写玩家掩码必须非空且仅新增当前经典地图玩家槽；旧掩码位可原样保留。");
    return Status::Ok();
}
Status Validate(const MapCustomSettingsValue& value, const MapSettings& settings,
    const MapCustomSettingsValidation& sources, const MapCustomSettingsValue* previous,
    std::vector<Diagnostic>& diagnostics) {
    if (const auto* row = std::get_if<MapUpgradeAvailability>(&value)) {
        const auto* old = previous ? std::get_if<MapUpgradeAvailability>(previous) : nullptr;
        auto mask = Mask(row->playerMask, settings, old ? std::optional(old->playerMask) : std::nullopt); if (!mask.Succeeded()) return mask;
        if (row->level < 0 || row->level == std::numeric_limits<std::int32_t>::max()
            || row->availability < 0 || row->availability > 2)
            return Bad("SETTINGS_UPGRADE_VALUE_INVALID", "升级等级须为非负零基整数，可用性须为 0/1/2。");
        auto fields = Fields(sources, ObjectData::ObjectType::Upgrade, row->id);
        if (!fields.Succeeded()) return {fields.diagnostics};
        auto levels = Integer(*fields.value, "glvl");
        if (!levels || *levels <= row->level) return Bad("SETTINGS_UPGRADE_LEVEL_INVALID", "升级等级超出真实物编定义或等级上限无法确定。");
    } else if (const auto* row = std::get_if<MapTechRestriction>(&value)) {
        const auto* old = previous ? std::get_if<MapTechRestriction>(previous) : nullptr;
        auto mask = Mask(row->playerMask, settings, old ? std::optional(old->playerMask) : std::nullopt); if (!mask.Succeeded()) return mask;
        unsigned matches = 0;
        for (auto type : {ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item, ObjectData::ObjectType::Ability}) {
            if (!sources.objectContext) return Bad("SETTINGS_OBJECT_SOURCE_REQUIRED", "科技限制需要真实物编来源。");
            auto context = sources.objectContext(type); if (!context.Succeeded()) return {context.diagnostics};
            if (!context.value->standard || !context.value->schema || !context.value->document)
                return Bad("SETTINGS_OBJECT_SOURCE_REQUIRED", "科技限制物编来源不完整。");
            if (!context.value->standard->Find(type, Id(row->id)) && !context.value->document->FindObject({type, Id(row->id)})) continue;
            const auto fields = ReadObjectFields(*context.value, Id(row->id));
            if (!fields.Succeeded()) return {fields.diagnostics};
            ++matches;
        }
        if (matches != 1) return Bad("SETTINGS_TECH_ID_UNRESOLVED", "限制对象须唯一解析到真实单位、物品或技能定义。");
    } else if (const auto* table = std::get_if<MapRandomUnitTable>(&value)) {
        if (table->number < 0 || table->columnTypes.size() > 64
            || table->rows.size() > 4096 || (!table->columnTypes.empty() && table->rows.size() > 16384 / table->columnTypes.size()))
            return Bad("SETTINGS_RANDOM_TABLE_BUDGET", "随机单位表编号、行列数量不合法或超出编辑预算。");
        for (const auto type : table->columnTypes) if (type < 0 || type > 2)
            return Bad("SETTINGS_RANDOM_COLUMN_INVALID", "随机列类型只能为 0 单位、1 建筑、2 物品。");
        const auto* old = previous ? std::get_if<MapRandomUnitTable>(previous) : nullptr;
        std::uint64_t total = 0;
        for (std::size_t r = 0; r < table->rows.size(); ++r) {
            const auto& row = table->rows[r];
            if (row.chance < 0 || row.chance > 100 || row.ids.size() != table->columnTypes.size())
                return Bad("SETTINGS_RANDOM_ROW_INVALID", "随机单位行概率须为 0..100，ID 数须等于列数。");
            total += row.chance;
            for (std::size_t c = 0; c < row.ids.size(); ++c) {
                if (old && r < old->rows.size() && c < old->rows[r].ids.size()
                    && c < old->columnTypes.size() && old->columnTypes[c] == table->columnTypes[c]
                    && old->rows[r].ids[c] == row.ids[c]) continue;
                auto checked = Object(sources, row.ids[c], table->columnTypes[c]); if (!checked.Succeeded()) return checked;
            }
        }
        if (!total) return Bad("SETTINGS_RANDOM_WEIGHT_EMPTY", "随机单位表概率总和必须至少为 1。");
        if (total != 100) diagnostics.push_back({DiagnosticSeverity::Info, "Map.CustomSettings", "SETTINGS_RANDOM_PROPORTIONAL", "随机单位表按概率总和成比例抽取。"});
    } else if (const auto* table = std::get_if<MapRandomItemTable>(&value)) {
        if (table->number < 0 || table->sets.size() > 4096)
            return Bad("SETTINGS_RANDOM_TABLE_BUDGET", "随机物品表编号或集合数量不合法。");
        const auto* old = previous ? std::get_if<MapRandomItemTable>(previous) : nullptr;
        std::size_t count = 0;
        for (std::size_t s = 0; s < table->sets.size(); ++s) {
            const auto& set = table->sets[s];
            if (set.size() > 16384 - count) return Bad("SETTINGS_RANDOM_TABLE_BUDGET", "随机物品总项数超过 16384。");
            count += set.size();
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < set.size(); ++i) {
                if (set[i].chance < 0 || set[i].chance > 100) return Bad("SETTINGS_RANDOM_CHANCE_INVALID", "随机物品概率须为 0..100。");
                total += set[i].chance;
                if (old && s < old->sets.size() && i < old->sets[s].size() && old->sets[s][i].id == set[i].id) continue;
                auto checked = Object(sources, set[i].id, 2); if (!checked.Succeeded()) return checked;
            }
            if (total > 100) return Bad("SETTINGS_RANDOM_CHANCE_SUM", "随机物品组概率总和不能大于 100；余量为不掉落。");
        }
    } else return Bad("SETTINGS_COMMAND_VALUE_REQUIRED", "非删除命令必须提供对应类型的值。");
    return Status::Ok();
}
Status Conflicts(const MapSettings& settings) {
    std::map<SettingsRawcode, std::pair<std::uint32_t, std::uint32_t>> masks;
    for (const auto& row : settings.custom->upgrades) {
        auto& [disabled, researched] = masks[row.id];
        if (row.availability == 0) disabled |= row.playerMask;
        if (row.availability == 2) researched |= row.playerMask;
        if (disabled & researched)
            return Bad("SETTINGS_UPGRADE_CONFLICT", "同一玩家的同一升级同时禁用和已研究，不能由行顺序决定初始化。");
    }
    return Status::Ok();
}
} // namespace

Status MapCustomSettingsEditing::WriteQualification(const MapDocument& document) {
    const auto& member = document.Member(MapDataKind::Settings);
    if (member.state != MapDataState::Ready || member.sources.size() != 1 || document.LniSettings()
        || !document.Settings() || !document.Settings()->FullyParsed())
        return Bad("SETTINGS_CUSTOM_SOURCE_READ_ONLY", "自定义表修改需要唯一、完整且无未知尾的原生 W3I；缺席或 FF 省略不能当成空表。");
    if (document.Settings()->SourceFormatVersion() != 25 || document.Settings()->formatVersion != 25)
        return Bad("SETTINGS_CUSTOM_VERSION_READ_ONLY", "本轮只开放来源为原生 W3I v25 的修改，其它版本仅查询。");
    return Status::Ok();
}
Status MapCustomSettingsEditing::RandomWriteQualification(const MapDocument& document,
    const LuaMapTargetRequest* selectedLuaTarget) {
    if (selectedLuaTarget) return Bad("SETTINGS_RANDOM_LUA_UNSUPPORTED", "随机单位/物品表当前只支持经典v25的JASS初始化；所选Lua目标尚不支持，请先切换保存目标。");
    auto writable = WriteQualification(document); if (!writable.Succeeded()) return writable;
    if (document.Settings()->scriptLanguage != 0)
        return Bad("SETTINGS_RANDOM_LANGUAGE_UNSUPPORTED", "随机单位/物品表当前只支持JASS初始化，不能按未知脚本语言保存。");
    if (!Script::RandomMapInitializationAvailable())
        return Bad("SETTINGS_RANDOM_COMMIT_UNSUPPORTED", "当前构建尚未提供已验收的随机表初始化。");
    return Status::Ok();
}
Result<std::string> MapCustomSettingsEditing::MemberIdentity(const MapDocument& document) {
    const auto& member = document.Member(MapDataKind::Settings);
    if (member.sources.size() != 1) return Result<std::string>::Failure("Map.CustomSettings", "SETTINGS_MEMBER_AMBIGUOUS", "W3I 没有唯一来源。");
    return Sha256(member.sources.front().data);
}
Result<std::string> MapCustomSettingsEditing::RecordKey(Section section, const MapCustomSettingsValue& record, std::int32_t version) {
    const auto size = std::visit([](const auto& row) -> std::uint64_t {
        using R = std::decay_t<decltype(row)>;
        if constexpr (std::is_same_v<R, MapRandomUnitTable>) { std::uint64_t n = 13ull + row.name.size() + row.columnTypes.size() * 4ull; for (const auto& entry : row.rows) n += 4ull + entry.ids.size() * 4ull; return n; }
        else if constexpr (std::is_same_v<R, MapRandomItemTable>) { std::uint64_t n = 9ull + row.name.size(); for (const auto& set : row.sets) n += 4ull + set.size() * 8ull; return n; }
        else return 16;
    }, record);
    if (size > 1024 * 1024 - 1024) return Result<std::string>::Failure("Map.CustomSettings", "SETTINGS_RECORD_KEY_BUDGET", "记录超过编辑令牌预算；仍可按子表分页查询。");
    MapSettings envelope; envelope.formatVersion = 25; envelope.custom.emplace();
    const auto add = [&](auto& rows, auto* row) { if (row) { rows.push_back(*row); return true; } return false; };
    const bool valid = section == Section::Upgrades ? add(envelope.custom->upgrades, std::get_if<MapUpgradeAvailability>(&record))
        : section == Section::TechRestrictions ? add(envelope.custom->techRestrictions, std::get_if<MapTechRestriction>(&record))
        : section == Section::RandomUnits ? add(envelope.custom->randomUnits, std::get_if<MapRandomUnitTable>(&record))
        : add(envelope.custom->randomItems, std::get_if<MapRandomItemTable>(&record));
    if (!valid) return Result<std::string>::Failure("Map.CustomSettings", "SETTINGS_COMMAND_TYPE_INVALID", "section 与记录类型不匹配。");
    auto bytes = envelope.Serialize(); if (!bytes.Succeeded()) return {{}, bytes.diagnostics};
    if (bytes.value->size() > 1024 * 1024) return Result<std::string>::Failure("Map.CustomSettings", "SETTINGS_RECORD_KEY_BUDGET", "记录超过 1 MiB，不能生成编辑令牌。");
    const auto domain = "MapCustomSettings/v1/" + std::to_string(version) + "/" + std::to_string(static_cast<int>(section)) + "/";
    bytes.value->insert(bytes.value->begin(), reinterpret_cast<const std::byte*>(domain.data()), reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
    return Sha256(*bytes.value);
}
Result<PreparedMapCustomSettingsEdit> MapCustomSettingsEditing::Prepare(std::shared_ptr<const MapDocument> current,
    const MapCustomSettingsEditRequest& request, const MapCustomSettingsValidation& sources) {
    using R = Result<PreparedMapCustomSettingsEdit>;
    const auto fail = [](Status s) -> R { return {{}, std::move(s.diagnostics)}; };
    if (!current) return fail(Bad("SETTINGS_DOCUMENT_REQUIRED", "需要当前地图文档。"));
    auto allowed = WriteQualification(*current); if (!allowed.Succeeded()) return fail(allowed);
    auto identity = MemberIdentity(*current);
    if (!identity.Succeeded()) return {{}, identity.diagnostics};
    if (*identity.value != request.expectedMemberSha256) return fail(Bad("SETTINGS_MEMBER_STALE", "W3I SHA 已变化，请重新查询。"));
    if (request.operations.empty() || request.operations.size() > 128) return fail(Bad("SETTINGS_COMMAND_BUDGET", "批次须含 1 到 128 条操作。"));
    if (sources.verify) { auto verified = sources.verify(); if (!verified.Succeeded()) return fail(verified); }
    PreparedMapCustomSettingsEdit result;
    result.before_ = current; result.request_ = request;
    result.randomAvailable_ = Script::RandomMapInitializationAvailable();
    auto settings = *current->Settings();
    if (!settings.custom->randomUnits.empty() || !settings.custom->randomItems.empty()
        || std::any_of(request.operations.begin(), request.operations.end(), [](const auto& operation) {
            return operation.section == Section::RandomUnits || operation.section == Section::RandomItems;
        })) {
        auto qualification = sources.randomWriteQualification ? sources.randomWriteQualification(*current) : RandomWriteQualification(*current);
        if (!qualification.Succeeded()) return fail(std::move(qualification));
    }
    if (settings.custom->upgrades.size() > 65536 || settings.custom->techRestrictions.size() > 65536
        || settings.custom->randomUnits.size() > 4096 || settings.custom->randomItems.size() > 4096)
        return fail(Bad("SETTINGS_SOURCE_EDIT_BUDGET", "源自定义表超过本轮编辑预算，保留查询能力。"));
    std::size_t commandBytes = 0;
    for (std::size_t operation = 0; operation < request.operations.size(); ++operation) {
        const auto& command = request.operations[operation];
        const auto edit = [&](auto& rows) -> Status {
            using Record = typename std::decay_t<decltype(rows)>::value_type;
            constexpr bool table = std::is_same_v<Record, MapRandomUnitTable> || std::is_same_v<Record, MapRandomItemTable>;
            const bool append = command.action == Action::Append, remove = command.action == Action::Delete;
            if (static_cast<unsigned>(command.action) > static_cast<unsigned>(Action::Delete)) return Bad("SETTINGS_COMMAND_ACTION_INVALID", "未知编辑动作。");
            std::size_t index = rows.size();
            if (append) {
                if (!command.expectedCount || *command.expectedCount != rows.size() || command.index || !command.expectedKey.empty())
                    return Bad("SETTINGS_COUNT_STALE", "append 必须匹配执行到本条时的实际表长，不能携带行定位。");
                if constexpr (!table) if (command.number) return Bad("SETTINGS_COMMAND_SHAPE_INVALID", "科技行不接受表编号。");
            } else {
                if (command.expectedCount || command.expectedKey.size() != 64) return Bad("SETTINGS_COMMAND_CAS_REQUIRED", "修改/删除必须携带完整记录令牌，不接受 expected_count。");
                if constexpr (table) {
                    if (!command.number || command.index) return Bad("SETTINGS_COMMAND_SHAPE_INVALID", "随机表按 number 定位，不能使用数组下标。");
                    for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].number == *command.number) {
                        if (index != rows.size()) return Bad("SETTINGS_TABLE_NUMBER_AMBIGUOUS", "源表编号重复，无法定位。");
                        index = i;
                    }
                } else {
                    if (!command.index || command.number) return Bad("SETTINGS_COMMAND_SHAPE_INVALID", "科技行须提供 index，不能携带表编号。");
                    index = *command.index;
                }
                if (index >= rows.size()) return Bad("SETTINGS_ROW_NOT_FOUND", "目标记录不存在。");
                auto key = RecordKey(command.section, rows[index], settings.formatVersion);
                if (!key.Succeeded()) return {key.diagnostics};
                if (*key.value != command.expectedKey) return Bad("SETTINGS_ROW_STALE", "目标记录已变化，请重新查询令牌。");
            }
            if (remove) {
                if (!std::holds_alternative<std::monostate>(command.value) || command.keepName) return Bad("SETTINGS_COMMAND_SHAPE_INVALID", "删除不接受值或名称。");
                if constexpr (table) {
                    const MapCustomSettingsValue old = rows[index];
                    auto references = CheckRandomSettingsReferences(*current, command.section, &old, nullptr, sources);
                    if (!references.Succeeded()) return references;
                    result.randomChanges_ = true;
                }
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index)); result.stepKeys_.emplace_back(); return Status::Ok();
            }
            const auto* supplied = std::get_if<Record>(&command.value);
            if (!supplied) return Bad("SETTINGS_COMMAND_TYPE_INVALID", "section 与值类型不匹配。");
            auto suppliedKey = RecordKey(command.section, command.value, settings.formatVersion); if (!suppliedKey.Succeeded()) return {suppliedKey.diagnostics};
            auto record = *supplied;
            if constexpr (table) {
                if (append && (!command.number || *command.number != record.number)) return Bad("SETTINGS_TABLE_NUMBER_REQUIRED", "新增表须明确提供相同的 number。");
                if (!append && record.number != rows[index].number) return Bad("SETTINGS_TABLE_NUMBER_IMMUTABLE", "replace 不能改变编号。");
                if (append && std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.number == record.number; })) return Bad("SETTINGS_TABLE_NUMBER_DUPLICATE", "表编号已存在，不自动重编号。");
                if (command.keepName) {
                    if (append) return Bad("SETTINGS_NAME_SOURCE_REQUIRED", "新增名称不能保留不存在的来源。");
                    record.name = rows[index].name;
                } else {
                    if (record.name.size() > 4096 || record.name.find('\0') != record.name.npos || record.name.starts_with("TRIGSTR_")) return Bad("SETTINGS_NAME_INVALID", "名称须为无 NUL 的 UTF-8 字面量，不能隐式创建 TRIGSTR 引用。");
                    for (std::size_t at = 0; at < record.name.size();) {
                        const auto scalar = Text::Detail::ReadUtf8Scalar(record.name, at);
                        if (!scalar.Valid()) return Bad("SETTINGS_NAME_INVALID", "名称字面量不是有效 UTF-8；保留原编码须用 keep。");
                        at += scalar.bytes;
                    }
                }
                const MapCustomSettingsValue replacement = record;
                const MapCustomSettingsValue old = append ? MapCustomSettingsValue{} : MapCustomSettingsValue{rows[index]};
                auto references = CheckRandomSettingsReferences(*current, command.section, append ? nullptr : &old, &replacement, sources);
                if (!references.Succeeded()) return references;
            } else if (command.keepName) return Bad("SETTINGS_COMMAND_SHAPE_INVALID", "科技行不接受名称。");
            auto key = RecordKey(command.section, record, settings.formatVersion); if (!key.Succeeded()) return {key.diagnostics};
            MapSettings budgetEnvelope; budgetEnvelope.formatVersion = 25; budgetEnvelope.custom.emplace();
            if constexpr (std::is_same_v<Record, MapUpgradeAvailability>) budgetEnvelope.custom->upgrades.push_back(record);
            else if constexpr (std::is_same_v<Record, MapTechRestriction>) budgetEnvelope.custom->techRestrictions.push_back(record);
            else if constexpr (std::is_same_v<Record, MapRandomUnitTable>) budgetEnvelope.custom->randomUnits.push_back(record);
            else budgetEnvelope.custom->randomItems.push_back(record);
            auto encodedBudget = budgetEnvelope.Serialize(); if (!encodedBudget.Succeeded()) return {encodedBudget.diagnostics};
            if (encodedBudget.value->size() > 1024 * 1024 - commandBytes) return Bad("SETTINGS_COMMAND_BYTE_BUDGET", "整个类型化批次超过 1 MiB。");
            commandBytes += encodedBudget.value->size();
            bool same = false;
            MapCustomSettingsValue previous;
            if (!append) { previous = rows[index]; auto old = RecordKey(command.section, previous, settings.formatVersion); if (!old.Succeeded()) return {old.diagnostics}; same = *old.value == *key.value; }
            if (!same) {
                auto checked = Validate(record, settings, sources, append ? nullptr : &previous, result.diagnostics_);
                if (!checked.Succeeded()) return checked;
                if constexpr (!table) for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (!append && i == index) continue;
                    bool duplicate = rows[i].id == record.id && rows[i].playerMask == record.playerMask;
                    if constexpr (std::is_same_v<Record, MapUpgradeAvailability>) duplicate = duplicate && rows[i].level == record.level && rows[i].availability == record.availability;
                    if (duplicate) return Bad("SETTINGS_ROW_DUPLICATE", "新写记录与另一源行重复，不自动合并。");
                }
                if constexpr (table) result.randomChanges_ = true;
                if (append) rows.push_back(std::move(record)); else rows[index] = std::move(record);
            }
            result.stepKeys_.push_back(*key.value); return Status::Ok();
        };
        auto status = command.section == Section::Upgrades ? edit(settings.custom->upgrades)
            : command.section == Section::TechRestrictions ? edit(settings.custom->techRestrictions)
            : command.section == Section::RandomUnits ? edit(settings.custom->randomUnits)
            : command.section == Section::RandomItems ? edit(settings.custom->randomItems)
            : Bad("SETTINGS_SECTION_INVALID", "未知自定义表类型。");
        if (!status.Succeeded()) {
            for (auto& diagnostic : status.diagnostics) diagnostic.message = "operations[" + std::to_string(operation) + "]: " + diagnostic.message;
            return fail(std::move(status));
        }
    }
    auto conflicts = Conflicts(settings); if (!conflicts.Succeeded()) return fail(conflicts);
    result.initializationBlocked_ = !settings.custom->randomUnits.empty() || !settings.custom->randomItems.empty();
    auto bytes = settings.Serialize(); if (!bytes.Succeeded()) return {{}, bytes.diagnostics};
    const std::vector<MapArchiveMember> replacements{{"war3map.w3i", *bytes.value}};
    auto after = MapData::ProjectMapData(*current, replacements); if (!after.Succeeded()) return {{}, after.diagnostics};
    result.changed_ = current->Member(MapDataKind::Settings).sources.front().data != *bytes.value;
    result.after_ = std::make_shared<const MapDocument>(std::move(*after.value));
    if (sources.verify) { auto verified = sources.verify(); if (!verified.Succeeded()) return fail(verified); }
    return R::Success(std::move(result));
}
Status MapCustomSettingsEditing::Apply(MapDataEditSession& session, const PreparedMapCustomSettingsEdit& plan,
    const MapCustomSettingsValidation& validation, const MapDataPublishValidator& validator) {
    if (!AuxiliaryEditing::AreEquivalentMapSources(session.Snapshot(), plan.Before())) return Bad("SETTINGS_PLAN_STALE", "准备后地图来源已变化，不能覆盖新编辑。");
    if (!plan.HasChanges()) return Status::Ok();
    if (!Script::RandomMapInitializationAvailable()) {
        if (plan.randomChanges_) return Bad("SETTINGS_RANDOM_COMMIT_UNSUPPORTED", "随机表提交尚未通过初始化和实际游戏验收。");
        if (plan.InitializationBlocked()) return Bad("MAP_INIT_RANDOM_TABLE_UNSUPPORTED", "当前构建尚未通过随机表初始化资格，当前仅允许预览。");
    }
    auto checked = Prepare(plan.before_, plan.request_, validation);
    if (!checked.Succeeded()) return {checked.diagnostics};
    if (!AuxiliaryEditing::AreEquivalentMapSources(checked.value->After(), plan.After())) return Bad("SETTINGS_PLAN_STALE", "重新准备结果与冻结候选不一致。");
    MapDataEditSession::Command command;
    auto prepared = session.Prepare(MapDataKind::Settings, Result<std::vector<std::byte>>::Success(
        plan.After().Member(MapDataKind::Settings).sources.front().data), command.changes);
    if (!prepared.Succeeded()) return prepared;
    return session.CommitCommand(std::move(command), [&](const auto& receipt) {
        if (validation.verify) { auto status = validation.verify(); if (!status.Succeeded()) return status; }
        return validator ? validator(plan.Before(), plan.After(), receipt) : Status::Ok();
    });
}
} // namespace War3Map::Services
