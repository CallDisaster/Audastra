assert(_VERSION == "Lua 5.3", "expected actual Lua 5.3 runtime")
-- 显式native替身只检验生成器的执行、参数、顺序与身份；不模拟游戏效果或联机。
local names = assert(loadfile(arg[2], "t", _G))()
local calls, serial, created, emptyCase, blizzardDone, bindingsChecked = {}, 0, {}, false, false, false
local function record(name, ...) calls[#calls + 1] = {name = name, args = table.pack(...)} end
local function object(kind)
    serial = serial + 1
    return {kind = kind, serial = serial}
end
local function approx(a, b) return math.abs(a - b) < 0.0001 end
local function get(key) return _G[names[key]] end
local function count(name)
    local found = 0
    for _, call in ipairs(calls) do if call.name == name then found = found + 1 end end
    return found
end
local function first(name)
    for i, call in ipairs(calls) do if call.name == name then return i, call.args end end
    error("missing expected native: " .. name)
end
local function matched(name, predicate)
    for _, call in ipairs(calls) do if call.name == name and predicate(table.unpack(call.args, 1, call.args.n)) then return true end end
    error("missing expected arguments for " .. name)
end
for _, name in ipairs({"UNIT_STATE_LIFE", "UNIT_STATE_MANA", "CAMERA_FIELD_ZOFFSET", "CAMERA_FIELD_ROTATION",
    "CAMERA_FIELD_ANGLE_OF_ATTACK", "CAMERA_FIELD_TARGET_DISTANCE", "CAMERA_FIELD_ROLL", "CAMERA_FIELD_FIELD_OF_VIEW",
    "CAMERA_FIELD_FARZ", "CAMERA_FIELD_NEARZ", "RACE_PREF_RANDOM", "RACE_PREF_HUMAN", "RACE_PREF_ORC",
    "RACE_PREF_UNDEAD", "RACE_PREF_NIGHTELF", "MAP_CONTROL_USER", "MAP_CONTROL_COMPUTER", "MAP_CONTROL_NEUTRAL",
    "MAP_CONTROL_RESCUABLE", "MAP_LOC_PRIO_HIGH", "MAP_LOC_PRIO_LOW", "MAP_PLACEMENT_TEAMS_TOGETHER", "MAP_PLACEMENT_USE_MAP_SETTINGS"}) do
    _G[name] = {constant = name}
end
CAMERA_MARGIN_LEFT, CAMERA_MARGIN_RIGHT, CAMERA_MARGIN_BOTTOM, CAMERA_MARGIN_TOP = 0, 1, 2, 3
function FourCC(value)
    assert(type(value) == "string" and #value == 4)
    local code = 0
    for i = 1, 4 do code = code * 256 + value:byte(i) end
    return code < 2147483648 and code or code - 4294967296
end
local players = {}
function Player(number)
    assert(math.type(number) == "integer")
    if not players[number] then players[number] = {kind = "player", number = number} end
    return players[number]
end
function ConvertPlayerColor(number) return {color = number} end
function GetCameraMargin(margin) assert(margin >= 0 and margin <= 3); return 0 end
for _, name in ipairs({"SetPlayerStartLocation", "ForcePlayerStartLocation", "SetPlayerColor", "SetPlayerRacePreference",
    "SetPlayerRaceSelectable", "SetPlayerController", "SetPlayerSlotAvailable", "SetPlayerTeam", "SetPlayerAllianceStateAllyBJ",
    "SetPlayerAllianceStateVisionBJ", "SetPlayerAllianceStateControlBJ", "SetPlayerAllianceStateFullControlBJ",
    "SetStartLocPrioCount", "SetStartLocPrio", "SetPlayerTechMaxAllowed", "SetPlayerTechResearched", "SetCameraBounds",
    "SetDayNightModels", "SetTerrainFogEx", "SetWaterBaseColor", "NewSoundEnvironment", "SetAmbientDaySound",
    "SetAmbientNightSound", "SetMapMusic", "SetMapName", "SetMapDescription", "SetPlayers", "SetTeams", "SetGamePlacement",
    "DefineStartLocation", "InitGenericPlayerSlots"}) do
    _G[name] = function(...) record(name, ...) end
end
function CreateSound(path, looping, spatial, stop, fadeIn, fadeOut, eax)
    record("CreateSound", path, looping, spatial, stop, fadeIn, fadeOut, eax)
    return object("sound")
end
for _, name in ipairs({"SetSoundParamsFromLabel", "SetSoundDuration", "SetSoundChannel", "SetSoundVolume", "SetSoundPitch",
    "SetSoundDistances", "SetSoundDistanceCutoff", "SetSoundConeAngles", "SetSoundConeOrientation"}) do
    _G[name] = function(sound, ...) assert(sound.kind == "sound"); record(name, sound, ...) end
end
function Rect(left, bottom, right, top)
    record("Rect", left, bottom, right, top)
    local value = object("rect"); value.left, value.bottom, value.right, value.top = left, bottom, right, top; return value
end
function AddWeatherEffect(rect, id)
    assert(rect.kind == "rect"); record("AddWeatherEffect", rect, id); return object("weathereffect")
end
function EnableWeatherEffect(value, enabled) assert(value.kind == "weathereffect" and enabled == true); record("EnableWeatherEffect", value, enabled) end
function CreateCameraSetup() record("CreateCameraSetup"); local value = object("camerasetup"); value.fields = {}; return value end
function CameraSetupSetField(camera, field, value, seconds)
    assert(camera.kind == "camerasetup" and seconds == 0 and field.constant)
    camera.fields[field.constant] = value; record("CameraSetupSetField", camera, field, value, seconds)
end
function CameraSetupSetDestPosition(camera, x, y, seconds)
    assert(camera.kind == "camerasetup" and seconds == 0); camera.x, camera.y = x, y; record("CameraSetupSetDestPosition", camera, x, y, seconds)
end
function CreateDestructableZ(rawcode, x, y, z, facing, scale, variation)
    record("CreateDestructableZ", rawcode, x, y, z, facing, scale, variation)
    local value = object("destructable"); value.rawcode, value.z = rawcode, z; return value
end
function CreateDestructable(rawcode, x, y, facing, scale, variation)
    record("CreateDestructable", rawcode, x, y, facing, scale, variation); return object("destructable")
end
function CreateItem(rawcode, x, y)
    record("CreateItem", rawcode, x, y)
    local value = object("item"); value.rawcode, value.x, value.y = rawcode, x, y; return value
end
function CreateUnit(owner, rawcode, x, y, facing)
    assert(owner.kind == "player"); record("CreateUnit", owner, rawcode, x, y, facing)
    local value = object("unit"); value.rawcode, value.x, value.y, value.facing, value.owner = rawcode, x, y, facing, owner
    created[#created + 1] = value
    return value
end
function CreateBlightedGoldmine(owner, x, y, facing) return CreateUnit(owner, FourCC("ugol"), x, y, facing) end
function GetUnitState(unit, state) assert(unit.kind == "unit" and state == UNIT_STATE_LIFE); record("GetUnitState", unit, state); return 1000 end
function SetUnitState(unit, state, value) record("SetUnitState", unit, state, value); unit[state.constant] = value end
function SetResourceAmount(unit, amount) unit.gold = amount; record("SetResourceAmount", unit, amount) end
for _, name in ipairs({"SetHeroLevel", "SetHeroStr", "SetHeroAgi", "SetHeroInt"}) do
    _G[name] = function(unit, value, permanent) unit[name] = value; record(name, unit, value, permanent) end
end
function SetUnitColor(unit, color) unit.color = color.color; record("SetUnitColor", unit, color) end
function SetUnitAcquireRange(unit, distance) unit.acquire = distance; record("SetUnitAcquireRange", unit, distance) end
function SelectHeroSkill(unit, ability)
    unit.learned = unit.learned or {}; unit.learned[#unit.learned + 1] = ability; record("SelectHeroSkill", unit, ability)
end
function IssueImmediateOrder(unit, order) unit.order = order; record("IssueImmediateOrder", unit, order); return true end
function UnitAddItemToSlotById(unit, id, slot) unit.inventory = {id = id, slot = slot}; record("UnitAddItemToSlotById", unit, id, slot); return true end
function GetRectCenterX(rect) assert(rect.kind == "rect"); return (rect.left + rect.right) / 2 end
function GetRectCenterY(rect) assert(rect.kind == "rect"); return (rect.bottom + rect.top) / 2 end
function WaygateSetDestination(unit, x, y) unit.waygateX, unit.waygateY = x, y; record("WaygateSetDestination", unit, x, y) end
function WaygateActivate(unit, active) unit.waygateActive = active; record("WaygateActivate", unit, active) end
function InitBlizzard()
    record("InitBlizzard")
    if not emptyCase then
        assert(get("unit0").kind == "unit" and get("unit3").kind == "item" and get("dest").kind == "destructable")
        assert(get("region").kind == "rect" and get("camera").kind == "camerasetup" and get("sound").kind == "sound")
        assert(udg_Preplaced == nil, "InitGlobals ran before InitBlizzard")
    end
    blizzardDone = true
end
function CreateTrigger()
    assert(blizzardDone and udg_Preplaced == get("unit1"), "T registered before object/Blizzard/global initialization")
    record("CreateTrigger"); return object("trigger")
end
function Condition(callback) assert(type(callback) == "function"); return callback end
function TriggerAddCondition(trigger, callback) trigger.condition = callback; return object("triggercondition") end
function TriggerAddAction(trigger, callback) trigger.action = callback; return object("triggeraction") end
function ConditionalTriggerExecute(trigger)
    record("ConditionalTriggerExecute", trigger)
    if trigger.condition() then trigger.action() end
end
function TestBindings(unit, item, dest, rect, camera, sound, music, state)
    record("TestBindings", unit, item, dest, rect, camera, sound, music, state)
    assert(unit == get("unit0") and item == get("unit3") and dest == get("dest") and rect == get("region"))
    assert(camera == get("camera") and sound == get("sound") and music == "Music\\Theme.mp3" and state == UNIT_STATE_LIFE)
    assert(udg_Preplaced == get("unit1"), "typed global default did not bind already-created hero")
    bindingsChecked = true
end

assert(loadfile(arg[1], "t", _G))()
assert(#calls == 0, "loading complete script invoked natives before config/main")
config()
assert(#created == 0 and udg_Preplaced == nil)
matched("SetMapName", function(value) return value == "owned 中文地图" end)
matched("SetMapDescription", function(value) return value == "完整地图 Lua 初始化" end)
matched("SetPlayers", function(value) return value == 2 end)
matched("SetTeams", function(value) return value == 2 end)
matched("SetGamePlacement", function(value) return value == MAP_PLACEMENT_TEAMS_TOGETHER end)
matched("DefineStartLocation", function(slot, x, y) return slot == 1 and x == 100 and y == 200 end)
matched("ForcePlayerStartLocation", function(player, slot) return player.number == 0 and slot == 0 end)
matched("SetPlayerController", function(player, controller) return player.number == 2 and controller == MAP_CONTROL_COMPUTER end)
matched("SetPlayerRacePreference", function(player, race) return player.number == 2 and race == RACE_PREF_ORC end)
matched("SetPlayerRaceSelectable", function(player, enabled) return player.number == 0 and enabled == false end)
assert(count("SetPlayerTeam") == 2 and count("SetPlayerAllianceStateAllyBJ") == 2 and count("SetPlayerAllianceStateVisionBJ") == 2)
assert(count("InitGenericPlayerSlots") == 0, "custom forces must not be overwritten by generic slots")
matched("SetStartLocPrio", function(index, rank, other, priority) return index == 0 and rank == 0 and other == 1 and priority == MAP_LOC_PRIO_HIGH end)
matched("SetStartLocPrio", function(index, rank, other, priority) return index == 1 and rank == 0 and other == 0 and priority == MAP_LOC_PRIO_LOW end)
main()
assert(bindingsChecked and count("CreateTrigger") == 1 and count("TestBindings") == 1)
assert(first("CreateSound") < first("CreateCameraSetup") and first("CreateCameraSetup") < first("SetPlayerTechResearched"))
assert(first("SetPlayerTechResearched") < first("CreateDestructableZ") and first("CreateDestructableZ") < first("CreateItem"))
assert(first("CreateItem") < first("CreateUnit") and first("CreateUnit") < first("InitBlizzard") and first("InitBlizzard") < first("CreateTrigger"))
assert(first("CreateTrigger") < first("TestBindings"), "initialization T ran before registration finished")
matched("SetCameraBounds", function(a, b, c, d, e, f, g, h) return a == -384 and b == -384 and c == 384 and d == 384 and e == -384 and f == 384 and g == 384 and h == -384 end)
matched("SetTerrainFogEx", function(style, near, far, density, red, green, blue) return style == 1 and near == 100 and far == 2000 and density == 0.5 and red == 1 and approx(green, 128 / 255) and blue == 0 end)
matched("SetWaterBaseColor", function(red, green, blue, alpha) return red == 10 and green == 20 and blue == 30 and alpha == 200 end)
matched("SetDayNightModels", function(terrain, units) return terrain == "Environment\\DNC\\DNCLordaeron\\DNCLordaeronTerrain\\DNCLordaeronTerrain.mdl" and units == "Environment\\DNC\\DNCLordaeron\\DNCLordaeronUnit\\DNCLordaeronUnit.mdl" end)
assert(count("EnableWeatherEffect") == 2)
matched("AddWeatherEffect", function(rect, id) return id == FourCC("WOcw") and rect.left == -512 and rect.bottom == -512 and rect.right == 512 and rect.top == 512 end)
matched("AddWeatherEffect", function(rect, id) return id == FourCC("RAhr") and rect == get("region") end)
assert(count("SetPlayerTechResearched") == 1, "upgrade levels not collapsed to final known state")
matched("SetPlayerTechResearched", function(player, id, level) return player.number == 0 and id == FourCC("Rfoo") and level == 3 end)
matched("SetPlayerTechMaxAllowed", function(player, id, level) return player.number == 2 and id == FourCC("hbar") and level == 0 end)
assert(#created == 4 and created[1] == get("unit2") and created[2] == get("unit5") and created[3] == get("unit0") and created[4] == get("unit1"), "owner/building creation ordering changed")
assert(approx(get("unit0").facing, 270) and get("unit0").UNIT_STATE_LIFE == 500 and get("unit0").UNIT_STATE_MANA == 30)
assert(get("unit0").color == 3 and get("unit0").acquire == 200 and get("unit0").inventory.id == FourCC("ratf") and get("unit0").inventory.slot == 1)
local hero = get("unit1")
assert(hero.color == 7 and hero.SetHeroLevel == 3 and hero.SetHeroStr == 21 and hero.SetHeroAgi == 17 and hero.SetHeroInt == 35)
assert(#hero.learned == 2 and hero.learned[1] == FourCC("AHwe") and hero.learned[2] == FourCC("AHwe") and hero.order == "slowon")
assert(get("unit2").gold == 23000 and get("unit5").waygateActive and get("unit5").waygateX == 10 and get("unit5").waygateY == 10)
assert(count("CreateItem") == 2 and get("unit4").serial < get("unit3").serial and get("unit4").x == 150, "same item type reverse-file order lost")
assert(count("CreateDestructableZ") == 1 and count("CreateDestructable") == 0 and get("dest").z == 77, "archive-only DOO was recreated or explicit Z lost")
matched("CreateDestructableZ", function(id, x, y, z, facing, scale, variation) return id == FourCC("LTlt") and x == 210 and y == 220 and z == 77 and facing == 0 and scale == 1.25 and variation == 2 end)
matched("CreateSound", function(path, looping, spatial, stop, fadeIn, fadeOut, eax) return path == "Sound\\Units.wav" and looping and spatial and stop and fadeIn == 10 and fadeOut == 20 and eax == "DefaultEAXON" end)
matched("SetSoundParamsFromLabel", function(sound, label) return sound == get("sound") and label == "UnitSound" end)
matched("SetSoundDuration", function(sound, duration) return sound == get("sound") and duration == 2000 end)
matched("SetSoundPitch", function(sound, pitch) return sound == get("sound") and pitch == 1.25 end)
matched("SetSoundDistances", function(sound, near, far) return sound == get("sound") and near == 64 and far == 512 end)
matched("SetSoundConeAngles", function(sound, inside, outside, volume) return sound == get("sound") and inside == 90 and outside == 180 and volume == 32 end)
matched("SetSoundConeOrientation", function(sound, x, y, z) return sound == get("sound") and x == 1 and y == 0 and z == 0 end)
local camera = get("camera")
local expectedFields = {CAMERA_FIELD_ZOFFSET = 13, CAMERA_FIELD_ROTATION = 14, CAMERA_FIELD_ANGLE_OF_ATTACK = 15,
    CAMERA_FIELD_TARGET_DISTANCE = 16, CAMERA_FIELD_ROLL = 17, CAMERA_FIELD_FIELD_OF_VIEW = 18, CAMERA_FIELD_FARZ = 1900, CAMERA_FIELD_NEARZ = 20}
for name, value in pairs(expectedFields) do assert(camera.fields[name] == value, "camera field omitted: " .. name) end
assert(camera.x == 11 and camera.y == 12 and count("CameraSetupSetField") == 8)
print("full-map config/main, object identity, ordering, player/tech/environment and typed T execution passed")

calls, created, emptyCase, blizzardDone = {}, {}, true, false
assert(loadfile(arg[3], "t", _G))()
assert(#calls == 0)
config(); main()
assert(blizzardDone and #created == 0 and count("CreateTrigger") == 0 and count("InitGenericPlayerSlots") == 1)
print("empty complete map executed; explicit native stubs only, Warcraft Lua/game/multiplayer not validated")
