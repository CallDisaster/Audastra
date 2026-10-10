-- Audastra 原创运行库元数据探针。仅由独立测试地图执行；不调用任何游戏 API。
-- 由准备工具替换一次 nonce；宿主只采纳本次匿名 IPC 的同 nonce 返回值。
local nonce = '__AUDASTRA_PROBE_NONCE__'
local commonCandidates = __AUDASTRA_COMMON_CANDIDATES__
local binaryModuleCandidates = __AUDASTRA_MODULE_CANDIDATES__
local limitModules, limitFields, limitBytes = 64, 24000, 500000
local function quote(text)
    return '"' .. tostring(text):gsub('[%z\1-\31\\"]', function(character)
        if character == '\\' then return '\\\\' end
        if character == '"' then return '\\"' end
        return ('\\u%04x'):format(character:byte())
    end) .. '"'
end
local function failure(message)
    return 'AUDASTRA_LUA_API_V1:' .. nonce .. ':'
        .. '{"schema":"audastra-lua-api-observation-v1","nonce":' .. quote(nonce)
        .. ',"completed":false,"reason":' .. quote(message) .. ',"modules":[]}'
end
if not nonce:match('^[a-f0-9]+$') or #nonce ~= 32 then return failure('Invalid probe nonce') end
if type(package) ~= 'table' or type(package.loaded) ~= 'table' or type(package.preload) ~= 'table' then
    return failure('Runtime does not expose package.loaded/package.preload; no module list was invented')
end
if type(package.searchers) ~= 'table' or type(debug) ~= 'table' or type(debug.getinfo) ~= 'function'
    or type(require) ~= 'function' or debug.getinfo(require, 'S').what ~= 'C' then
    return failure('Runtime native require/searcher metadata unavailable; filesystem fallback was not executed')
end
local originalSearchers = package.searchers
local function nativePreloadOnly(name)
    local loader = rawget(package.preload, name)
    if type(loader) == 'function' and debug.getinfo(loader, 'S').what == 'C' then return loader end
    return 'Unregistered runtime C preload; filesystem Lua/C searchers were not executed'
end
-- require 的文件 Lua/C searcher（含嵌套 require）在本次观察中全部禁用；结束后还原。
rawset(package, 'searchers', {nativePreloadOnly})
local discovered, candidates, fields = {}, {}, 0
local function discover(source, origin)
    for name in next, source do
        if type(name) == 'string' and name:match('^jass%.[%w_%.]+$') then
            if not discovered[name] then
                if #candidates >= limitModules then error('Module enumeration budget exceeded', 0) end
                discovered[name] = { origin = origin }
                candidates[#candidates + 1] = name
            elseif discovered[name].origin ~= origin then discovered[name].origin = 'loaded-and-preload' end
        end
    end
end
local success, observed = pcall(function()
    discover(package.loaded, 'package.loaded')
    discover(package.preload, 'package.preload')
        -- 这些仅是本机实际二进制中的候选名字；缺 preload/loaded 时保持未知。
    for _, name in ipairs(binaryModuleCandidates) do
        if not discovered[name] then
            if #candidates >= limitModules then error('Module enumeration budget exceeded', 0) end
            discovered[name] = {origin = 'local-runtime-binary-candidate'}
            candidates[#candidates + 1] = name
        end
    end
    table.sort(candidates)
    if #candidates == 0 then error('No jass.* modules were exposed by runtime package tables; internal registrations were not guessed', 0) end
    local modules = {}
    for _, name in ipairs(candidates) do
        local loaded, loader = rawget(package.loaded, name), rawget(package.preload, name)
        local registered = loaded ~= nil and loaded ~= false
            or type(loader) == 'function' and debug.getinfo(loader, 'S').what == 'C'
        local ok, module = false, 'Not registered in actual runtime loaded/native preload; existence unknown; no fallback chunk executed'
        if registered then ok, module = pcall(require, name) end
        local item = '{"name":' .. quote(name) .. ',"discovery":' .. quote(discovered[name].origin)
            .. ',"available":' .. tostring(ok) .. ',"enumerationComplete":false'
        if not ok then
            item = item .. ',"reason":' .. quote(tostring(module):sub(1, 1024)) .. ',"fields":[]}'
        elseif type(module) ~= 'table' then
            item = item .. ',"valueType":' .. quote(type(module))
                .. ',"reason":"Module value is not a table; fields were not inferred","fields":[]}'
        else
            local entries, seen = {}, {}
            local function record(key, value, origin)
                if type(key) ~= 'string' or #key > 256 or not key:match('^[%a_][%w_]*$') then return end
                if seen[key] then
                    -- 某些 __pairs 返回的是注册元数据而非 module[key] 的可调用值。
                    -- 同次声明候选的实际 lookup 才可确认 common 函数类型。
                    if origin == 'common-declaration-runtime-lookup' then
                        seen[key].kind, seen[key].origin = type(value), origin
                    end
                    return
                end
                fields = fields + 1
                if fields > limitFields then error('Field enumeration budget exceeded', 0) end
                local entry = {name = key, kind = type(value), origin = origin}
                seen[key] = entry
                entries[#entries + 1] = entry
            end
            for key, value in next, module do record(key, value, 'raw-table') end
            -- __pairs 是运行库自己的元数据入口；失败只降低覆盖，不推断缺失函数。
            local enumerationOk, enumerationError = pcall(function()
                for key, value in pairs(module) do record(key, value, 'runtime-pairs') end
            end)
            if name == 'jass.common' then
                -- 姓名来自同次 SHA 冻结的 common.j 生产声明索引；这里只查询，不调用函数。
                for _, candidate in ipairs(commonCandidates) do
                    local present, value = pcall(function() return module[candidate] end)
                    if present and value ~= nil then record(candidate, value, 'common-declaration-runtime-lookup') end
                end
            end
            table.sort(entries, function(left, right) return left.name < right.name end)
            local encoded = {}
            for _, entry in ipairs(entries) do
                encoded[#encoded + 1] = '{"name":' .. quote(entry.name) .. ',"kind":' .. quote(entry.kind)
                    .. ',"origin":' .. quote(entry.origin) .. ',"parameterCount":null}'
            end
            item = item .. ',"valueType":"table","fields":[' .. table.concat(encoded, ',') .. ']'
            if not enumerationOk then item = item .. ',"reason":' .. quote(tostring(enumerationError):sub(1, 1024))
            elseif #entries == 0 then item = item .. ',"reason":"No enumerable fields; lazy __index coverage is unknown"'
            else item = item .. ',"reason":"Observed fields only; absence and function signatures remain unknown"' end
            item = item .. '}'
        end
        modules[#modules + 1] = item
    end
    local json = '{"schema":"audastra-lua-api-observation-v1","nonce":' .. quote(nonce)
        .. ',"completed":true,"moduleCoverage":"runtime-discovery-and-candidates-partial","gameApiFunctionsInvokedByProbe":0,"runtimePreloadOnly":true'
        .. ',"signatures":"unknown","modules":[' .. table.concat(modules, ',') .. ']}'
    if #json > limitBytes then error('Payload budget exceeded', 0) end
    return json
end)
rawset(package, 'searchers', originalSearchers)
if not success then return failure(tostring(observed):sub(1, 2048)) end
return 'AUDASTRA_LUA_API_V1:' .. nonce .. ':' .. observed
