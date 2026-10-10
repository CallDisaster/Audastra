#include "LuaMapScriptInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/PlacementInitializationRules.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <tuple>

namespace War3Map::Script::lua {
namespace {
constexpr std::size_t kOutputBudget = 64 * 1024 * 1024;
constexpr MapInitStage kStages[]{MapInitStage::Sounds, MapInitStage::Regions, MapInitStage::Cameras,
    MapInitStage::Upgrades, MapInitStage::TechTree, MapInitStage::Destructables, MapInitStage::Items,
    MapInitStage::Units, MapInitStage::Blizzard, MapInitStage::Globals, MapInitStage::CustomTriggers,
    MapInitStage::InitializationTriggers};
constexpr const char* kFunctions[]{"InitSounds", "CreateRegions", "CreateCameras", "InitUpgrades",
    "InitTechTree", "CreateAllDestructables", "CreateAllItems", "CreateAllUnits", "InitBlizzard",
    "InitGlobals", "InitCustomTriggers", "RunInitializationTriggers"};

std::string Raw(const std::array<std::byte, 4>& value) {
    std::string result;
    for (const auto byte : value) result.push_back(static_cast<char>(byte));
    return result;
}
bool EmptyRaw(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char c) { return c == 0; });
}
double Facing(float radians) {
    // 与已有经典writer相同的单精度角度换算；不先走JASS文本。
    float value = radians * (180.0f / std::numbers::pi_v<float>);
    if (value < 0) value += 360.0f;
    return value;
}
void AppendNumber(std::string& target, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) target.push_back(static_cast<char>((value >> shift) & 0xffu));
}
void AppendString(std::string& target, std::string_view value) {
    AppendNumber(target, value.size()); target.append(value);
}
}

MapWriter::MapWriter(const MapInitializationPlan& plan, const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks)
    : plan_(plan), document_(document), catalog_(catalog), target_(target), callbacks_(callbacks) {}

[[noreturn]] void MapWriter::Fail(std::string code, std::string message) const {
    if (!currentObject_.empty()) message = currentObject_ + ": " + message;
    throw Diagnostic{DiagnosticSeverity::Error, "Script.LuaMap", std::move(code), std::move(message), currentOrigin_};
}

void MapWriter::Prepare() {
    std::vector<MapInitStage> expectedStages;
    for (const auto stage : kStages) {
        bool used = true;
        if (stage == MapInitStage::Cameras) used = !plan_.cameras.empty();
        if (stage == MapInitStage::Upgrades) used = plan_.settings.custom && !plan_.settings.custom->upgrades.empty();
        if (stage == MapInitStage::TechTree) used = plan_.settings.custom && !plan_.settings.custom->techRestrictions.empty();
        if (stage == MapInitStage::Destructables) used = !plan_.doodadCreationOrder.empty();
        if (stage == MapInitStage::Items) used = !plan_.itemCreationOrder.empty();
        if (used) expectedStages.push_back(stage);
    }
    if (plan_.mainOrder != expectedStages)
        Fail("LUA_MAP_INIT_ORDER", "初始化阶段缺失、重复或顺序未验证。");
    if ((plan_.playerSlotCount != 12 && plan_.playerSlotCount != 24)
        || plan_.neutralAggressive != plan_.playerSlotCount || plan_.neutralPassive != plan_.playerSlotCount + 3)
        Fail("LUA_MAP_PLAYER_LAYOUT", "玩家与中立槽位不是显式支持的12/24槽位布局。");
    for (const auto& api : target_.apis) apis_.emplace(api.name, &api);
    for (const auto& constant : target_.constants) constants_.emplace(constant.name, constant.type);
    for (const auto& [name, parent] : target_.handleTypes) parents_.emplace(name, parent);

    std::map<std::string, MapInitSymbol> expected;
    const auto symbol = [&](const std::string& name, std::string type, const char* member, std::size_t index) {
        if (!Identifier(name) || !expected.emplace(name, MapInitSymbol{name, std::move(type), member, index}).second)
            Fail("LUA_MAP_SYMBOL_INVALID", "初始化事实含无效或重复的对象符号：" + name);
    };
    for (const auto& unit : plan_.units) {
        if (!unit.startLocation) symbol(unit.symbol, unit.item ? "item" : "unit", "war3mapUnits.doo", unit.sourceIndex);
        if (!MapData::IsKnownFixedPlacementConfig(unit.placement.random)
            || MapData::IsRandomPlacementMarker(unit.placement.rawcode)
            || unit.placement.droppedItemTable != -1 || !unit.placement.itemSets.empty())
            Fail("LUA_MAP_RANDOM_DROP_UNSUPPORTED", "随机单位与掉落回调尚无完整Lua合同，不生成部分初始化。");
        if (unit.placement.skinId)
            Fail("LUA_MAP_SKIN_UNSUPPORTED", "皮肤ID尚未绑定到目标创建API，原始记录必须保留。");
        if (unit.placement.owner < 0 || static_cast<std::uint32_t>(unit.placement.owner) > plan_.neutralPassive)
            Fail("LUA_MAP_PLAYER_LAYOUT", "预放对象所有者超出所选布局。");
    }
    for (const auto& doodad : plan_.doodads) {
        if (doodad.scripted) symbol(doodad.symbol, "destructable", "war3map.doo", doodad.sourceIndex);
        if (doodad.placement.droppedItemTable != -1 || !doodad.placement.itemSets.empty())
            Fail("LUA_MAP_RANDOM_DROP_UNSUPPORTED", "可破坏物掉落回调尚无完整Lua合同。");
        if (doodad.scripted && (doodad.placement.skinId || doodad.placement.lifePercent != 100
                || (doodad.placement.flags != 0 && doodad.placement.flags != 4)))
            Fail("LUA_MAP_DESTRUCTABLE_UNSUPPORTED", "脚本化可破坏物含未支持的皮肤、生命或创建标志。");
    }
    for (const auto& sound : plan_.sounds) symbol(sound.symbol, sound.sound.IsMusic() ? "string" : "sound", "war3map.w3s", sound.sourceIndex);
    for (const auto& region : plan_.regions) symbol(region.symbol, "rect", "war3map.w3r", region.sourceIndex);
    for (const auto& camera : plan_.cameras) symbol(camera.symbol, "camerasetup", "war3map.w3c", camera.sourceIndex);
    for (const auto& entry : plan_.symbols) {
        const auto found = expected.find(entry.name);
        if (found == expected.end() || found->second.type != entry.type || found->second.member != entry.member
            || found->second.sourceIndex != entry.sourceIndex || !symbols_.emplace(entry.name, entry).second)
            Fail("LUA_MAP_SYMBOL_DIRECTORY", "对象符号目录与初始化记录不一致：" + entry.name);
    }
    if (symbols_.size() != expected.size()) Fail("LUA_MAP_SYMBOL_DIRECTORY", "对象符号目录遗漏初始化记录。");
    const auto checkOrder = [&](const auto& records, const auto& order, const auto& included) {
        std::vector<bool> seen(records.size());
        for (const auto index : order) {
            if (index >= records.size() || seen[index] || !included(records[index]))
                Fail("LUA_MAP_OBJECT_ORDER", "创建顺序越界、重复或引用了错误对象类别。");
            seen[index] = true;
        }
        for (std::size_t index = 0; index < records.size(); ++index)
            if (seen[index] != included(records[index])) Fail("LUA_MAP_OBJECT_ORDER", "创建顺序遗漏对象。");
    };
    checkOrder(plan_.units, plan_.unitCreationOrder, [](const auto& value) { return !value.item && !value.startLocation; });
    checkOrder(plan_.units, plan_.itemCreationOrder, [](const auto& value) { return value.item && !value.startLocation; });
    checkOrder(plan_.doodads, plan_.doodadCreationOrder, [](const auto& value) { return value.scripted; });
    if (plan_.settings.custom && (!plan_.settings.custom->randomUnits.empty() || !plan_.settings.custom->randomItems.empty()))
        Fail("LUA_MAP_RANDOM_DROP_UNSUPPORTED", "地图随机表尚未获得完整Lua初始化支持。");
    for (const auto& player : plan_.settings.players) {
        if (player.number < 0 || static_cast<std::uint32_t>(player.number) >= plan_.playerSlotCount)
            Fail("LUA_MAP_PLAYER_LAYOUT", "地图玩家编号超出所选布局。");
        if (player.enemyLowPriorityMask || player.enemyHighPriorityMask)
            Fail("LUA_MAP_ENEMY_PRIORITY_UNSUPPORTED", "新版敌人起点优先级尚未绑定到目标API。");
    }
}

bool MapWriter::Assignable(std::string actual, std::string_view expected) const {
    if (actual == expected || (actual == "integer" && expected == "real")) return true;
    std::set<std::string> seen;
    while (seen.insert(actual).second) {
        const auto found = parents_.find(actual);
        if (found == parents_.end()) return false;
        actual = found->second;
        if (actual == expected) return true;
    }
    return false;
}

Expression MapWriter::Api(std::string_view name, std::string_view result, std::initializer_list<Expression> arguments) {
    const auto found = apis_.find(std::string(name));
    if (found == apis_.end()) Fail("LUA_MAP_API_MISSING", "目标未声明初始化所需API：" + std::string(name));
    const auto& api = *found->second;
    if (api.parameters.size() != arguments.size() || !Assignable(api.returns, result))
        Fail("LUA_MAP_API_SIGNATURE", "目标API的返回或参数数量不匹配：" + std::string(name));
    std::string code(name); code += "(";
    std::size_t index = 0;
    for (const auto& argument : arguments) {
        if (!Assignable(argument.type, api.parameters[index]))
            Fail("LUA_MAP_API_SIGNATURE", "目标API参数类型不匹配：" + std::string(name) + "[" + std::to_string(index) + "]");
        if (index++) code += ", ";
        code += argument.code;
    }
    code += ")";
    requiredApis_.emplace(name);
    return {api.returns, std::move(code), {}};
}

Expression MapWriter::Constant(std::string_view name, std::string_view type) {
    const auto found = constants_.find(std::string(name));
    if (found == constants_.end() || !Assignable(found->second, type))
        Fail("LUA_MAP_CONSTANT_MISSING", "目标未声明正确类型的引擎常量：" + std::string(name));
    requiredConstants_.emplace(name);
    return {found->second, found->first, {}};
}
Expression MapWriter::Global(std::string_view name, std::string_view type) {
    const auto found = symbols_.find(std::string(name));
    if (found == symbols_.end() || !Assignable(found->second.type, type))
        Fail("LUA_MAP_GLOBAL_UNBOUND", "初始化引用未绑定到匹配类型的对象：" + std::string(name));
    requiredGlobals_.emplace(name);
    return {found->second.type, found->first, {}};
}
Expression MapWriter::Real(double value) const {
    if (!std::isfinite(value)) Fail("LUA_MAP_REAL_INVALID", "初始化浮点值不是有限数。");
    char buffer[64];
    const auto conversion = std::to_chars(std::begin(buffer), std::end(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (conversion.ec != std::errc{}) Fail("LUA_MAP_REAL_INVALID", "初始化浮点值无法无损编码。");
    std::string code(buffer, conversion.ptr);
    if (code.find_first_of(".eE") == std::string::npos) code += ".0";
    return {"real", std::move(code), {}};
}
Expression MapWriter::Integer(std::int64_t value) { return {"integer", std::to_string(value), {}}; }
Expression MapWriter::Boolean(bool value) { return {"boolean", value ? "true" : "false", {}}; }
Expression MapWriter::String(std::string_view value) { return {"string", Quote(value), std::string(value)}; }
Expression MapWriter::Rawcode(std::string_view value) {
    if (value.size() != 4) Fail("LUA_MAP_RAWCODE_INVALID", "对象ID不是精确四字节。");
    return Api("FourCC", "integer", {String(value)});
}
Expression MapWriter::Player(std::int32_t number) { return Api("Player", "player", {Integer(number)}); }
std::string MapWriter::Resolve(std::string_view text) const {
    if (!text.starts_with("TRIGSTR_")) return std::string(text);
    if (!plan_.strings) Fail("LUA_MAP_STRING_UNRESOLVED", "WTS引用没有owned字符串表。");
    const auto value = plan_.strings->Resolve(text);
    if (!value.Succeeded()) Fail("LUA_MAP_STRING_UNRESOLVED", "WTS引用无法从owned来源解析：" + std::string(text));
    return *value.value;
}
void MapWriter::Line(std::string_view value) {
    if (value.size() > kOutputBudget || out_.Size() > kOutputBudget - value.size())
        Fail("LUA_MAP_OUTPUT_BUDGET", "完整地图Lua文本超过64 MiB字节预算。");
    out_.Line(value);
    if (out_.Size() > kOutputBudget) Fail("LUA_MAP_OUTPUT_BUDGET", "完整地图Lua文本超过64 MiB字节预算。");
}
void MapWriter::Call(std::string_view name, std::initializer_list<Expression> arguments) {
    Line(Api(name, "nothing", arguments).code);
}
void MapWriter::Begin(std::string_view name) { Line("function " + std::string(name) + "()"); out_.Begin(); }
void MapWriter::End() { out_.End(); Line("end"); Line(""); }
std::size_t MapWriter::ObjectBegin(std::string_view name) {
    const auto& source = symbols_.at(std::string(name));
    currentObject_ = source.name + " (record " + std::to_string(source.sourceIndex) + ")";
    currentOrigin_ = {source.member, 0, 0, {}};
    return out_.Size();
}
void MapWriter::ObjectEnd(std::string_view name, std::size_t start) {
    objectSpans_.push_back({symbols_.at(std::string(name)), start, out_.Size() - start});
    currentObject_.clear(); currentOrigin_ = {};
}
void MapWriter::Declarations() {
    Line("-- FORGE complete map Lua; generated from owned typed map facts and natural-language T.");
    Line("-- Native stubs are backend evidence only; Warcraft Lua and multiplayer require separate acceptance.");
    for (const auto& source : plan_.symbols) {
        const auto start = ObjectBegin(source.name);
        Line(source.name + " = nil");
        ObjectEnd(source.name, start);
    }
    Line("");
}

void MapWriter::Sounds() {
    Begin("InitSounds");
    for (const auto& value : plan_.sounds) {
        const auto start = ObjectBegin(value.symbol);
        const auto& sound = value.sound;
        if (sound.extended) Fail("LUA_MAP_SOUND_UNSUPPORTED", "新版声音扩展字段尚未支持。");
        if (sound.IsMusic()) {
            Line(value.symbol + " = " + String(sound.filePath).code);
            ObjectEnd(value.symbol, start); continue;
        }
        // classic native没有独立设置priority/pitchVariance的入口；不能把任意override说成label等价。
        if (!MapData::IsSoundDefaultInteger(sound.priority) || !MapData::IsSoundDefaultReal(sound.pitchVariance))
            Fail("LUA_SOUND_PARAMETER_UNSUPPORTED", "显式priority/pitchVariance缺少可证明的目标设置路径，拒绝静默丢弃。");
        const auto ref = Global(value.symbol, "sound");
        Line(value.symbol + " = " + Api("CreateSound", "sound", {String(sound.filePath), Boolean(sound.IsLooping()),
            Boolean(sound.Is3D()), Boolean(sound.StopsOutOfRange()), Integer(sound.fadeInRate), Integer(sound.fadeOutRate),
            String(sound.eaxSetting)}).code);
        if (value.label) Call("SetSoundParamsFromLabel", {ref, String(*value.label)});
        if (value.duration) Call("SetSoundDuration", {ref, Integer(*value.duration)});
        if (!MapData::IsSoundDefaultInteger(sound.channel)) Call("SetSoundChannel", {ref, Integer(sound.channel)});
        if (!MapData::IsSoundDefaultInteger(sound.volume)) Call("SetSoundVolume", {ref, Integer(sound.volume)});
        if (!MapData::IsSoundDefaultReal(sound.pitch)) Call("SetSoundPitch", {ref, Real(sound.pitch)});
        if (sound.Is3D()) {
            const auto uniform = [&](std::initializer_list<bool> defaults) {
                const bool first = *defaults.begin();
                if (!std::all_of(defaults.begin(), defaults.end(), [&](bool valueDefault) { return first == valueDefault; }))
                    Fail("LUA_SOUND_DEFAULT_GROUP_MIXED", "声音同组参数混合默认哨兵与显式值。");
                return first;
            };
            if (!uniform({MapData::IsSoundDefaultReal(sound.minDistance), MapData::IsSoundDefaultReal(sound.maxDistance)}))
                Call("SetSoundDistances", {ref, Real(sound.minDistance), Real(sound.maxDistance)});
            if (!MapData::IsSoundDefaultReal(sound.distanceCutoff)) Call("SetSoundDistanceCutoff", {ref, Real(sound.distanceCutoff)});
            if (!uniform({MapData::IsSoundDefaultReal(sound.coneAngleInside), MapData::IsSoundDefaultReal(sound.coneAngleOutside),
                    MapData::IsSoundDefaultInteger(sound.coneOutsideVolume)}))
                Call("SetSoundConeAngles", {ref, Real(sound.coneAngleInside), Real(sound.coneAngleOutside), Integer(sound.coneOutsideVolume)});
            if (!uniform({MapData::IsSoundDefaultReal(sound.coneOrientation[0]), MapData::IsSoundDefaultReal(sound.coneOrientation[1]),
                    MapData::IsSoundDefaultReal(sound.coneOrientation[2])}))
                Call("SetSoundConeOrientation", {ref, Real(sound.coneOrientation[0]), Real(sound.coneOrientation[1]), Real(sound.coneOrientation[2])});
        }
        ObjectEnd(value.symbol, start);
    }
    End();
}

void MapWriter::Regions() {
    Begin("CreateRegions");
    for (const auto& value : plan_.regions) {
        const auto start = ObjectBegin(value.symbol);
        const auto& region = value.region;
        if (!region.ambientSound.empty()) Fail("LUA_MAP_REGION_SOUND_UNSUPPORTED", "区域环境音尚无完整初始化路径。");
        Line(value.symbol + " = " + Api("Rect", "rect", {Real(region.left), Real(region.bottom), Real(region.right), Real(region.top)}).code);
        const auto weather = Raw(region.weatherId);
        if (!EmptyRaw(weather)) Call("EnableWeatherEffect", {Api("AddWeatherEffect", "weathereffect",
            {Global(value.symbol, "rect"), Rawcode(weather)}), Boolean(true)});
        ObjectEnd(value.symbol, start);
    }
    End();
}

void MapWriter::Cameras() {
    Begin("CreateCameras");
    for (const auto& value : plan_.cameras) {
        const auto start = ObjectBegin(value.symbol);
        const auto& camera = value.camera;
        if (camera.localRotation) Fail("LUA_MAP_CAMERA_UNSUPPORTED", "新版镜头局部旋转尚无完整初始化路径。");
        const auto ref = Global(value.symbol, "camerasetup");
        Line(value.symbol + " = " + Api("CreateCameraSetup", "camerasetup").code);
        const std::pair<const char*, float> fields[]{
            {"CAMERA_FIELD_ZOFFSET", camera.zOffset}, {"CAMERA_FIELD_ROTATION", camera.rotation},
            {"CAMERA_FIELD_ANGLE_OF_ATTACK", camera.angleOfAttack}, {"CAMERA_FIELD_TARGET_DISTANCE", camera.targetDistance},
            {"CAMERA_FIELD_ROLL", camera.roll}, {"CAMERA_FIELD_FIELD_OF_VIEW", camera.fieldOfView},
            {"CAMERA_FIELD_FARZ", camera.farClippingPlane}, {"CAMERA_FIELD_NEARZ", camera.nearClippingPlane}};
        for (const auto& [name, number] : fields)
            Call("CameraSetupSetField", {ref, Constant(name, "camerafield"), Real(number), Real(0)});
        Call("CameraSetupSetDestPosition", {ref, Real(camera.targetX), Real(camera.targetY), Real(0)});
        ObjectEnd(value.symbol, start);
    }
    End();
}

void MapWriter::Destructables() {
    Begin("CreateAllDestructables");
    for (const auto index : plan_.doodadCreationOrder) {
        const auto& value = plan_.doodads[index];
        const auto start = ObjectBegin(value.symbol);
        const auto& entry = value.placement;
        Expression creation;
        if (entry.flags == 4) creation = Api("CreateDestructableZ", "destructable", {Rawcode(entry.rawcode),
            Real(entry.position[0]), Real(entry.position[1]), Real(entry.position[2]), Real(Facing(entry.rotation)),
            Real(entry.scale[0]), Integer(entry.variation)});
        else creation = Api("CreateDestructable", "destructable", {Rawcode(entry.rawcode), Real(entry.position[0]),
            Real(entry.position[1]), Real(Facing(entry.rotation)), Real(entry.scale[0]), Integer(entry.variation)});
        Line(value.symbol + " = " + creation.code);
        ObjectEnd(value.symbol, start);
    }
    End();
}

void MapWriter::Items() {
    Begin("CreateAllItems");
    for (const auto index : plan_.itemCreationOrder) {
        const auto& value = plan_.units[index];
        const auto start = ObjectBegin(value.symbol);
        Line(value.symbol + " = " + Api("CreateItem", "item", {Rawcode(value.placement.rawcode),
            Real(value.placement.position[0]), Real(value.placement.position[1])}).code);
        ObjectEnd(value.symbol, start);
    }
    End();
}

void MapWriter::Unit(const MapInitUnit& value) {
    const auto start = ObjectBegin(value.symbol);
    const auto& entry = value.placement;
    const auto ref = Global(value.symbol, "unit");
    Expression creation;
    if (entry.rawcode == "ugol") creation = Api("CreateBlightedGoldmine", "unit", {Player(entry.owner),
        Real(entry.position[0]), Real(entry.position[1]), Real(Facing(entry.rotation))});
    else creation = Api("CreateUnit", "unit", {Player(entry.owner), Rawcode(entry.rawcode),
        Real(entry.position[0]), Real(entry.position[1]), Real(Facing(entry.rotation))});
    Line(value.symbol + " = " + creation.code);
    if (entry.hitPoints != -1) {
        const auto life = Constant("UNIT_STATE_LIFE", "unitstate");
        const auto current = Api("GetUnitState", "real", {ref, life});
        Call("SetUnitState", {ref, life, {"real", "(" + Real(entry.hitPoints / 100.0).code + " * " + current.code + ")", {}}});
    }
    if (entry.rawcode == "ngol" || entry.rawcode == "ugol") Call("SetResourceAmount", {ref, Integer(entry.gold)});
    if (value.hero) {
        if (entry.heroLevel > 1) Call("SetHeroLevel", {ref, Integer(entry.heroLevel), Boolean(false)});
        if (entry.heroStrength > 0) Call("SetHeroStr", {ref, Integer(entry.heroStrength), Boolean(true)});
        if (entry.heroAgility > 0) Call("SetHeroAgi", {ref, Integer(entry.heroAgility), Boolean(true)});
        if (entry.heroIntelligence > 0) Call("SetHeroInt", {ref, Integer(entry.heroIntelligence), Boolean(true)});
    }
    if (entry.mana != -1) Call("SetUnitState", {ref, Constant("UNIT_STATE_MANA", "unitstate"), Integer(entry.mana)});
    const auto color = entry.teamColor != -1 ? entry.teamColor : value.typeTeamColor;
    if (color != -1) Call("SetUnitColor", {ref, Api("ConvertPlayerColor", "playercolor", {Integer(color)})});
    if (entry.targetAcquisition == -2.0f) Call("SetUnitAcquireRange", {ref, Real(200)});
    else if (entry.targetAcquisition >= 0) Call("SetUnitAcquireRange", {ref, Real(entry.targetAcquisition)});
    if (entry.abilities.size() != value.abilities.size()) Fail("LUA_MAP_ABILITY_UNRESOLVED", "放置技能与owned技能事实数量不一致。");
    for (std::size_t index = 0; index < value.abilities.size(); ++index) {
        const auto& ability = value.abilities[index];
        if (entry.abilities[index].rawcode != ability.placement.rawcode || entry.abilities[index].level != ability.placement.level
            || entry.abilities[index].autocast != ability.placement.autocast || !ability.effective.known)
            Fail("LUA_MAP_ABILITY_UNRESOLVED", "放置技能缺少一致的有效物编事实。");
        const auto& info = ability.effective;
        if (info.hero && !value.hero) continue; // 经典WE忽略普通单位上的英雄技能。
        const bool selected = value.hero && info.hero && ability.placement.level > 0;
        for (std::int32_t level = 0; selected && level < ability.placement.level; ++level)
            Call("SelectHeroSkill", {ref, Rawcode(ability.placement.rawcode)});
        if (ability.placement.autocast || info.autocast || (info.custom && selected)) {
            const auto& order = ability.placement.autocast ? info.orderOn : info.orderOff;
            if (order.empty()) Fail("LUA_MAP_ABILITY_UNRESOLVED", "技能所需命令串缺失。");
            Line(Api("IssueImmediateOrder", "boolean", {ref, String(order)}).code);
        }
    }
    for (const auto& item : entry.inventory)
        Line(Api("UnitAddItemToSlotById", "boolean", {ref, Rawcode(item.rawcode), Integer(item.slot)}).code);
    if (entry.waygateRegionId != -1) {
        if (!value.waygateRegionSymbol) Fail("LUA_MAP_WAYGATE_UNBOUND", "传送门没有解析到目标区域。");
        const auto region = Global(*value.waygateRegionSymbol, "rect");
        Call("WaygateSetDestination", {ref, Api("GetRectCenterX", "real", {region}), Api("GetRectCenterY", "real", {region})});
        Call("WaygateActivate", {ref, Boolean(true)});
    }
    ObjectEnd(value.symbol, start);
}

void MapWriter::Units() {
    Begin("CreateAllUnits");
    for (const auto index : plan_.unitCreationOrder) Unit(plan_.units[index]);
    End();
}

void MapWriter::Upgrades() {
    Begin("InitUpgrades");
    if (plan_.settings.custom) for (const auto& player : plan_.settings.players) {
        std::vector<std::string> order;
        std::map<std::string, std::pair<std::int32_t, std::int32_t>> states;
        for (const auto& upgrade : plan_.settings.custom->upgrades) {
            if ((upgrade.playerMask & (1u << player.number)) == 0 || upgrade.availability == 1) continue;
            if (upgrade.availability != 0 && upgrade.availability != 2)
                Fail("LUA_MAP_UPGRADE_UNSUPPORTED", "升级可用性不是已知0/1/2状态。");
            const auto id = Raw(upgrade.id);
            auto [found, inserted] = states.try_emplace(id, upgrade.availability, upgrade.level);
            if (inserted) order.push_back(id);
            else if (upgrade.availability == 2 && found->second.first == 2) found->second.second = std::max(found->second.second, upgrade.level);
            else if (upgrade.availability == 0 && found->second.first == 0) found->second.second = std::min(found->second.second, upgrade.level);
            else Fail("LUA_MAP_UPGRADE_CONFLICT", "同一玩家科技同时为禁用与已研究，拒绝猜测覆盖顺序。");
        }
        for (const auto& id : order) {
            const auto [availability, level] = states.at(id);
            const auto effective = static_cast<std::int64_t>(level) + (availability == 2 ? 1 : 0);
            if (effective < 0 || effective > std::numeric_limits<std::int32_t>::max())
                Fail("LUA_MAP_UPGRADE_UNSUPPORTED", "科技等级超出目标整数范围。");
            Call(availability == 0 ? "SetPlayerTechMaxAllowed" : "SetPlayerTechResearched", {Player(player.number), Rawcode(id), Integer(effective)});
        }
    }
    End();
}

void MapWriter::TechTree() {
    Begin("InitTechTree");
    if (plan_.settings.custom) for (const auto& player : plan_.settings.players)
        for (const auto& restriction : plan_.settings.custom->techRestrictions)
            if ((restriction.playerMask & (1u << player.number)) != 0)
                Call("SetPlayerTechMaxAllowed", {Player(player.number), Rawcode(Raw(restriction.id)), Integer(0)});
    End();
}

void MapWriter::Players() {
    const auto& players = plan_.settings.players;
    Begin("InitCustomPlayerSlots");
    for (std::size_t index = 0; index < players.size(); ++index) {
        const auto& player = players[index];
        const auto ref = Player(player.number);
        Call("SetPlayerStartLocation", {ref, Integer(static_cast<std::int64_t>(index))});
        if ((player.flags & 1u) != 0) Call("ForcePlayerStartLocation", {ref, Integer(static_cast<std::int64_t>(index))});
        Call("SetPlayerColor", {ref, Api("ConvertPlayerColor", "playercolor", {Integer(player.number)})});
        const char* race = nullptr;
        switch (player.race) {
        case 0: race = "RACE_PREF_RANDOM"; break;
        case 1: race = "RACE_PREF_HUMAN"; break;
        case 2: race = "RACE_PREF_ORC"; break;
        case 3: race = "RACE_PREF_UNDEAD"; break;
        case 4: race = "RACE_PREF_NIGHTELF"; break;
        default: Fail("LUA_MAP_PLAYER_RACE", "玩家种族未验证。");
        }
        Call("SetPlayerRacePreference", {ref, Constant(race, "racepreference")});
        Call("SetPlayerRaceSelectable", {ref, Boolean(player.race == 0 || (plan_.settings.flags & 0x20u) == 0)});
        const char* controller = nullptr;
        switch (player.controller) {
        case 0: case 1: controller = "MAP_CONTROL_USER"; break;
        case 2: controller = "MAP_CONTROL_COMPUTER"; break;
        case 3: controller = "MAP_CONTROL_NEUTRAL"; break;
        case 4: controller = "MAP_CONTROL_RESCUABLE"; break;
        default: Fail("LUA_MAP_PLAYER_CONTROLLER", "玩家控制器未验证。");
        }
        Call("SetPlayerController", {ref, Constant(controller, "mapcontrol")});
    }
    End();
    Begin("InitCustomTeams");
    if ((plan_.settings.flags & 0x40u) != 0) for (std::size_t index = 0; index < plan_.settings.forces.size(); ++index) {
        const auto& force = plan_.settings.forces[index];
        std::vector<std::int32_t> members;
        for (const auto& player : players) if ((force.playerMask & (1u << player.number)) != 0) {
            members.push_back(player.number);
            Call("SetPlayerTeam", {Player(player.number), Integer(static_cast<std::int64_t>(index))});
        }
        const std::pair<std::uint32_t, const char*> flags[]{{1u, "SetPlayerAllianceStateAllyBJ"},
            {8u, "SetPlayerAllianceStateVisionBJ"}, {16u, "SetPlayerAllianceStateControlBJ"}, {32u, "SetPlayerAllianceStateFullControlBJ"}};
        for (const auto& [flag, name] : flags) if ((force.flags & flag) != 0)
            for (const auto first : members) for (const auto second : members) if (first != second)
                Call(name, {Player(first), Player(second), Boolean(true)});
    }
    End();
    Begin("InitAllyPriorities");
    for (std::size_t index = 0; index < players.size(); ++index) {
        std::vector<std::pair<std::size_t, bool>> priorities;
        for (std::size_t other = 0; other < players.size(); ++other) {
            if (other == index) continue;
            const auto bit = 1u << players[other].number;
            if ((players[index].allyHighPriorityMask & bit) != 0) priorities.emplace_back(other, true);
            else if ((players[index].allyLowPriorityMask & bit) != 0) priorities.emplace_back(other, false);
        }
        if (priorities.empty()) continue;
        Call("SetStartLocPrioCount", {Integer(static_cast<std::int64_t>(index)), Integer(static_cast<std::int64_t>(priorities.size()))});
        for (std::size_t rank = 0; rank < priorities.size(); ++rank)
            Call("SetStartLocPrio", {Integer(static_cast<std::int64_t>(index)), Integer(static_cast<std::int64_t>(rank)),
                Integer(static_cast<std::int64_t>(priorities[rank].first)),
                Constant(priorities[rank].second ? "MAP_LOC_PRIO_HIGH" : "MAP_LOC_PRIO_LOW", "startlocprio")});
    }
    End();
}

void MapWriter::Main() {
    Begin("main");
    currentOrigin_ = {"war3map.w3i", 0, 0, {}};
    const auto& bounds = plan_.cameraBounds;
    const auto adjusted = [&](std::size_t index, const char* margin, bool plus) {
        const auto extra = Api("GetCameraMargin", "real", {Constant(margin, "integer")});
        return Expression{"real", "(" + Real(bounds[index]).code + (plus ? " + " : " - ") + extra.code + ")", {}};
    };
    Call("SetCameraBounds", {adjusted(0, "CAMERA_MARGIN_LEFT", true), adjusted(1, "CAMERA_MARGIN_BOTTOM", true),
        adjusted(2, "CAMERA_MARGIN_RIGHT", false), adjusted(3, "CAMERA_MARGIN_TOP", false),
        adjusted(4, "CAMERA_MARGIN_LEFT", true), adjusted(5, "CAMERA_MARGIN_TOP", false),
        adjusted(6, "CAMERA_MARGIN_RIGHT", false), adjusted(7, "CAMERA_MARGIN_BOTTOM", true)});
    if (plan_.dayNightGroup.empty() || plan_.ambientSound.empty()) Fail("LUA_MAP_ENVIRONMENT_UNRESOLVED", "昼夜模型或环境音缺少解析事实。");
    const auto light = [&](const char* kind) {
        return String("Environment\\DNC\\DNC" + plan_.dayNightGroup + "\\DNC" + plan_.dayNightGroup + kind
            + "\\DNC" + plan_.dayNightGroup + kind + ".mdl");
    };
    Call("SetDayNightModels", {light("Terrain"), light("Unit")});
    const auto& environment = plan_.settings.environment;
    if ((plan_.settings.flags & MapData::kMapFlagTerrainFog) != 0) {
        const auto& color = environment.fogColor;
        Call("SetTerrainFogEx", {Integer(environment.fogStyle), Real(environment.fogStart), Real(environment.fogEnd),
            Real(environment.fogDensity), Real(color.red / 255.0), Real(color.green / 255.0), Real(color.blue / 255.0)});
    }
    if ((plan_.settings.flags & 0x10000u) != 0) {
        const auto& color = environment.waterColor;
        Call("SetWaterBaseColor", {Integer(color.red), Integer(color.green), Integer(color.blue), Integer(color.alpha)});
    }
    const auto weather = Raw(environment.weather);
    if (!EmptyRaw(weather)) {
        if (!plan_.weatherBounds) Fail("LUA_MAP_WEATHER_BOUNDS", "全局天气缺少完整地形范围。");
        const auto& box = *plan_.weatherBounds;
        const auto rect = Api("Rect", "rect", {Real(box[0]), Real(box[1]), Real(box[2]), Real(box[3])});
        Call("EnableWeatherEffect", {Api("AddWeatherEffect", "weathereffect", {rect, Rawcode(weather)}), Boolean(true)});
    }
    Call("NewSoundEnvironment", {String(environment.soundEnvironment.empty() ? "Default" : environment.soundEnvironment)});
    Call("SetAmbientDaySound", {String(plan_.ambientSound + "Day")});
    Call("SetAmbientNightSound", {String(plan_.ambientSound + "Night")});
    Call("SetMapMusic", {String("Music"), Boolean(true), Integer(0)});
    currentOrigin_ = {};
    for (const auto stage : plan_.mainOrder) {
        if (stage == MapInitStage::Blizzard) Call("InitBlizzard");
        else Line(std::string(kFunctions[static_cast<std::size_t>(stage)]) + "()");
    }
    End();
}

void MapWriter::Config() {
    Begin("config");
    currentOrigin_ = {"war3map.w3i", 0, 0, {}};
    Call("SetMapName", {String(Resolve(plan_.settings.mapName))});
    Call("SetMapDescription", {String(Resolve(plan_.settings.description))});
    const auto& players = plan_.settings.players;
    Call("SetPlayers", {Integer(static_cast<std::int64_t>(players.size()))});
    Call("SetTeams", {Integer(static_cast<std::int64_t>(players.size()))});
    const bool hasPriorities = std::any_of(players.begin(), players.end(), [](const auto& player) {
        return player.allyLowPriorityMask || player.allyHighPriorityMask;
    });
    Call("SetGamePlacement", {Constant(hasPriorities ? "MAP_PLACEMENT_TEAMS_TOGETHER" : "MAP_PLACEMENT_USE_MAP_SETTINGS", "placement")});
    for (std::size_t index = 0; index < players.size(); ++index)
        Call("DefineStartLocation", {Integer(static_cast<std::int64_t>(index)), Real(players[index].startX), Real(players[index].startY)});
    Line("InitCustomPlayerSlots()");
    if ((plan_.settings.flags & 0x40u) != 0) Line("InitCustomTeams()");
    else {
        for (const auto& player : players) Call("SetPlayerSlotAvailable", {Player(player.number), Constant("MAP_CONTROL_USER", "mapcontrol")});
        Call("InitGenericPlayerSlots");
    }
    Line("InitAllyPriorities()");
    currentOrigin_ = {};
    End();
}

Result<std::string> ComputeLuaMapSourceMapSha256(const LuaMapScriptResult& result) {
    const auto failure = [](const char* code, const char* message) {
        return Result<std::string>::Failure("Script.LuaMap", code, message);
    };
    const auto digest = [](std::string_view value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (result.text.empty() || result.text.size() > kOutputBudget || !digest(result.scriptSha256) || !digest(result.targetSha256))
        return failure("LUA_MAP_IDENTITY_INVALID", "完整Lua文本或SHA身份无效。");
    const auto scriptHash = Sha256Text(result.text);
    if (!scriptHash.Succeeded()) return scriptHash;
    if (*scriptHash.value != result.scriptSha256)
        return failure("LUA_MAP_SCRIPT_CHANGED", "完整Lua实际字节与生成时的SHA不一致。");
    const auto validRange = [&](std::size_t offset, std::size_t length) {
        return length != 0 && offset <= result.text.size() && length <= result.text.size() - offset;
    };
    for (const auto& span : result.sourceSpans) {
        const auto header = span.kind == LuaSourceSpanKind::AuthorHeader;
        if ((header ? (span.nodeId != 0 || span.triggerId != 0) : span.nodeId == 0)
            || (span.kind == LuaSourceSpanKind::AuthorCustom && span.triggerId != span.nodeId)
            || !validRange(span.byteOffset, span.byteLength)
            || static_cast<unsigned>(span.kind) > static_cast<unsigned>(LuaSourceSpanKind::AuthorCustom))
            return failure("LUA_MAP_SOURCE_SPAN_INVALID", "T来源映射范围或身份无效。");
    }
    for (const auto& span : result.objectSpans) {
        if (!validRange(span.byteOffset, span.byteLength) || !Identifier(span.source.name)
            || span.source.type.empty() || span.source.member.empty())
            return failure("LUA_MAP_SOURCE_SPAN_INVALID", "对象来源映射范围或身份无效。");
    }
    std::string identity = "forge-complete-lua-source-map-v1:" + *scriptHash.value;
    AppendString(identity, result.targetSha256);
    AppendNumber(identity, result.sourceSpans.size());
    for (const auto& span : result.sourceSpans) {
        AppendNumber(identity, span.byteOffset); AppendNumber(identity, span.byteLength);
        AppendNumber(identity, span.triggerId); AppendNumber(identity, span.nodeId); AppendNumber(identity, static_cast<std::uint64_t>(span.kind));
    }
    AppendNumber(identity, result.objectSpans.size());
    for (const auto& span : result.objectSpans) {
        AppendNumber(identity, span.byteOffset); AppendNumber(identity, span.byteLength);
        AppendString(identity, span.source.name); AppendString(identity, span.source.type); AppendString(identity, span.source.member);
        AppendNumber(identity, span.source.sourceIndex);
    }
    return Sha256Text(identity);
}

void MapWriter::Finish() {
    const auto scriptHash = Sha256Text(out_.Text());
    if (!scriptHash.Succeeded()) Fail("LUA_MAP_HASH_FAILED", "整图脚本指纹生成失败。");
    std::sort(objectSpans_.begin(), objectSpans_.end(), [](const auto& left, const auto& right) {
        return std::tie(left.byteOffset, left.source.name) < std::tie(right.byteOffset, right.source.name);
    });
    result_.scriptSha256 = *scriptHash.value;
    result_.objectSpans = std::move(objectSpans_);
    result_.text = out_.Take();
    const auto mapHash = ComputeLuaMapSourceMapSha256(result_);
    if (!mapHash.Succeeded()) throw mapHash.diagnostics.front();
    result_.sourceMapSha256 = *mapHash.value;
}

LuaMapScriptResult MapWriter::Run() {
    result_.targetIdentity = target_.identity;
    try {
        Prepare();
        std::vector<LuaGlobalBinding> globals;
        globals.reserve(plan_.symbols.size());
        for (const auto& symbol : plan_.symbols) globals.push_back({symbol.name, symbol.type});
        result_.triggers = GenerateLuaTriggerUnit(document_, catalog_, target_, callbacks_, globals);
        for (const auto& diagnostic : result_.triggers.diagnostics) {
            if (diagnostic.diagnostic.code != "LUA_RUNTIME_UNVERIFIED") result_.diagnostics.push_back(diagnostic.diagnostic);
        }
        if (!result_.triggers.Succeeded()) return std::move(result_);
        result_.targetSha256 = result_.triggers.targetSha256;
        requiredApis_.insert(result_.triggers.requiredApis.begin(), result_.triggers.requiredApis.end());
        requiredGlobals_.insert(result_.triggers.requiredGlobals.begin(), result_.triggers.requiredGlobals.end());
        requiredConstants_.insert(result_.triggers.requiredConstants.begin(), result_.triggers.requiredConstants.end());
        Declarations();
        const auto triggerOffset = out_.Size();
        out_.Raw(result_.triggers.text);
        for (auto span : result_.triggers.sourceSpans) { span.byteOffset += triggerOffset; result_.sourceSpans.push_back(span); }
        Sounds(); Regions(); Cameras(); Destructables(); Items(); Units(); Upgrades(); TechTree(); Players(); Main(); Config();
        Finish();
        result_.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.LuaMap", "LUA_MAP_RUNTIME_UNVERIFIED",
            "已生成整图config/main、对象初始化与T；实际Lua执行的native替身不是游戏或联机签收，实数不承诺经典float32逐位等价。", {}});
    } catch (const Diagnostic& failure) {
        result_.diagnostics.push_back(failure);
    } catch (const std::bad_alloc&) {
        result_.diagnostics.push_back({DiagnosticSeverity::Error, "Script.LuaMap", "LUA_MAP_ALLOCATION_FAILED", "完整地图Lua生成内存不足。", currentOrigin_});
    }
    result_.requiredApis.assign(requiredApis_.begin(), requiredApis_.end());
    result_.requiredGlobals.assign(requiredGlobals_.begin(), requiredGlobals_.end());
    result_.requiredConstants.assign(requiredConstants_.begin(), requiredConstants_.end());
    if (!result_.Succeeded()) {
        result_.text.clear();
        result_.sourceSpans.clear(); result_.objectSpans.clear();
        result_.scriptSha256.clear(); result_.sourceMapSha256.clear();
    }
    return std::move(result_);
}

} // namespace War3Map::Script::lua

namespace War3Map::Script {
bool LuaMapScriptResult::Succeeded() const noexcept {
    return !text.empty() && std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& value) {
        return value.severity == DiagnosticSeverity::Error;
    });
}
LuaMapScriptResult GenerateLuaMapScript(const MapInitializationPlan& plan, const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks) {
    return lua::MapWriter(plan, document, catalog, target, callbacks).Run();
}
} // namespace War3Map::Script
