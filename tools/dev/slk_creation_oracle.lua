-- 原创测试宿主。实际目标库只从显式路径加载；不复制目标实现，不执行地图模板。
local libraryPath, fixturePath = assert(arg[1]), assert(arg[2])
package.path, package.cpath = '', ''
assert(_VERSION == 'Lua 5.3' and math.type and string.pack, 'pinned Lua 5.3 required')
assert(loadfile(libraryPath, 't', {})) -- 只编译，执行与所有case都在宿主核验身份后。
io.stdout:write('SLK_CREATION_ORACLE_READY\n'); io.stdout:flush()
assert(io.stdin:read('*l') == 'RUN', 'host/runtime/library identity handshake not approved')
local fixture = assert(loadfile(fixturePath, 't', {string={rep=string.rep}}))()
assert(fixture.schema == 'slk-creation-original-inputs-v1' and #fixture.cases > 0 and #fixture.cases <= 32, 'invalid fixture')
local types = {'ability','buff','unit','item','upgrade','doodad','destructable','misc'}
local members = {ability='war3map.w3a',buff='war3map.w3h',unit='war3map.w3u',item='war3map.w3t',upgrade='war3map.w3q',doodad='war3map.w3d',destructable='war3map.w3b'}
local function kind(value) return type(value)=='number' and math.type(value) or type(value) end
local function hex(value) return (value:gsub('.', function(c) return string.format('%02x',string.byte(c)) end)) end
local function quote(value)
    return '"'..value:gsub('[%z\1-\31\\"]',function(c) return string.format('\\u%04x',string.byte(c)) end)..'"'
end
local function clone(value,depth,budget)
    depth,budget=depth or 0,budget or {nodes=0,bytes=0,active={}}
    budget.nodes=budget.nodes+1; assert(budget.nodes<=65536 and depth<=16,'original input clone budget')
    if type(value)=='string' then budget.bytes=budget.bytes+#value; assert(#value<=65536 and budget.bytes<=1048576,'original string budget') end
    if type(value)~='table' then assert(type(value)=='number' or type(value)=='boolean' or type(value)=='string','original scalar type'); return value end
    assert(not getmetatable(value) and not budget.active[value],'original fixture cannot contain metatable/cycle')
    budget.active[value]=true
    local result={}
    for key,item in pairs(value) do result[clone(key,depth+1,budget)]=clone(item,depth+1,budget) end
    budget.active[value]=nil
    return result
end
local function export(value,depth,budget)
    depth,budget=depth or 0,budget or {nodes=0,bytes=0}
    budget.nodes=budget.nodes+1; assert(depth<=16 and budget.nodes<=8192,'typed output node budget')
    local t=kind(value)
    if t=='integer' then return '{"kind":"integer","decimal":'..quote(tostring(value))..'}' end
    if t=='float' then return '{"kind":"float","hex":'..quote(string.format('%a',value))..',"ieee754le":'..quote(hex(string.pack('<d',value)))..'}' end
    if t=='boolean' then return '{"kind":"boolean","value":'..tostring(value)..'}' end
    if t=='string' then
        budget.bytes=budget.bytes+#value; assert(#value<=65536 and budget.bytes<=1048576,'typed output string budget')
        return '{"kind":"string","byteCount":'..#value..',"hex":'..quote(hex(value))..'}'
    end
    assert(t=='table','uncovered typed value '..t)
    local keys={}
    for key in pairs(value) do assert(#keys<4096,'typed key budget'); keys[#keys+1]=key end
    table.sort(keys,function(a,b)
        local ka,kb=kind(a),kind(b)
        if ka~=kb then return ka<kb end
        if ka=='integer' or ka=='float' or ka=='string' then return a<b end
        if ka=='boolean' then return not a and b end
        error('unsupported typed table key')
    end)
    local entries={}
    for _,key in ipairs(keys) do entries[#entries+1]='{"key":'..export(key,depth+1,budget)..',"value":'..export(value[key],depth+1,budget)..'}' end
    return '{"kind":"table","count":'..#keys..',"entries":['..table.concat(entries,',')..']}'
end
local outputBytes=0
local function emit(text)
    outputBytes=outputBytes+#text+1; assert(outputBytes<=16*1024*1024,'total oracle output budget')
    io.stdout:write(text,'\n')
end
local function checksAt(case,step,state,failures)
    for index,check in ipairs(case.checks) do if check.step==step then
        local value=state
        for _,key in ipairs(check.path) do if type(value)~='table' then value=nil; break end; value=rawget(value,key) end
        local ok
        if check.absent then ok=value==nil
        else
            ok=kind(value)==check.kind
            if check.value~=nil then ok=ok and value==check.value end
            if check.bytes then ok=ok and type(value)=='string' and #value==check.bytes end
            if check.count then
                local count=0; if type(value)=='table' then for _ in pairs(value) do count=count+1 end end
                ok=ok and type(value)=='table' and count==check.count
            end
        end
        if not ok then failures[#failures+1]='authored check '..index..' at step '..step..' differs' end
    end end
end
local function runCase(case)
    assert(type(case.id)=='string' and case.id:match('^[a-z0-9_]+$') and #case.id<=80,'invalid case ID')
    assert(#case.ops<=32 and #case.checks<=64 and #(case.patches or {})<=32,'case budget')
    for _,check in ipairs(case.checks) do assert(check.step>=0 and check.step<=#case.ops and #check.path<=12,'check cannot refer to unobserved step') end
    local input={defaults=clone(fixture.defaults),old=clone(fixture.old),metadata=clone(fixture.metadata)}
    for _,tp in ipairs(types) do
        input.defaults[tp]=input.defaults[tp] or {}; input.old[tp]=input.old[tp] or {}; input.metadata[tp]=input.metadata[tp] or {}
    end
    for _,patch in ipairs(case.patches or {}) do
        assert(#patch.path>0 and #patch.path<=12,'patch path budget')
        local target=assert(input[patch.scope],'unsupported patch scope')
        for index=1,#patch.path-1 do target=assert(target[patch.path[index]],'patch missing parent') end
        if patch.remove then target[patch.path[#patch.path]]=nil else target[patch.path[#patch.path]]=clone(patch.value) end
    end
    local inputTyped=export(input)
    local backend={cleanCount=0,objectCount=0,saveCount=0,removeCount=0,events={}}
    local errors,infos={},{}
    local function logTo(destination,...)
        assert(#destination<64,'log count budget')
        local parts={}; for index=1,select('#',...) do
            local text=tostring(select(index,...)); assert(#text<=8192,'log string budget'); parts[#parts+1]=text
        end
        destination[#destination+1]=table.concat(parts,' | ')
    end
    local env={assert=assert,error=error,ipairs=ipairs,next=next,pairs=pairs,pcall=pcall,select=select,setmetatable=setmetatable,
        tonumber=tonumber,tostring=tostring,type=type,math=math,string=string,table=table,
        log={error=function(...) logTo(errors,...) end,info=function(...) logTo(infos,...) end}}
    -- 每case重新编译并执行完整目标chunk，module-local w2l/errors/slk_proxy不会跨case共享。
    local factory=assert(loadfile(libraryPath,'t',env))()
    assert(type(factory)=='function','target library must return session factory')
    local observed
    local w2l={setting={},messager={},info={key={max_level={ability='levels',upgrade='maxlevel',doodad='numvar'}},obj=members}}
    function w2l:frontend(slk)
        for _,tp in ipairs(types) do slk[tp]=clone(input.old[tp]) end
        observed=slk -- 可信测试注入点持有目标实际修改的表，未用代理读取（会around_number）。
    end
    function w2l:get_default() return input.defaults end
    function w2l:metadata() return input.metadata end
    function w2l:get_editstring(value) return value end
    function w2l:backend_cleanobj(objects)
        backend.cleanCount=backend.cleanCount+1
        backend.events[#backend.events+1]={kind='clean-observed-only',objects=clone(objects)}
    end
    function w2l:backend_obj(tp,objects)
        backend.objectCount=backend.objectCount+1
        backend.events[#backend.events+1]={kind='encode-stub',type=tp,objects=clone(objects)}
        if next(objects) then return 'ORIGINAL-ORACLE-NOT-NATIVE-BYTES' end
        return nil
    end
    function w2l:file_save(scope,path,bytes)
        assert(scope=='map' and bytes=='ORIGINAL-ORACLE-NOT-NATIVE-BYTES','unexpected save stub request')
        backend.saveCount=backend.saveCount+1; backend.events[#backend.events+1]={kind='save-observed-only',path=path}
    end
    function w2l:file_remove(scope,path)
        assert(scope=='map','unexpected remove scope')
        backend.removeCount=backend.removeCount+1; backend.events[#backend.events+1]={kind='remove-observed-only',path=path}
    end
    local proxy=factory(w2l,false,false)
    local steps,failures={},{}
    local traceBytes=0
    local function capture(step,operation,ok,errorValue)
        local state={objects=observed,defaults=input.defaults,backend=backend,errorCount=#errors,errors=errors}
        if step<=#case.ops then checksAt(case,step,state,failures) end
        local typed=export(state)
        local text='{"step":'..step..',"operation":'..quote(operation)..',"ok":'..tostring(ok)..',"errorHex":'
            ..(ok and 'null' or quote(hex(tostring(errorValue))))..',"typed":'..typed..'}'
        traceBytes=traceBytes+#text; assert(traceBytes<=2*1024*1024,'case trace byte budget')
        steps[#steps+1]=text
    end
    capture(0,'initialized',true)
    local current,allOk=nil,true
    for step,operation in ipairs(case.ops) do
        local ok,message=pcall(function()
            if operation.kind=='new' then current=assert(proxy.ability[operation.parent]):new(operation.id)
            elseif operation.kind=='write' then assert(current,'write before new')[operation.field]=operation.value
            else error('unsupported original operation') end
        end)
        if not ok then allOk=false; failures[#failures+1]='operation '..step..' raised an unexpected error' end
        capture(step,operation.kind,ok,message)
        if not ok then break end
    end
    local refreshOk,report=pcall(function() return proxy:refresh() end)
    if not refreshOk then failures[#failures+1]='refresh raised an unexpected error' end
    capture(#case.ops+1,'refresh',refreshOk,report)
    if backend.cleanCount~=1 or backend.saveCount~=case.save or backend.removeCount~=case.remove or #errors~=case.errors then
        failures[#failures+1]='refresh call/error counts differ from authored expectation'
    end
    local failureJson={}; for _,message in ipairs(failures) do failureJson[#failureJson+1]=quote(message) end
    emit('{"caseId":'..quote(case.id)..',"cppPolicy":'..quote(case.cppPolicy)..',"operationCount":'..#case.ops
        ..',"checkCount":'..#case.checks..',"executionOk":'..tostring(allOk and refreshOk)..',"expectationPassed":'..tostring(#failures==0)
        ..',"failures":['..table.concat(failureJson,',')..'],"saveCount":'..backend.saveCount..',"removeCount":'..backend.removeCount
        ..',"errorCount":'..#errors..',"reportHex":'..(refreshOk and quote(hex(report)) or 'null')
        ..',"inputTyped":'..inputTyped..',"steps":['..table.concat(steps,',')..']}')
    return #failures==0
end
local failed,seen=0,{}
for _,case in ipairs(fixture.cases) do
    assert(not seen[case.id],'duplicate case'); seen[case.id]=true
    if not runCase(case) then failed=failed+1 end
end
emit('{"summary":true,"coverage":"OriginalInjectedOldNewFillRefreshOnly","luaVersion":'..quote(_VERSION)
    ..',"caseCount":'..#fixture.cases..',"isolatedLibraryLoads":'..#fixture.cases..',"failedExpectations":'..failed..'}')
io.stdout:flush()
if failed>0 then os.exit(1) end
