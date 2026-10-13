-- 模板 slk 库：原创实现，按 YDWE/KKWE 模板 require 'slk' 的公开语义工作。
-- 数据由宿主一次性传入（w3x2lni prebuilt 默认值、metadata、本次保存的最终地图物编），本文件不做任何 I/O。
-- 读取：slk.<类>[ID].<字段>，字段名不分大小写，等级字段用 字段名+等级，列表字段用 _序号 或 :序号。
-- 生成：slk.<类>[父ID]:new(ID或标签)，只有本次生成的对象可写；结束时由宿主按 finish() 结果改写物编。
-- 上次生成的对象带 W2lo 标记；本次未再生成的被移除，内容相同的不改动文件。
local data = ...

local type, pairs, ipairs, next, tostring, tonumber, error = type, pairs, ipairs, next, tostring, tonumber, error
local setmetatable, select = setmetatable, select
local floor, mathtype = math.floor, math.type
local sort, concat = table.sort, table.concat

local classes = {'unit', 'ability', 'item', 'buff', 'upgrade', 'doodad', 'destructable'}
local refreshOrder = {'ability', 'buff', 'unit', 'item', 'upgrade', 'doodad', 'destructable'}
local levelKey = {ability = 'levels', doodad = 'numvar', upgrade = 'maxlevel'}
local displayType = {unit = '单位', ability = '技能', item = '物品', buff = '魔法效果',
    upgrade = '科技', doodad = '装饰物', destructable = '可破坏物'}

local meta, codemeta, defaults, nativetype = data.meta, data.codemeta, data.default, data.nativetype
local slkOverride, slkIds, txtOverride = data.slkoverride, data.slkids, data.txtoverride

local errors, errorOrder = {}, {}
local function report(kind, text)
    if not errors[kind] then
        errors[kind] = {}
        errorOrder[#errorOrder + 1] = kind
    end
    local list = errors[kind]
    list[#list + 1] = text
end

local function copy(value)
    if type(value) ~= 'table' then return value end
    local result = {}
    for k, v in pairs(value) do result[k] = copy(v) end
    return result
end

local function round(value)
    if mathtype(value) ~= 'float' then return value end
    return floor(value * 10000 + 0.5) / 10000
end

local function emptyOf(list)
    local first = list[1]
    if type(first) == 'number' then
        return mathtype(first) == 'integer' and 0 or 0.0
    elseif type(first) == 'string' then
        return ''
    end
    return nil
end

local function getMeta(m1, m2, key)
    return (m1 and m1[key]) or (m2 and m2[key])
end

-- 字段名 -> metadata、等级、列表形式（'index' 或 'level'）。
local function findMeta(key, m1, m2)
    key = key:lower()
    if key == 'w2lobject' then return nil end
    local base, level = key:match '^(%a+)(%d*)'
    if not base then return nil end
    if getMeta(m1, m2, base .. '_1') then
        if key == base then return getMeta(m1, m2, base .. '_1'), nil, 'index' end
        local index = 0
        while true do
            index = index + 1
            local k = base .. '_' .. index
            local m = getMeta(m1, m2, k)
            if key == k or key == base .. ':' .. index then return m end
            if not m then break end
        end
    end
    local m = getMeta(m1, m2, base)
    if m then
        if m['repeat'] then
            if key == base then return m, 1, 'level' end
            return m, tonumber(level)
        elseif key == base then
            return m
        end
    end
    return getMeta(m1, m2, key)
end

-- 读取对象的值；第二个返回值是等级，第三个为 true 表示没有该属性。
local function readValue(object, key, m1, m2)
    key = key:lower()
    if key == 'code' then return object._code end
    if key == 'w2lobject' then return nil end
    local base, level = key:match '^(%a+)(%d*)'
    if not base then return nil end
    if object[base .. '_1'] ~= nil then
        local values, index = {}, 0
        while true do
            index = index + 1
            local k = base .. '_' .. index
            local v = object[k]
            if key == k or key == base .. ':' .. index then return v end
            if v == nil then break end
            values[index] = v
        end
        if key == base then return concat(values, ',') end
    end
    local value = object[base]
    if value ~= nil then
        if type(value) == 'table' then
            if key == base then return value, 1 end
            return value, tonumber(level)
        elseif key == base then
            return value
        end
    end
    if object[key] == nil and not findMeta(key, m1, m2) then return nil, nil, true end
    return object[key]
end

-- 按最大等级补齐/截断等级数组：profile 字段沿用前一级，其余取默认值的最后一级。
local function fillList(list, maxLevel, m, default)
    if not m['repeat'] or type(list) ~= 'table' then return end
    default = type(default) == 'table' and default or {}
    for i = #list + 1, maxLevel do
        if m.profile then
            list[i] = default[i] or list[i - 1]
        else
            list[i] = default[i] or default[#default]
        end
    end
    for i = #list, maxLevel + 1, -1 do list[i] = nil end
end

local function fillObject(object, class)
    local default = defaults[class][object._parent] or defaults[class][object._id]
    if not default then return end
    local maxLevel = object._max_level or 1
    for key, m in pairs(meta[class]) do fillList(object[key], maxLevel, m, default[key]) end
    local scope = object._code and codemeta[object._code]
    if scope then
        for key, m in pairs(scope) do fillList(object[key], maxLevel, m, default[key]) end
    end
end

-- 原生字段 ID -> 字段名；技能先查自身代码的专有字段。
local rawIndex, rawCodeIndex = {}, {}
local function indexScope(scope)
    local index = {}
    for key, m in pairs(scope) do
        if m.id then index[m.id] = key end
    end
    return index
end
for _, class in ipairs(classes) do
    rawIndex[class] = indexScope(meta[class])
    rawIndex[class].W2lo = rawIndex[class].W2lo or 'w2lobject'
end

local function fieldOf(class, code, rawId)
    if code and codemeta[code] then
        local scoped = rawCodeIndex[code]
        if not scoped then
            scoped = indexScope(codemeta[code])
            rawCodeIndex[code] = scoped
        end
        local key = scoped[rawId]
        if key then return key, codemeta[code][key] end
    end
    local key = rawIndex[class][rawId]
    if key then return key, meta[class][key] or (key == 'w2lobject' and {type = 3}) end
end

-- 把一条原生物编记录合成为完整对象（父默认值 + 覆盖）。
local function materialize(class, record)
    local parent = record.parent or record.id
    local base = defaults[class][parent]
    if not base then
        report('继承对象不存在', ("[%s] '%s' 的父对象 '%s'"):format(displayType[class], record.id, parent))
    end
    local object = copy(base or {})
    object._id, object._type, object._obj = record.id, class, true
    if record.parent then object._parent = record.parent end
    local levelField = levelKey[class]
    local maxLevel = object._max_level or 1
    for _, mod in ipairs(record.mods) do
        local key = fieldOf(class, object._code, mod.id)
        if key and key == levelField then maxLevel = tonumber(mod.value) or maxLevel end
    end
    object._max_level = maxLevel
    fillObject(object, class)
    for _, mod in ipairs(record.mods) do
        local key, m = fieldOf(class, object._code, mod.id)
        if not key then
            report('无效的数据', ("[%s] '%s' 的字段 %s 没有对应定义"):format(displayType[class], record.id, mod.id))
        elseif m['repeat'] then
            if class == 'doodad' then
                error(("装饰物 '%s' 的多变体字段 %s 暂不支持模板读取"):format(record.id, mod.id), 0)
            end
            local level = mod.level > 0 and mod.level or 1
            if level <= maxLevel then
                local list = object[key]
                if type(list) ~= 'table' then
                    list = {}
                    object[key] = list
                end
                list[level] = mod.value
            end
        else
            object[key] = mod.value
        end
    end
    return object
end

-- 会话状态
local slk, built = {}, {}
local all, dynamics, old, oldIds, new, change, counters = {}, {}, {}, {}, {}, {}, {}
for _, class in ipairs(classes) do
    dynamics[class], old[class], oldIds[class], new[class] = {}, {}, {}, {}
    for id in pairs(defaults[class]) do all[id:lower()] = true end
    for id in pairs(slkIds[class]) do all[id:lower()] = true end
    for _, record in ipairs(data.native[class]) do
        if record.parent and record.w2l then
            -- 上次模板生成的对象：从当前对象集中移出，等待本次重新生成或移除。
            old[class][record.id] = record
            oldIds[class][#oldIds[class] + 1] = record.id
            local kind = record.w2l:match '^(%a+)|'
            if kind == 'dynamic' then
                dynamics[class][record.w2l] = record.id
                dynamics[class][record.id] = record.w2l
            end
        else
            all[record.id:lower()] = true
        end
    end
end

local function build(class)
    local objects = slk[class]
    if objects then return objects end
    if data.unsupported then error(data.unsupported, 0) end
    if data.broken[class] then error(data.broken[class], 0) end
    objects = {}
    if class == 'misc' then
        for id, object in pairs(defaults.misc) do objects[id] = object end
        for section, fields in pairs(data.miscmap or {}) do
            local object = copy(objects[section] or {_id = section, _type = 'misc'})
            for key, text in pairs(fields) do
                local m = meta.misc[key]
                local value = text
                if m and m.type == 0 then
                    local n = tonumber(text)
                    value = n and math.tointeger(floor(n)) or text
                elseif m and (m.type == 1 or m.type == 2) then
                    value = tonumber(text) or text
                end
                object[key] = value
            end
            objects[section] = object
        end
    else
        for id, object in pairs(defaults[class]) do objects[id] = object end
        for _, record in ipairs(data.native[class]) do
            if not (record.parent and record.w2l) then objects[record.id] = materialize(class, record) end
        end
        for id in pairs(slkIds[class]) do
            if not objects[id] then objects[id] = {_id = id, _type = class} end
        end
    end
    slk[class] = objects
    built[#built + 1] = class
    return objects
end

local chars = {}
for c in ('0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'):gmatch '.' do chars[#chars + 1] = c end

local function findId(class, source, tag)
    local known = dynamics[class]
    local id = known[tag]
    if id then
        local object = all[id:lower()]
        if not object then return id end
        if object ~= true and object._create then return nil end
        known[tag], known[id] = nil, nil
    end
    local first
    if class == 'unit' or class == 'ability' or class == 'buff' then first = source:sub(1, 1)
    elseif class == 'item' then first = 'I'
    elseif class == 'destructable' then first = 'B'
    elseif class == 'doodad' then first = 'D'
    elseif class == 'upgrade' then first = 'R' end
    local digits = counters[first]
    if not digits then
        digits = {1, 1, 1}
        counters[first] = digits
    end
    while true do
        local candidate = first .. chars[digits[3]] .. chars[digits[2]] .. chars[digits[1]]
        if not all[candidate:lower()] and not known[candidate] then return candidate end
        for x = 1, 3 do
            digits[x] = digits[x] + 1
            if chars[digits[x]] then break end
            digits[x] = 1
            if x == 3 then return nil end
        end
    end
end

local proxies = {}
local createObject

local function packId(value)
    local ok, packed = pcall(string.pack, '>I4', value)
    if ok then return packed end
    report('不合法的ID', ('[%s]'):format(tostring(value)))
    return tostring(value)
end

-- 字段由地图自带 SLK/TXT 定义的现有对象：不拿默认值冒充，读取时明确报错。
local function requireReadable(object, class)
    if object._create then return end
    local source = txtOverride[object._id] or slkOverride[class]
    if source then
        error(("[%s] 的数据由地图内的 %s 定义，模板 slk 库暂不支持读取这类来源的字段"):format(object._id, source), 3)
    end
end

createObject = function(object, class, name)
    local scopeMeta = meta[class]
    local proxy = {}
    local mt = {}
    function mt:__index(key)
        if not object or type(key) ~= 'string' then return '' end
        requireReadable(object, class)
        local value, level, missing = readValue(object, key, scopeMeta, object._code and codemeta[object._code])
        if missing then report('读取数据错误', ('对象[%s]没有[%s]属性'):format(object._id, key)) end
        if not value then return '' end
        if not level then return round(value) end
        if level > (object._max_level or 1) then return emptyOf(value) or '' end
        local item = value[level]
        if item == nil then return '' end
        return round(item)
    end
    function mt:__newindex(key, value)
        if type(key) ~= 'string' then return end
        if not object then
            -- 键不是对象 ID（例如把 new 返回的对象本身当键）时不显示内存地址。
            local shown = type(name) == 'string' and name or ('非 ID 的' .. type(name) .. '键')
            report('写入被忽略', ("对象[%s]不存在，对 %s 的写入没有效果"):format(shown, key))
            return
        end
        if not object.w2lobject then
            report('写入被忽略', ("[%s]不是本次模板新建的对象，对 %s 的写入没有效果"):format(object._id or name, key))
            return
        end
        local parent = defaults[class][object._parent]
        if not parent then
            object[key] = value
            return
        end
        local m, level, listType = findMeta(key, scopeMeta, parent._code and codemeta[parent._code])
        if not m then
            report('不是物编字段', ("[%s].%s 只在模板内保存，不会写入地图"):format(object._id, key))
            object[key] = value
            return
        end
        local function write(item, at)
            local field = m.field:lower()
            if type(item) == 'string' and #item > 1023 then
                item = item:sub(1, 1023)
                report('字符串太长（不能超过1023个字符）', item:sub(1, 20))
            end
            if at then
                if type(object[field]) ~= 'table' then object[field] = {} end
                object[field][at] = item
            else
                object[field] = item
                if field == levelKey[class] then
                    object._max_level = tonumber(item) or object._max_level
                    fillObject(object, class)
                end
            end
        end
        if listType == 'index' then
            if type(value) ~= 'table' then
                self[key .. '_1'] = value
                return
            end
            for k, v in pairs(value) do self[key .. '_' .. k] = v end
            return
        end
        if listType == 'level' then
            if type(value) ~= 'table' then
                write(value, level)
                return
            end
            local maxLevel = object._max_level or 1
            if #value == maxLevel then
                for i = 1, maxLevel do write(value[i], i) end
                return
            end
            if maxLevel <= 1 then return end
            local low, high = value[1], value[#value]
            local step = (high - low) / (maxLevel - 1)
            for i = 1, maxLevel do write(low + step * (i - 1), i) end
            return
        end
        if type(value) == 'table' then return end
        write(value, level)
    end
    function mt:__pairs()
        if not object then return function() end end
        requireReadable(object, class)
        local codeScope = object._code and codemeta[object._code]
        local keys, values = {}, {}
        for k, v in pairs(object) do
            if k == '_code' then
                keys[#keys + 1] = 'code'
                values.code = v
            else
                local m = findMeta(k, scopeMeta, codeScope)
                if m and m.field then
                    if type(v) ~= 'table' then
                        keys[#keys + 1] = m.field
                        values[m.field] = v
                    else
                        for level = 1, object._max_level or 1 do
                            local lk = m.field .. level
                            keys[#keys + 1] = lk
                            values[lk] = v[level]
                        end
                    end
                end
            end
        end
        sort(keys)
        local i = 0
        return function()
            i = i + 1
            local k = keys[i]
            return k, values[k]
        end
    end
    function mt:__call(fields)
        if not object or not object.w2lobject or type(fields) ~= 'table' then return self end
        local key = levelKey[class]
        if key and fields[key] then
            self[key] = fields[key]
            fields[key] = nil
        end
        for k, v in pairs(fields) do self[k] = v end
        return self
    end
    function proxy:get_id() return name end
    function proxy:new(id)
        if class == 'misc' then error('slk.misc 不能新建对象', 2) end
        local parentDefault = defaults[class][name] or {}
        local marker
        if type(id) == 'number' then id = packId(id) end
        if type(id) == 'string' and #id == 4 and not id:find('%W') then
            marker = 'static'
            if defaults[class][id] or build(class)[id] then
                report('新建对象的ID重复', ('[%s]'):format(id))
                return proxies[class][id]
            end
        else
            marker = 'dynamic|' .. tostring(id)
            local found = findId(class, name, marker)
            if not found then
                report('无法找到可用ID', ('[%s]'):format(tostring(id)))
                return createObject(nil, class, '')
            end
            id = found
            dynamics[class][marker] = id
        end
        local created = copy(parentDefault)
        fillObject(created, class)
        created._id, created._parent, created._type, created._obj = id, name, class, true
        created._code, created._create, created.w2lobject = parentDefault._code, true, marker
        build(class)[id] = created
        all[id:lower()] = created
        if defaults[class][name] then new[class][id] = created end
        return createObject(created, class, id)
    end
    function proxy:permanent()
        if object then object.w2lobject = nil end
        return self
    end
    return setmetatable(proxy, mt)
end

local function classProxy(class)
    local mt = {}
    function mt:__index(key)
        if type(key) == 'number' then key = packId(key) end
        return createObject(build(class)[key], class, key)
    end
    function mt:__newindex() end
    function mt:__pairs()
        local keys = {}
        for k in pairs(build(class)) do keys[#keys + 1] = k end
        sort(keys)
        local i = 0
        return function()
            i = i + 1
            local k = keys[i]
            return k, self[k]
        end
    end
    return setmetatable({}, mt)
end

for _, class in ipairs(classes) do proxies[class] = classProxy(class) end
proxies.misc = classProxy('misc')

-- 写出比较：数值按四位小数，nil 与 0/'' 视为相同。
local function same(a, b)
    if a == nil or b == nil then
        local other = a == nil and b or a
        return other == nil or other == 0 or other == ''
    end
    if type(a) == 'number' and type(b) == 'number' then return round(a) == round(b) end
    return a == b
end

local function equalObject(a, b)
    for k, v in pairs(a) do
        if k ~= '_create' then
            local w = b[k]
            if type(v) == 'table' then
                if type(w) ~= 'table' or not equalObject(v, w) then return false end
            elseif not same(v, w) then
                return false
            end
        end
    end
    return true
end

local function convert(m, value, object, field)
    local kind = m.type
    if kind == 0 then
        local n = tonumber(value)
        if not n or n ~= n or n == math.huge or n == -math.huge then return nil end
        n = floor(n)
        if n < -2147483648 or n > 2147483647 then return nil end
        return mathtype(n) == 'integer' and n or math.tointeger(n)
    elseif kind == 1 or kind == 2 then
        local n = tonumber(value)
        if not n or n ~= n or n == math.huge or n == -math.huge then return nil end
        return n + 0.0
    end
    return tostring(value)
end

local function modsOf(class, object)
    local parent = defaults[class][object._parent] or {}
    local maxLevel = object._max_level or 1
    local codeScope = object._code and codemeta[object._code]
    local keys = {}
    for k in pairs(object) do
        if type(k) == 'string' and k:sub(1, 1) ~= '_' then keys[#keys + 1] = k end
    end
    sort(keys)
    local mods = {}
    -- 存储类型以游戏元数据为准（专有字段先查技能代码）；canonical 类型只在游戏表没有该字段时使用。
    local codeTypes = object._code and nativetype[object._code]
    local classTypes = nativetype[class]
    local function add(m, key, level, value)
        local kind = (codeTypes and codeTypes[key]) or (classTypes and classTypes[key]) or m.type
        local converted = convert({type = kind}, value)
        if converted == nil then
            error(("[%s] '%s'.%s 的值 %s 不能写成%s"):format(displayType[class], object._id, m.field or key,
                tostring(value), kind == 0 and '整数' or kind == 3 and '字符串' or '实数'), 0)
        end
        mods[#mods + 1] = {id = m.id, level = level, data = m.data or 0, type = kind, value = converted}
    end
    for _, key in ipairs(keys) do
        local m = (codeScope and codeScope[key]) or meta[class][key]
        if key == 'w2lobject' then m = m or {id = 'W2lo', type = 3, field = 'W2LObject'} end
        if m and m.id then
            local value = object[key]
            if m['repeat'] then
                if not levelKey[class] or class == 'doodad' then
                    error(("[%s] '%s' 的等级字段 %s 暂不支持生成"):format(displayType[class], object._id, m.field or key), 0)
                end
                if type(value) == 'table' then
                    local default = copy(parent[key])
                    if type(default) ~= 'table' then default = {} end
                    fillList(default, maxLevel, m, parent[key])
                    for level = 1, maxLevel do
                        if value[level] ~= nil and not same(value[level], default[level]) then add(m, key, level, value[level]) end
                    end
                end
            elseif type(value) ~= 'table' and not same(value, parent[key]) then
                add(m, key, 0, value)
            end
        end
    end
    return mods
end

local function displayName(object)
    local parent = defaults[object._type] and defaults[object._type][object._parent] or {}
    local name = object.name or object.bufftip or object.editorname or parent.name or parent.bufftip or parent.editorname
    if type(name) == 'table' then name = name[1] end
    if name == nil then name = '<未知>' end
    return (tostring(name):sub(1, 100):gsub('\r\n', ' '))
end

local function listOf(groups)
    local list = {}
    for _, group in pairs(groups) do
        for _, object in pairs(group) do list[#list + 1] = object end
    end
    sort(list, function(a, b) return a._id < b._id end)
    return list
end

local function finish()
    local write = {}
    for _, class in ipairs(refreshOrder) do
        change[class] = {}
        local previous = old[class]
        for id, object in pairs(new[class]) do
            local record = previous[id]
            if record then
                if not equalObject(object, materialize(class, record)) then change[class][id] = object end
                previous[id], new[class][id] = nil, nil
            end
        end
        if next(change[class]) or next(new[class]) or next(previous) then
            local add = {}
            local objects = slk[class]
            if objects then
                local ids = {}
                for id, object in pairs(objects) do
                    if type(object) == 'table' and object._create then ids[#ids + 1] = id end
                end
                sort(ids)
                for _, id in ipairs(ids) do
                    local object = objects[id]
                    add[#add + 1] = {id = id, parent = object._parent, mods = modsOf(class, object)}
                end
            end
            write[class] = {remove = oldIds[class], add = add}
        end
    end
    local lines = {}
    for _, kind in ipairs(errorOrder) do
        if #lines > 0 then lines[#lines + 1] = '' end
        lines[#lines + 1] = kind
        for _, text in ipairs(errors[kind]) do lines[#lines + 1] = text end
    end
    local function section(title, objects)
        if #objects == 0 then return end
        if #lines > 0 then lines[#lines + 1] = '' end
        lines[#lines + 1] = title:format(#objects)
        for i = 1, math.min(10, #objects) do
            local object = objects[i]
            lines[#lines + 1] = ("[%s][%s] '%s'"):format(displayType[object._type] or object._type, displayName(object), object._id)
        end
    end
    local removed = {}
    for _, class in ipairs(classes) do
        removed[class] = {}
        for id, record in pairs(old[class]) do
            removed[class][id] = {_id = id, _type = class, _parent = record.parent}
        end
    end
    section('移除了 %d 个对象', listOf(removed))
    section('新建了 %d 个对象', listOf(new))
    section('修改了 %d 个对象', listOf(change))
    -- 每类问题汇总成一条：数量 + 前几个例子，完整列表在模板阶段日志里。
    local warnings = {}
    for _, kind in ipairs(errorOrder) do
        local list = errors[kind]
        local examples = {}
        for i = 1, math.min(5, #list) do examples[i] = list[i] end
        local more = #list > 5 and ('…（共 %d 条）'):format(#list) or ''
        warnings[#warnings + 1] = kind .. '：' .. concat(examples, '；') .. more
    end
    return {write = write, report = concat(lines, '\n'), warnings = warnings}
end

return proxies, finish
