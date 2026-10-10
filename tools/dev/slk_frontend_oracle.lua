-- 固定目标 frontend 的原创输入 oracle；不执行模板，也不调用 backend/save。
-- 已安装 sandbox.lua 仅用于复用目标模块加载方式，不是未知 Lua 的安全隔离。
local manifestPath, fixturePath = assert(arg[1]), assert(arg[2])
local manifest = assert(loadfile(manifestPath, 't', {}))()
package.path, package.cpath = '', ''
local modules = {}
for _, name in ipairs {'lpeg', 'lni', 'w3xparser', 'lml'} do
    local loader, err = package.loadlib(assert(manifest.modules[name]), 'luaopen_' .. name)
    assert(loader, err)
    modules[name] = loader()
    package.loaded[name] = modules[name]
end
assert(type(modules.lni.classics) == 'function' and type(modules.w3xparser.slk) == 'function')
assert(type(math.type) == 'function' and string.packsize('l') == 4, 'requires target Windows Lua 5.3 ABI')
io.stdout:write('SLK_FRONTEND_ORACLE_READY\n'); io.stdout:flush()
assert(io.stdin:read('*l') == 'RUN', 'actual module identity was not approved')

local function canonical(path)
    -- mpq_path 的空层会产生 mpq\\\\units；Windows 将其视为同一路径。
    path = path:gsub('/', '\\'):gsub('\\+', '\\'):lower()
    assert(not path:find('\0', 1, true) and not path:match('[\\]%.[%.]?[\\]'), 'unsafe source path')
    return path
end
local function hex(value) return (value:gsub('.', function(c) return string.format('%02x', c:byte()) end)) end
local function quote(value)
    return '"' .. value:gsub('[%z\1-\31\\"]', function(c) return string.format('\\u%04x', c:byte()) end) .. '"'
end
local function json(value)
    if type(value) == 'string' then return quote(value) end
    if type(value) == 'number' or type(value) == 'boolean' then return tostring(value) end
    assert(type(value) == 'table', 'unsupported JSON value')
    local keys = {}; for key in pairs(value) do keys[#keys+1] = key end
    table.sort(keys)
    local parts = {}
    for _, key in ipairs(keys) do parts[#parts+1] = quote(tostring(key)) .. ':' .. json(value[key]) end
    return '{' .. table.concat(parts, ',') .. '}'
end
local function kind(v) return type(v) == 'number' and math.type(v) or type(v) end
local function typed(v)
    local k = kind(v)
    if k == 'nil' then return {kind='nil'} end
    if k == 'integer' then return {kind=k, decimal=tostring(v)} end
    if k == 'float' then return {kind=k, ieee754le=hex(string.pack('<d',v)), hex=string.format('%a',v)} end
    if k == 'boolean' then return {kind=k, value=v} end
    assert(k == 'string' and #v <= 4096, 'selection is not a bounded scalar')
    return {kind=k, byteCount=#v, hex=hex(v)}
end
local function at(tree, path)
    for _, key in ipairs(path) do if type(tree) ~= 'table' then return nil end; tree = rawget(tree,key) end
    return tree
end
local function count(t) local n=0; for _ in pairs(t) do n=n+1 end; return n end
local cases = assert(loadfile(fixturePath, 't', {string=string, table=table, pairs=pairs}))()
assert(type(cases) == 'table' and #cases > 0 and #cases <= 12, 'case budget')
local sourceIndex = {}
for _, entry in ipairs(manifest.files) do sourceIndex[canonical(entry.path)] = entry end
local failed = 0
for _, case in ipairs(cases) do
    local readSet, mapReads, writes, messages, buildCalls = {}, {}, {}, {}, 0
    local physicalBytes, readCalls = 0, 0
    local function physical(path, required)
        local key = canonical(path)
        local entry = sourceIndex[key]
        assert(key:sub(1,#manifest.corePrefix) == manifest.corePrefix or
            key:sub(1,#manifest.installedPrefix) == manifest.installedPrefix or
            key == canonical(manifest.sandbox), 'source escaped explicit roots')
        if not entry then
            -- 仅允许核心模块探测或 Custom_V1 的正常下层回退。
            assert(not required, 'required source missing from locked manifest: ' .. path)
            readSet[key] = {path=path, present=false}
            return nil
        end
        readCalls = readCalls + 1; assert(readCalls <= 4096, 'physical read call budget')
        local f = assert(io.open(entry.path, 'rb'))
        local content = f:read(entry.bytes+1); f:close()
        assert(#content == entry.bytes, 'locked source length changed')
        physicalBytes = physicalBytes + #content
        assert(physicalBytes <= 128*1024*1024, 'cumulative physical read budget')
        readSet[key] = {path=entry.path, present=true, sha256=entry.sha256, bytes=#content}
        return content
    end
    local function openCore(path, mode)
        assert(mode == nil or mode == 'r' or mode == 'rb', 'source write denied')
        local content = physical(path, false)
        if content == nil then return nil end
        local closed = false
        return {
            read=function(_, format) assert(not closed and (format=='a' or format=='*a'), 'unexpected core read'); return content end,
            close=function() closed=true end,
        }
    end
    local ok, result = xpcall(function()
        local loaded = {}
        for name, module in pairs(modules) do loaded[name]=module end
        loaded.data_load = function(w2l, name)
            assert(w2l.setting.data == manifest.dataId, 'config selected an unpinned dataset')
            name = name:gsub('/', '\\')
            local relative, optional
            if name:sub(1,9) == 'prebuilt\\' then
                relative = 'plugin\\w3x2lni\\data\\' .. manifest.dataId .. '\\' .. name
            elseif name:sub(1,3) == 'ui\\' then relative = name
            else
                relative = 'share\\' .. manifest.locale .. '\\' .. name
                optional = name:lower():match('^mpq\\custom_v1\\') ~= nil
            end
            return physical(manifest.installedRoot .. '\\' .. relative, not optional)
        end
        local sandbox = assert(load(assert(physical(manifest.sandbox, true)), '@'..manifest.sandbox, 't', _G))()
        local factory = assert(sandbox(manifest.coreRoot .. '\\', openCore, loaded))
        local w2l = factory()
        w2l:set_setting {mode='obj', read_slk=true, version=case.version}
        w2l.input_mode = 'obj'
        w2l:set_messager(function(event, ...)
            assert(#messages < 256, 'message budget')
            local args = table.pack(...)
            local pieces = {tostring(event)}
            for i=1,args.n do pieces[#pieces+1]=tostring(args[i]):sub(1,1024) end
            messages[#messages+1]=table.concat(pieces,' | ')
        end)
        assert(count(case.members) <= 8, 'original member count budget')
        local mapBytes = 0
        for _, value in pairs(case.members) do mapBytes=mapBytes+#value end
        assert(mapBytes <= 64*1024, 'original member byte budget')
        w2l.input_ar = {get=function(_, name)
            local key=name:gsub('/','\\'):lower()
            local content=case.members[key]
            mapReads[key]={present=content~=nil, bytes=content and #content or 0}
            return content
        end}
        w2l.output_ar = {
            set=function(_,name) error('frontend unexpectedly requested file output: '..name) end,
            remove=function(_,name)
                assert(name=='w3x2lni\\locale\\w3i.lng' or name=='w3x2lni\\locale\\lml.lng', 'unexpected removal')
                writes[#writes+1]={operation='remove',name=name,committed=false}
            end,
        }
        local originalBuild = w2l.frontend_buildslk
        w2l.frontend_buildslk = function(self, loadMap)
            buildCalls=buildCalls+1
            assert(loadMap==true and buildCalls<=1, 'unexpected fallback or repeat buildslk')
            return originalBuild(self,loadMap)
        end
        local slk={}; w2l:frontend(slk)
        assert(w2l.setting.version == case.version, 'W3I version selection differs')
        assert(buildCalls == case.buildCalls, 'buildslk branch differs from authored expectation')
        assert(at(slk,{'unit','Z001','name'}) == 'Original native WTS marker', 'native/WTS overlay was not applied')
        assert(kind(at(slk,{'unit','Z001','hp'}))=='integer' and at(slk,{'unit','Z001','hp'})==873, 'native integer differs')
        for _,check in ipairs(case.checks) do
            local value=at(slk,check.path)
            assert(kind(value)==check.kind and value==check.value, 'original overlay differs: '..table.concat(check.path,'.'))
        end
        local selection, counts = {}, {}
        for _, path in ipairs {
            {'unit','hfoo','name'}, {'unit','hfoo','hp'}, {'unit','Z001','name'}, {'unit','Z001','hp'},
            {'misc','Misc','goldtextheight'}, {'misc','Misc','buildingangle'}, {'misc','Terrain','maxslope'},
        } do selection[table.concat(path,'.')]=typed(at(slk,path)) end
        for _,name in ipairs {'unit','ability','item','buff','upgrade','doodad','destructable','misc','txt'} do
            assert(type(slk[name])=='table', 'missing frontend class '..name); counts[name]=count(slk[name])
        end
        return {selection=selection,counts=counts,version=w2l.setting.version}
    end, debug.traceback)
    if not ok then failed=failed+1 end
    local row={caseId=case.id,passed=ok,buildCalls=buildCalls,readSet=readSet,mapReads=mapReads,
        outputRequests=writes,messages=messages,physicalBytes=physicalBytes,physicalReads=readCalls}
    if ok then row.actual=result else row.error=tostring(result) end
    io.stdout:write(json(row),'\n'); io.stdout:flush()
    collectgarbage('collect')
end
io.stdout:write(json{summary=true,caseCount=#cases,failedExpectations=failed,luaVersion=_VERSION,
    coverage='OriginalFrontendInputsAndInstalledReadOnlyBaseline'} , '\n'); io.stdout:flush()
os.exit(failed == 0 and 0 or 1)
