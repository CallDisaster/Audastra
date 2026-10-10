-- 生产 wholechunk 模板事务驱动（原创）。编辑器把本文件与 LuaTemplateHost.lua 写入本次保存的独立工作目录，
-- 以 YDWE 自带 lua.exe -E 运行：lua.exe -E LuaTemplateRunner.lua <request.bin> <result.bin>。
-- 只读取请求里冻结的地图成员与 YDWE 安装内固定目录；唯一写入是 result（先写 .tmp 再改名）。
-- 可信工具（w3x2lni core、template.lua、slk_lib.lua）按 YDWE 保存时的方式构造；模板本身只在宿主受限环境内执行。
local requestPath, resultPath = assert(arg[1], 'request path'), assert(arg[2], 'result path')
local here = assert(arg[0]):match('^(.*)[\\/][^\\/]*$') or '.'
package.path, package.cpath = '', ''

local function canonical(path) return (path:gsub('/', '\\'):gsub('\\+', '\\'):lower()) end
local function readFile(path)
    local f = io.open(path, 'rb'); if not f then return nil end
    local bytes = f:read('a'); f:close(); return bytes
end

-- ---- 请求：FORGE-LTQ2 | s2 ydweRoot | s2 dataId | s2 locale | s4 preprocessed |
--      u1 名字清单完整性 | I4 n × (s2 name, u1 presence, s4 bytes) 全部固定槽位 |
--      I4 m × s2 已知最终成员名 | END!。Missing 必须是查证过的固定槽位。
local request = assert(readFile(requestPath), 'request unreadable')
local position = 1
local function take(format)
    local values = table.pack(string.unpack(format, request, position))
    position = values[values.n]
    return table.unpack(values, 1, values.n - 1)
end
assert(take('c10') == 'FORGE-LTQ2', 'request magic')
local ydweRoot, dataId, locale = take('<s2'), take('<s2'), take('<s2')
local preprocessed = take('<s4')
local complete = take('B'); assert(complete == 0 or complete == 1, 'inventory completeness')
local members, domain, finalNames = {}, {}, {}
local slotCount = take('<I4'); assert(slotCount <= 256, 'captured domain budget')
local retained = 0
for _ = 1, slotCount do
    local name = canonical(take('<s2'))
    assert(#name > 0 and #name <= 1024 and domain[name] == nil, 'duplicate or invalid captured member')
    local present, bytes = take('B'), take('<s4')
    assert(present == 0 or present == 1, 'captured presence')
    assert(present == 1 or #bytes == 0, 'missing member has bytes')
    retained = retained + #bytes; assert(retained <= 64 * 1024 * 1024, 'captured byte budget')
    domain[name] = present == 1
    if present == 1 then members[name] = bytes end
end
local knownCount = take('<I4'); assert(knownCount <= 65536, 'known inventory budget')
for _ = 1, knownCount do
    local name = canonical(take('<s2'))
    assert(#name > 0 and #name <= 1024 and not finalNames[name], 'duplicate or invalid known member')
    finalNames[name] = true
end
for name, present in pairs(domain) do
    assert(present == (finalNames[name] == true), 'captured and known inventory disagree')
end
assert(take('c4') == 'END!' and position == #request + 1, 'request trailer')
request = nil

local coreRoot = ydweRoot .. '\\plugin\\w3x2lni\\script\\core'
local corePrefix, installedPrefix = canonical(coreRoot .. '\\'), canonical(ydweRoot .. '\\')

-- ---- 结果：FORGE-LTR1 | u1 ok | ok: s4 script, I4 n × (s2 name, s4 bytes) | 失败: s2 code, s4 message |
--      s4 工具消息 | END!。只有完整写完才改名为 result。
local messages = {}
local function message(...)
    if #messages >= 256 then return end
    local args, parts = table.pack(...), {}
    for i = 1, args.n do parts[#parts + 1] = tostring(args[i]):sub(1, 1024) end
    messages[#messages + 1] = table.concat(parts, ' | ')
end
local function writeResult(body)
    local text = 'FORGE-LTR1' .. body .. string.pack('<s4', table.concat(messages, '\n')) .. 'END!'
    local temporary = resultPath .. '.tmp'
    local f = assert(io.open(temporary, 'wb')); assert(f:write(text)); assert(f:close())
    assert(os.rename(temporary, resultPath))
end

-- 只有固定域内的显式 Missing 返回 nil；域外即使未列名也不能猜成 Missing。
local function mapMember(name)
    local key = canonical(name)
    if domain[key] == nil then error('map source outside captured domain: ' .. name, 0) end
    return members[key]
end

-- 可信工具读取：只允许 core 与 YDWE 安装目录内的只读访问。
local readBytes = 0
local function installedRead(path, allowCore)
    local key = canonical(path)
    if key:find('\\..\\', 1, true) then error('tool path escapes root: ' .. path, 0) end
    if not (key:sub(1, #installedPrefix) == installedPrefix or (allowCore and key:sub(1, #corePrefix) == corePrefix)) then
        error('tool path outside YDWE: ' .. path, 0)
    end
    local bytes = readFile(path)
    if bytes then
        readBytes = readBytes + #bytes
        if readBytes > 512 * 1024 * 1024 then error('tool read budget exceeded', 0) end
    end
    return bytes
end
local function openCore(path, mode)
    if not (mode == nil or mode == 'r' or mode == 'rb') then error('tool write denied: ' .. path, 0) end
    local bytes = installedRead(path, true)
    if bytes == nil then return nil end
    return {read = function(_, format) assert(format == 'a' or format == '*a', 'unexpected read'); return bytes end,
        close = function() end}
end

local function upvalue(fn, name)
    for i = 1, 64 do
        local key, value = debug.getupvalue(fn, i)
        if not key then break end
        if key == name then return value end
    end
    error('fixed target upvalue missing: ' .. name, 0)
end

-- Storm 字符串哈希（与 C++ StormStringHash 同一算法：Jenkins lookup2，a–z 转大写、/ 转 \）。
local M = 0xFFFFFFFF
local function stormStringHash(text)
    local function at(i)
        local c = text:byte(i)
        if c >= 97 and c <= 122 then c = c - 32 elseif c == 47 then c = 92 end
        return c
    end
    local function word(i) return at(i) | at(i + 1) << 8 | at(i + 2) << 16 | at(i + 3) << 24 end
    local function mix(a, b, c)
        a = (a - b - c) & M; a = a ~ (c >> 13)
        b = (b - c - a) & M; b = (b ~ (a << 8)) & M
        c = (c - a - b) & M; c = c ~ (b >> 13)
        a = (a - b - c) & M; a = a ~ (c >> 12)
        b = (b - c - a) & M; b = (b ~ (a << 16)) & M
        c = (c - a - b) & M; c = c ~ (b >> 5)
        a = (a - b - c) & M; a = a ~ (c >> 3)
        b = (b - c - a) & M; b = (b ~ (a << 10)) & M
        c = (c - a - b) & M; c = c ~ (b >> 15)
        return a, b, c
    end
    local a, b, c = 0x9e3779b9, 0x9e3779b9, 0
    local index, remaining = 1, #text
    while remaining >= 12 do
        a = (a + word(index)) & M; b = (b + word(index + 4)) & M; c = (c + word(index + 8)) & M
        a, b, c = mix(a, b, c)
        index, remaining = index + 12, remaining - 12
    end
    c = (c + #text) & M
    for i = 0, remaining - 1 do
        local value = at(index + i)
        if i < 4 then a = (a + (value << (8 * i))) & M
        elseif i < 8 then b = (b + (value << (8 * (i - 4)))) & M
        else c = (c + (value << (8 * (i - 7)))) & M end
    end
    local _, _, hash = mix(a, b, c)
    return hash
end

local function loadData(relative)
    local path = ydweRoot .. '\\compiler\\script\\computed\\' .. relative
    local source = assert(installedRead(path, false), 'missing ' .. relative)
    return assert(load(source, '@' .. relative, 't', {}))()
end

local function run()
    local modules = {}
    for _, name in ipairs {'lpeg', 'lni', 'w3xparser', 'lml'} do
        local loader, err = package.loadlib(ydweRoot .. '\\bin\\modules\\' .. name .. '.dll', 'luaopen_' .. name)
        assert(loader, err); modules[name] = loader(); package.loaded[name] = modules[name]
    end
    assert(string.packsize('l') == 4 and type(modules.lni.classics) == 'function', 'target Lua ABI mismatch')
    local hostSource = assert(readFile(here .. '\\LuaTemplateHost.lua'), 'host missing')
    local host = assert(load(hostSource, '@LuaTemplateHost.lua', 't', _G))()
    assert(host.schema == 'forge-lua-template-host-v1', 'host protocol mismatch')
    local sandboxPath = ydweRoot .. '\\plugin\\w3x2lni\\script\\backend\\sandbox.lua'
    local templatePath = ydweRoot .. '\\compiler\\script\\template.lua'
    local slkLibraryPath = ydweRoot .. '\\compiler\\script\\w3x2lni\\slk_lib.lua'
    local orderList, orderId = loadData('order_list.lua'), loadData('order_id.lua')
    local logger = setmetatable({}, {__index = function(_, name) return function(...) message(name, ...) end end})

    local trusted = {compileChunkProtocol = 'forge-guarded-template-load-v1'}
    trusted.templateSource = assert(installedRead(templatePath, false), 'template.lua missing')
    trusted.inputMemberExists = function(name) return mapMember(name) ~= nil end
    trusted.createSession = function(output)
        local loaded = {}
        for name, module in pairs(modules) do loaded[name] = module end
        -- 与 YDWE compiler/w3x2lni/data_load 相同的查找位置。
        loaded.data_load = function(w2l, name)
            if w2l.setting.data ~= dataId then error('unexpected dataset: ' .. tostring(w2l.setting.data), 0) end
            name = name:gsub('/', '\\')
            if name:sub(1, 3) == 'ui\\' then return installedRead(ydweRoot .. '\\' .. name, false) end
            if name:sub(1, 9) == 'prebuilt\\' then
                return installedRead(ydweRoot .. '\\plugin\\w3x2lni\\data\\' .. dataId .. '\\' .. name, false)
            end
            return installedRead(ydweRoot .. '\\share\\' .. locale .. '\\' .. name, false)
        end
        local sandbox = assert(load(assert(installedRead(sandboxPath, false)), '@sandbox.lua', 't', _G))()
        local factory = assert(sandbox(coreRoot .. '\\', openCore, loaded))
        -- 与 YDWE 保存时相同：默认设置，frontend 按 w3i 决定 Custom/Melee；slk_lib 自行打开 read_slk。
        local w2l = factory()
        w2l:set_messager(message)
        w2l.input_ar = {get = function(_, name) return mapMember(name) end}
        w2l.output_ar = {set = function(_, name, bytes) output('set', name, bytes) end,
            remove = function(_, name) output('remove', name) end}
        local toolEnv = setmetatable({log = logger}, {__index = _G})
        local construct = assert(load(assert(installedRead(slkLibraryPath, false)), '@slk_lib.lua', 't', toolEnv))()
        local rawSlk = construct(w2l, false, true)
        local realRefresh = assert(rawget(rawSlk, 'refresh'))
        local state = upvalue(realRefresh, 'session')
        assert(type(state) == 'table' and type(state.default) == 'table' and type(state.metadata) == 'table',
            'unexpected target state shape')
        local tryMeta = upvalue(assert(debug.getmetatable(rawSlk.ability.AHhb).__newindex), 'try_meta')
        assert(type(tryMeta) == 'function', 'target try_meta missing')
        return {
            rawSlk = rawSlk,
            refresh = function() return realRefresh(rawSlk) end,
            errors = function() return upvalue(construct, 'errors') end,
            hasStandard = function(class, id) return state.default[class] and state.default[class][id] ~= nil end,
            validateWrite = function(class, parent, key)
                local base = state.default[class] and state.default[class][parent]
                if not base then return nil end
                local meta, level = tryMeta(key, state.metadata[class], base._code and state.metadata[base._code])
                if not meta then return nil end
                return meta.field:lower(), level
            end,
        }
    end
    trusted.StringHash = function(_, text)
        if type(text) ~= 'string' then error('StringHash expects a string', 0) end
        return ('0x%08X'):format(stormStringHash(text))
    end
    -- 与 YDWE abilityOrder 行为一致：'xxxx' 形式的技能在同一 SLK 会话中查找，ANcl 取 DataF1 命令串。
    trusted.abilityOrder = function(session, skill, kind)
        if type(skill) ~= 'string' then return 0 end
        if not (#skill >= 2 and skill:sub(1, 1) == "'" and skill:sub(-1) == "'") then return 0 end
        local ability = session.rawSlk.ability[skill:sub(2, -2)]
        if not ability then return 0 end
        local code = ability.code
        if code == 'ANcl' then
            local order = ability.DataF1
            if not order then return 0 end
            return orderId[order] or 0
        end
        local list = orderList[kind]
        if not list then return 0 end
        return list[code] or 0
    end
    trusted.compileWhole = function(text, environment, compileChunk)
        local written, detail
        local templateEnv = {load = compileChunk, string = string, table = table, pcall = pcall, xpcall = xpcall,
            tostring = tostring, debug = {traceback = debug.traceback}, log = logger,
            LNG = {UNKNOWN = 'unknown template error'},
            gui = {error_message = function(_, value) detail = tostring(value) end},
            require = function(name)
                assert(name == 'compiler.computed.env', 'unexpected target template dependency')
                return environment
            end,
            io = {load = function(path) assert(path == 'forge-input', 'unexpected template input'); return text end,
                save = function(path, bytes) assert(path == 'forge-output', 'unexpected template output'); written = bytes; return true end}}
        local template = assert(load(trusted.templateSource, '@template.lua', 't', templateEnv))()
        local success = template:compile {input = 'forge-input', output = 'forge-output', map_path = 'forge-readonly-map'}
        return success, written, detail
    end

    local result = host.Run({schema = 'forge-lua-template-request-v1', preprocessed = preprocessed}, trusted)
    if not result.ok then
        return string.pack('<Bs2s4', 0, tostring(result.errorCode), tostring(result.error))
    end
    local names = {}
    for name in pairs(result.nativeMembers) do names[#names + 1] = name end
    table.sort(names)
    local parts = {string.pack('<Bs4I4', 1, result.script, #names)}
    for _, name in ipairs(names) do parts[#parts + 1] = string.pack('<s2s4', name, result.nativeMembers[name]) end
    return table.concat(parts)
end

local ok, body = xpcall(run, debug.traceback)
if not ok then body = string.pack('<Bs2s4', 0, 'TEMPLATE_HOST_FAILED', tostring(body):sub(1, 8192)) end
writeResult(body)
