-- 固定目标工具执行 oracle；只有仓库原创fixture，不接受任意地图模板参数。
local manifestPath,fixturePath=assert(arg[1]),assert(arg[2])
local manifest=assert(loadfile(manifestPath,'t',{}))()
package.path,package.cpath='',''
local modules={}
for _,name in ipairs {'lpeg','lni','w3xparser','lml'} do
    local loader,err=package.loadlib(assert(manifest.modules[name]),'luaopen_'..name)
    assert(loader,err); modules[name]=loader(); package.loaded[name]=modules[name]
end
assert(string.packsize('l')==4 and type(modules.lni.classics)=='function','target Lua ABI mismatch')
io.stdout:write('SLK_TRANSACTION_ORACLE_READY\n'); io.stdout:flush()
assert(io.stdin:read('*l')=='RUN','actual loaded module identity was not approved')
local function canonical(path) return path:gsub('/','\\'):gsub('\\+','\\'):lower() end
local function hex(value) return (value:gsub('.',function(c) return string.format('%02x',c:byte()) end)) end
local function quote(value)
    return '"'..value:gsub('[%z\1-\31\\"]',function(c) return string.format('\\u%04x',c:byte()) end)..'"'
end
local function json(value)
    if type(value)=='string' then return quote(value) end
    if type(value)=='number' or type(value)=='boolean' then return tostring(value) end
    assert(type(value)=='table','unsupported JSON value')
    local keys={}; for key in pairs(value) do keys[#keys+1]=key end; table.sort(keys)
    local parts={}; for _,key in ipairs(keys) do parts[#parts+1]=quote(tostring(key))..':'..json(value[key]) end
    return '{'..table.concat(parts,',')..'}'
end
local function count(value) local n=0; for _ in pairs(value) do n=n+1 end; return n end
local function upvalue(fn,name)
    for i=1,64 do
        local key,value=debug.getupvalue(fn,i)
        if not key then break end
        if key==name then return value end
    end
    error('fixed target upvalue missing: '..name)
end
local sourceIndex={}
for _,entry in ipairs(manifest.files) do sourceIndex[canonical(entry.path)]=entry end
local cases=assert(loadfile(fixturePath,'t',{string=string,table=table,pairs=pairs,ipairs=ipairs}))()
assert(type(cases)=='table' and #cases>0 and #cases<=32,'fixture count budget')
local failed=0
for _,case in ipairs(cases) do
    local readSet,mapReads,messages={}, {}, {}
    local physicalReads,physicalBytes=0,0
    local function physical(path,required)
        local key=canonical(path)
        assert(key:sub(1,#manifest.corePrefix)==manifest.corePrefix or key:sub(1,#manifest.installedPrefix)==manifest.installedPrefix or
            key==canonical(manifest.sandbox) or key==canonical(manifest.hostLua),'source escaped locked roots')
        local entry=sourceIndex[key]
        if not entry then assert(not required,'required locked source missing: '..path); readSet[key]={path=path,present=false}; return nil end
        physicalReads=physicalReads+1; assert(physicalReads<=8192,'read call budget')
        local f=assert(io.open(entry.path,'rb')); local bytes=f:read(entry.bytes+1); f:close()
        assert(#bytes==entry.bytes,'locked source length differs')
        physicalBytes=physicalBytes+#bytes; assert(physicalBytes<=256*1024*1024,'read byte budget')
        readSet[key]={path=entry.path,present=true,sha256=entry.sha256,bytes=#bytes}
        return bytes
    end
    local function openCore(path,mode)
        assert(mode==nil or mode=='r' or mode=='rb','source write denied')
        local bytes=physical(path,false); if bytes==nil then return nil end
        return {read=function(_,format) assert(format=='a' or format=='*a','unexpected read'); return bytes end,close=function() end}
    end
    local function message(...)
        assert(#messages<1024,'diagnostic count budget')
        local args=table.pack(...); local parts={}
        for i=1,args.n do parts[#parts+1]=tostring(args[i]):sub(1,2048) end
        messages[#messages+1]=table.concat(parts,' | ')
    end
    local logger=setmetatable({},{__index=function(_,name) return function(...) message(name,...) end end})
    local host=assert(load(assert(physical(manifest.hostLua,true)),'@'..manifest.hostLua,'t',_G))()
    assert(host.schema=='forge-lua-template-host-v1','host protocol mismatch')
    local function run(code,members,limits)
        local trusted={compileChunkProtocol='forge-guarded-template-load-v1'}
        trusted.inputMemberExists=function(name)
            local key=canonical(name); local bytes=members[key]
            mapReads[key]={present=bytes~=nil,bytes=bytes and #bytes or 0,membershipChecked=true}
            return bytes~=nil
        end
        trusted.createSession=function(output)
            local loaded={}; for name,module in pairs(modules) do loaded[name]=module end
            loaded.data_load=function(w2l,name)
                assert(w2l.setting.data==manifest.dataId,'unlocked dataset')
                name=name:gsub('/','\\')
                local relative,optional
                if name:sub(1,9)=='prebuilt\\' then relative='plugin\\w3x2lni\\data\\'..manifest.dataId..'\\'..name
                elseif name:sub(1,3)=='ui\\' then relative=name
                else relative='share\\'..manifest.locale..'\\'..name; optional=name:lower():match('^mpq\\custom_v1\\')~=nil end
                return physical(manifest.installedRoot..'\\'..relative,not optional)
            end
            local sandbox=assert(load(assert(physical(manifest.sandbox,true)),'@'..manifest.sandbox,'t',_G))()
            local factory=assert(sandbox(manifest.coreRoot..'\\',openCore,loaded))
            local w2l=factory(); w2l:set_setting {mode='obj',read_slk=true,version='Custom'}; w2l.input_mode='obj'
            w2l:set_messager(message)
            assert(count(members)<=16,'member count budget')
            local inputBytes=0; for _,bytes in pairs(members) do inputBytes=inputBytes+#bytes end
            assert(inputBytes<=1024*1024,'original member byte budget')
            w2l.input_ar={get=function(_,name)
                local key=canonical(name); local bytes=members[key]
                mapReads[key]={present=bytes~=nil,bytes=bytes and #bytes or 0}
                return bytes
            end}
            w2l.output_ar={set=function(_,name,bytes) output('set',name,bytes) end,remove=function(_,name) output('remove',name) end}
            -- 目标模块运行于可信工具环境；这个_G不成为模板environment的回退。
            local toolEnv=setmetatable({log=logger},{__index=_G})
            local construct=assert(load(assert(physical(manifest.slkLibrary,true)),'@'..manifest.slkLibrary,'t',toolEnv))()
            local rawSlk=construct(w2l,false,true)
            local realRefresh=assert(rawget(rawSlk,'refresh'))
            local state=upvalue(realRefresh,'session')
            assert(type(state)=='table' and type(state.default)=='table' and type(state.metadata)=='table','unexpected target state shape')
            local sample=rawSlk.ability.AHhb
            local tryMeta=upvalue(assert(debug.getmetatable(sample).__newindex),'try_meta')
            assert(type(tryMeta)=='function','target try_meta missing')
            return {
                rawSlk=rawSlk,
                refresh=function() return realRefresh(rawSlk) end,
                errors=function() return upvalue(construct,'errors') end,
                hasStandard=function(class,id) return state.default[class] and state.default[class][id]~=nil end,
                validateWrite=function(class,parent,key)
                    local base=state.default[class] and state.default[class][parent]
                    if not base then return nil end
                    local meta,level=tryMeta(key,state.metadata[class],base._code and state.metadata[base._code])
                    if not meta then return nil end
                    return meta.field:lower(),level
                end,
            }
        end
        trusted.compileWhole=function(text,environment,compileChunk)
            local written,detail
            local templateEnv={load=compileChunk,string=string,table=table,pcall=pcall,xpcall=xpcall,tostring=tostring,
                debug={traceback=debug.traceback},log=logger,LNG={UNKNOWN='unknown template error'},
                gui={error_message=function(_,value) detail=tostring(value) end},
                require=function(name) assert(name=='compiler.computed.env','unexpected target template dependency'); return environment end,
                io={load=function(path) assert(path=='oracle-input','unexpected template input'); return text end,
                    save=function(path,bytes) assert(path=='oracle-output','unexpected template output'); written=bytes; return true end}}
            -- physical()以普通字符串方法规范化路径，暂不在模板受限元表下读取源码。
            local template=assert(load(trusted.templateSource,'@'..manifest.templateLibrary,'t',templateEnv))()
            local success=template:compile {input='oracle-input',output='oracle-output',map_path='oracle-readonly-map'}
            return success,written,detail
        end
        trusted.templateSource=assert(physical(manifest.templateLibrary,true))
        local result=host.Run({schema='forge-lua-template-request-v1',preprocessed=code,limits=limits},trusted)
        assert(debug.getmetatable('')==manifest._originalStringMeta,'host did not restore string metatable')
        assert(debug.gethook()==nil,'host did not restore hook')
        return result
    end
    manifest._originalStringMeta=debug.getmetatable('')
    local ok,actual=xpcall(function()
        local result=run(case.code,case.members,case.limits)
        assert(result.ok==case.expect.ok,'success state differs: '..tostring(result.errorCode)..' '..tostring(result.error))
        local row={ok=result.ok,coverage=result.coverage}
        if not result.ok then
            assert(result.errorCode==case.expect.errorCode,'failure code differs: '..tostring(result.errorCode)..' '..tostring(result.error))
            assert(result.script==nil and result.nativeMembers==nil and result.memberOps==nil,'failure exposed partial output')
            row.errorCode=result.errorCode; row.error=result.error
            return row
        end
        assert(result.script==case.expect.script,'wholechunk script differs')
        assert(count(result.nativeMembers)==#case.expect.members,'native member count differs')
        local members={}
        for _,name in ipairs(case.expect.members) do
            local bytes=assert(result.nativeMembers[name],'expected native output missing')
            members[name]={bytes=#bytes,hex=hex(bytes)}
        end
        if case.expect.emptyNative then assert(result.nativeMembers[case.expect.emptyNative]==string.pack('<i4i4i4',2,0,0),'remove did not become empty v2 native') end
        if case.expect.compactScript then
            row.scriptEvidence={bytes=#result.script,prefixHex=hex(result.script:sub(1,32)),suffixHex=hex(result.script:sub(-32)),exactOriginalExpectedMatched=true}
        else row.scriptHex=hex(result.script) end
        row.nativeMembers=members; row.memberOps=result.memberOps; row.stats=result.stats
        assert(result.stats.templateLoads==1 and result.stats.templateExecutions==1 and result.stats.outputSinkCalls==1,'wholechunk was not executed exactly once')
        if case.expect.creations then assert(result.stats.creations==case.expect.creations,'creation count differs') end
        row.metadataCoverage=result.metadataCoverage; row.metadataRequests=result.metadataRequests
        assert(result.metadataCoverage=='AbsentLocaleCleanupOnly','metadata coverage missing')
        assert(result.stats.metadataRequests==count(result.metadataRequests),'metadata request count differs')
        for _,event in ipairs(result.metadataRequests) do
            assert(event.operation=='remove' and event.inputPresent==false and event.disposition=='AbsentInputNoOp','unexpected metadata disposition')
        end
        if case.expect.roundtrip then
            local nextMembers={}; for name,bytes in pairs(case.members) do nextMembers[name]=bytes end
            for name,bytes in pairs(result.nativeMembers) do nextMembers[name]=bytes end
            local again=run(case.code,nextMembers,nil)
            assert(again.ok,'second pass failed: '..tostring(again.errorCode)..' '..tostring(again.error))
            assert(again.script==result.script and count(again.nativeMembers)==0,'same template after real native roundtrip must have no writes')
            row.secondPass={ok=true,nativeCount=0,scriptHex=hex(again.script),coverage=again.coverage}
        end
        return row
    end,debug.traceback)
    if not ok then failed=failed+1 end
    local row={caseId=case.id,passed=ok,readSet=readSet,mapReads=mapReads,messages=messages,physicalReads=physicalReads,physicalBytes=physicalBytes}
    if ok then row.actual=actual else row.error=tostring(actual) end
    io.stdout:write(json(row),'\n'); io.stdout:flush(); collectgarbage('collect')
end
io.stdout:write(json{summary=true,caseCount=#cases,failedExpectations=failed,luaVersion=_VERSION,coverage='OriginalRestrictedWholeChunkTransactions'},'\n')
io.stdout:flush(); os.exit(failed==0 and 0 or 1)
