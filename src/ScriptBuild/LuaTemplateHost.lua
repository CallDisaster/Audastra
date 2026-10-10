-- 原创、受限的 wholechunk 模板宿主。可信工具模块由调用者固定身份并独立加载。
-- 本文件不读取文件、不启动进程、不写地图；Run 的失败结果不含任何部分输出。
local Host = {schema='forge-lua-template-host-v1'}
local hard = {inputBytes=16*1024*1024, scriptBytes=16*1024*1024, chunkBytes=64*1024*1024, stringBytes=1024*1024,
    memberBytes=16*1024*1024, totalMemberBytes=64*1024*1024, operations=65536,
    creations=4096, instructions=100000000, tableItems=4096, levels=256, heapBytes=512*1024*1024}
local nativeNames = {['war3map.w3u']=true,['war3map.w3a']=true,['war3map.w3t']=true,
    ['war3map.w3h']=true,['war3map.w3q']=true,['war3map.w3d']=true,['war3map.w3b']=true}
local classes = {unit=true,ability=true,item=true,buff=true,upgrade=true,doodad=true,destructable=true}
local localeNames = {'w3x2lni\\locale\\w3i.lng','w3x2lni\\locale\\lml.lng'}
local rawString, rawTable, rawMath = string, table, math
local rawType, rawPairs, rawNext, rawError, rawPcall = type, pairs, next, error, pcall
local rawPack, rawUnpack = table.pack, table.unpack
local rawLoad = load
local rawGetMeta, rawSetMeta = debug.getmetatable, debug.setmetatable
local function finite(v) return v==v and v~=math.huge and v~=-math.huge end

function Host.Run(request, trusted)
    local firstError, session, environment, safeStringMeta
    local createdIds={}
    local originalStringMeta = rawGetMeta('')
    local previousHook, previousMask, previousCount = debug.gethook()
    local protected = setmetatable({}, {__mode='k'})
    local limits, stats, bundle = {}, {operations=0,creations=0,instructions=0,outputRequests=0,metadataRequests=0,
        templateLoads=0,templateExecutions=0,outputSinkCalls=0}, {}
    local memberBytes, outputLog, metadataLog = 0, {}, {}
    local function reject(code, message)
        if not firstError then firstError={code=code,message=message} end
        rawError(code..': '..message,0)
    end
    local function spend(kind, count, maximum)
        if count > maximum-stats[kind] then reject('TEMPLATE_LIMIT',kind..' budget exceeded') end
        stats[kind]=stats[kind]+count
    end
    local function text(value, maximum)
        if rawType(value)~='string' or #value>maximum then reject('TEMPLATE_LIMIT','string outside supported bounds') end
        return value
    end
    local function ordinary(value)
        if rawType(value)~='table' or protected[value] or rawGetMeta(value)~=nil then
            reject('TEMPLATE_CAPABILITY_DENIED','table operation requires an ordinary template table')
        end
        if #value>limits.tableItems then reject('TEMPLATE_LIMIT','template array too large') end
        return value
    end
    local function scalar(value)
        local kind=rawType(value)
        if kind=='number' then if not finite(value) then reject('TEMPLATE_VALUE_UNSUPPORTED','nonfinite number') end
        elseif kind=='string' then text(value,limits.stringBytes)
        elseif kind~='boolean' then reject('TEMPLATE_VALUE_UNSUPPORTED','only scalar SLK assignment is supported') end
        return value
    end
    -- 可信工具（frontend 读整图、refresh 写出）不计入模板指令/堆采样预算；
    -- 整个进程仍受外层 Job 内存与超时限制。工具调用不会回调模板代码。
    local function trustedCall(fn,...)
        local previous=rawGetMeta('')
        local hook,mask,count=debug.gethook()
        debug.sethook()
        rawSetMeta('',originalStringMeta)
        local result=rawPack(rawPcall(fn,...))
        rawSetMeta('',previous)
        debug.sethook(hook,mask,count)
        if not result[1] then rawError(result[2],0) end
        return rawUnpack(result,2,result.n)
    end
    local function readonly(source, depth)
        if rawType(source)~='table' then
            if rawType(source)=='function' or rawType(source)=='userdata' or rawType(source)=='thread' then
                reject('TEMPLATE_CAPABILITY_DENIED','tool capability cannot be exposed as a value')
            end
            if source~=nil then scalar(source) end
            return source
        end
        if depth>4 then reject('TEMPLATE_LIMIT','read value nesting') end
        local count=trustedCall(function()
            local seen=0
            for _ in rawPairs(source) do
                seen=seen+1
                if seen>limits.tableItems then break end
            end
            return seen
        end)
        if count>limits.tableItems then reject('TEMPLATE_LIMIT','read table size') end
        local proxy={}; protected[proxy]=true
        return setmetatable(proxy,{
            __index=function(_,key)
                if rawType(key)=='string' then text(key,256)
                elseif rawMath.type(key)~='integer' or key<1 or key>limits.tableItems then
                    reject('TEMPLATE_VALUE_UNSUPPORTED','read-only key outside bounds')
                end
                spend('operations',1,limits.operations)
                return readonly(trustedCall(function() return source[key] end),depth+1)
            end,
            __newindex=function() reject('TEMPLATE_CAPABILITY_DENIED','tool tables are read-only; use object field assignment') end,
            __len=function() return trustedCall(function() return #source end) end,
            __metatable=false,
        })
    end
    local function frozen(entries)
        local proxy={}; protected[proxy]=true
        return setmetatable(proxy,{
            __index=function(_,key)
                local value=entries[key]
                if value==nil then reject('TEMPLATE_CAPABILITY_DENIED','unsupported library entry '..tostring(key)) end
                return value
            end,
            __newindex=function() reject('TEMPLATE_CAPABILITY_DENIED','library is read-only') end,
            __metatable=false,
        })
    end
    local function output(operation,name,bytes)
        if rawType(name)~='string' then reject('TEMPLATE_OUTPUT_INVALID','output name must be a string') end
        name=rawString.lower(rawString.gsub(name,'/','\\'))
        if name=='w3x2lni\\locale\\w3i.lng' or name=='w3x2lni\\locale\\lml.lng' then
            if operation~='remove' then reject('TEMPLATE_OUTPUT_INVALID','unexpected locale write') end
            -- ensureSession 已对不可变输入证实不存在；仍把目标清理请求显式记入回执。
            stats.metadataRequests=stats.metadataRequests+1
            if stats.metadataRequests>4 then reject('TEMPLATE_LIMIT','metadata request count') end
            metadataLog[#metadataLog+1]={operation=operation,name=name,inputPresent=false,disposition='AbsentInputNoOp'}
            return
        end
        if not nativeNames[name] then reject('TEMPLATE_OUTPUT_INVALID','output outside seven native members: '..name) end
        if operation=='remove' then bytes=rawString.pack('<i4i4i4',2,0,0)
        elseif operation~='set' then reject('TEMPLATE_OUTPUT_INVALID','unknown output operation') end
        text(bytes,limits.memberBytes)
        if memberBytes+#bytes>limits.totalMemberBytes then reject('TEMPLATE_LIMIT','cumulative native output bytes') end
        memberBytes=memberBytes+#bytes
        stats.outputRequests=stats.outputRequests+1
        if stats.outputRequests>32 then reject('TEMPLATE_LIMIT','native output request count') end
        bundle[name]=bytes
        outputLog[#outputLog+1]={operation=operation,name=name,bytes=#bytes}
    end
    local function checkErrors()
        local errors=trustedCall(session.errors)
        if rawType(errors)~='table' then reject('TEMPLATE_TOOL_IDENTITY','target errors upvalue is not a table') end
        local count=0
        for key,value in rawPairs(errors) do
            count=count+1
            if count>256 or (rawType(key)~='number' and rawType(key)~='string') or
                (rawType(value)~='string' and rawType(value)~='table') then
                reject('TEMPLATE_TOOL_IDENTITY','unexpected target error structure')
            end
            if rawType(value)=='table' then
                local entries=0
                for index,message in rawPairs(value) do
                    entries=entries+1
                    if rawMath.type(index)~='integer' or index<1 or entries>256 or rawType(message)~='string' or #message>65536 then
                        reject('TEMPLATE_TOOL_IDENTITY','unexpected target error item')
                    end
                end
            end
        end
        if count>0 then reject('TEMPLATE_TOOL_ERRORS','target SLK accumulated semantic errors') end
    end
    local function ensureSession()
        if session then return end
        if rawType(trusted.inputMemberExists)~='function' then reject('TEMPLATE_PROTOCOL','trusted immutable input membership callback missing') end
        for _,name in ipairs(localeNames) do
            local present=trustedCall(trusted.inputMemberExists,name)
            if rawType(present)~='boolean' then reject('TEMPLATE_TOOL_IDENTITY','input membership must be known') end
            if present then reject('TEMPLATE_METADATA_UNSUPPORTED','existing locale member cannot be preserved by seven-native bundle: '..name) end
        end
        session=trustedCall(trusted.createSession,output)
        for _,name in ipairs {'refresh','errors','hasStandard','validateWrite'} do
            if rawType(session[name])~='function' then reject('TEMPLATE_TOOL_IDENTITY','missing trusted session operation '..name) end
        end
        if rawType(session.rawSlk)~='table' then reject('TEMPLATE_TOOL_IDENTITY','missing raw SLK proxy') end
        checkErrors()
    end
    local function rawId(id)
        if rawType(id)~='string' or #id~=4 or not id:match('^[A-Za-z0-9]+$') then
            reject('TEMPLATE_ID_UNSUPPORTED','this host supports four ASCII alphanumeric IDs only')
        end
        return id
    end
    local function objectProxy(rawObject,class,id,created,parent)
        local proxy={}; protected[proxy]=true
        return setmetatable(proxy,{
            __index=function(_,key)
                if key=='new' then return function(self,newId)
                    if self~=proxy then reject('TEMPLATE_CAPABILITY_DENIED','invalid new receiver') end
                    if created or not trustedCall(session.hasStandard,class,id) then
                        reject('TEMPLATE_PARENT_UNSUPPORTED','new requires original standard parent')
                    end
                    rawId(newId); spend('creations',1,limits.creations)
                    local nextObject=trustedCall(function() return rawObject:new(newId) end)
                    checkErrors()
                    if rawType(nextObject)~='table' then reject('TEMPLATE_TOOL_ERRORS','new did not return an object') end
                    createdIds[class..':'..newId]=id
                    return objectProxy(nextObject,class,newId,true,id)
                end end
                if key=='get_id' then return function(self)
                    if self~=proxy then reject('TEMPLATE_CAPABILITY_DENIED','invalid get_id receiver') end
                    return id
                end end
                if rawType(key)~='string' or #key>256 or key:sub(1,1)=='_' or key=='permanent' then
                    reject('TEMPLATE_CAPABILITY_DENIED','unsupported object member')
                end
                spend('operations',1,limits.operations)
                return readonly(trustedCall(function() return rawObject[key] end),0)
            end,
            __newindex=function(_,key,value)
                if not created then reject('TEMPLATE_CAPABILITY_DENIED','direct writes to existing objects are unsupported') end
                if rawType(key)~='string' or #key>256 or not key:match('^[A-Za-z][A-Za-z0-9_]*$') then
                    reject('TEMPLATE_FIELD_UNSUPPORTED','invalid field name')
                end
                scalar(value); spend('operations',1,limits.operations)
                local field,level=trustedCall(session.validateWrite,class,parent,key)
                if rawType(field)~='string' or field:sub(1,1)=='_' or field=='w2lobject' or field=='code' then
                    reject('TEMPLATE_FIELD_UNSUPPORTED','field is not backed by target metadata')
                end
                if level and (rawMath.type(level)~='integer' or level<1 or level>limits.levels) then
                    reject('TEMPLATE_LIMIT','field level outside bounds')
                end
                if field=='levels' or field=='maxlevel' or field=='numvar' then
                    if rawMath.type(value)~='integer' or value<0 or value>limits.levels then reject('TEMPLATE_LIMIT','maximum level outside bounds') end
                end
                trustedCall(function() rawObject[key]=value end)
                checkErrors()
            end,
            __metatable=false,
        })
    end
    local library, classCache = {}, {}
    protected[library]=true
    setmetatable(library,{
        __index=function(_,class)
            -- misc 的组名不是四字节对象ID，且不属于七类可写 native。
            -- 只读同一已冻结 frontend 的实际数据，绝不补默认值或允许 refresh 写 misc。
            if class=='misc' then
                ensureSession()
                if not classCache.misc then
                    classCache.misc=readonly(trustedCall(function() return session.rawSlk.misc end),0)
                end
                return classCache.misc
            end
            if not classes[class] then reject('TEMPLATE_CAPABILITY_DENIED','SLK class/method not supported: '..tostring(class)) end
            ensureSession()
            if not classCache[class] then
                local proxy={}; protected[proxy]=true
                classCache[class]=setmetatable(proxy,{
                    __index=function(_,id)
                        rawId(id); spend('operations',1,limits.operations)
                        local obj=trustedCall(function() return session.rawSlk[class][id] end)
                        local parent=createdIds[class..':'..id]
                        return objectProxy(obj,class,id,parent~=nil,parent)
                    end,
                    __newindex=function() reject('TEMPLATE_CAPABILITY_DENIED','cannot replace SLK objects') end,
                    __metatable=false,
                })
            end
            return classCache[class]
        end,
        __newindex=function() reject('TEMPLATE_CAPABILITY_DENIED','cannot replace SLK classes or methods') end,
        __metatable=false,
    })
    local ok,result=rawPcall(function()
        if previousHook then reject('TEMPLATE_TOOL_IDENTITY','host requires exclusive Lua state without existing debug hook') end
        if rawType(request)~='table' or request.schema~='forge-lua-template-request-v1' then reject('TEMPLATE_PROTOCOL','request schema mismatch') end
        for key,value in rawPairs(hard) do limits[key]=value end
        for key,value in rawPairs(request.limits or {}) do
            if not hard[key] or rawMath.type(value)~='integer' or value<1 or value>hard[key] then reject('TEMPLATE_LIMIT','only tighter positive budgets are allowed') end
            limits[key]=value
        end
        text(request.preprocessed,limits.inputBytes)
        stats.inputBytes=#request.preprocessed
        local position=1
        while true do
            local opening=rawString.find(request.preprocessed,'<?',position,true)
            if not opening then break end
            local closing=rawString.find(request.preprocessed,'?>',opening+2,true)
            if not closing then reject('TEMPLATE_SYNTAX','unclosed template block') end
            -- 固定编译器的私有输出能力通过局部参数传入。保守拒绝块内这些字节，
            -- 包括字符串/注释；静态 JASS 文本不受影响。不能靠隐藏名字充当权限边界。
            local block=rawString.sub(request.preprocessed,opening+2,closing-1)
            if rawString.find(block,'__FORGE_OUTPUT_SINK',1,true) or rawString.find(block,'...',1,true) then
                reject('TEMPLATE_CAPABILITY_DENIED','reserved output identifier or varargs in template block')
            end
            position=closing+2
        end
        if rawType(trusted)~='table' or rawType(trusted.createSession)~='function' or rawType(trusted.compileWhole)~='function' then
            reject('TEMPLATE_PROTOCOL','trusted callbacks missing')
        end
        if trusted.compileChunkProtocol~='forge-guarded-template-load-v1' then
            reject('TEMPLATE_PROTOCOL','trusted compiler must use guarded template load protocol')
        end
        local safeStrings={}
        for _,name in ipairs {'len','sub','lower','upper','reverse','byte'} do
            local fn=rawString[name]
            safeStrings[name]=function(value,...)
                text(value,limits.stringBytes)
                if name=='byte' then
                    local args=rawPack(...)
                    local first=args[1]==nil and 1 or args[1]
                    local last=args[2]==nil and first or args[2]
                    if rawMath.type(first)~='integer' or rawMath.type(last)~='integer' then reject('TEMPLATE_LIMIT','string.byte integer indices required') end
                    -- 按 Lua 相对索引语义先归一化；避免负末索引和 int64 相减溢出绕过数量限额。
                    local length=#value
                    if first<0 then first=first < -length and 0 or length+first+1 end
                    if last<0 then last=last < -length and 0 or length+last+1 end
                    first=rawMath.max(first,1); last=rawMath.min(last,length)
                    if first<=last and last-first+1>limits.tableItems then reject('TEMPLATE_LIMIT','string.byte result count') end
                end
                return fn(value,...)
            end
        end
        safeStrings.find=function(value,pattern,init,plain)
            text(value,limits.stringBytes); text(pattern,256)
            if plain~=true then reject('TEMPLATE_CAPABILITY_DENIED','only plain string.find is supported') end
            return rawString.find(value,pattern,init,true)
        end
        safeStrings.match=function(value,pattern)
            text(value,256); text(pattern,128)
            -- 仅宿主本身用到的 ID/字段形状；模板的任意模式明确拒绝。
            if pattern~='^[A-Za-z0-9]+$' and pattern~='^[A-Za-z][A-Za-z0-9_]*$' then reject('TEMPLATE_CAPABILITY_DENIED','string pattern not supported') end
            return rawString.match(value,pattern)
        end
        safeStrings.rep=function(value,n,separator)
            text(value,limits.stringBytes); separator=separator or ''; text(separator,limits.stringBytes)
            if rawMath.type(n)~='integer' or n<0 or n>limits.tableItems or (#value+#separator)*n>limits.stringBytes then reject('TEMPLATE_LIMIT','string.rep allocation') end
            return rawString.rep(value,n,separator)
        end
        safeStrings.format=function(format,...)
            text(format,4096)
            local args=rawPack(...); if args.n>64 then reject('TEMPLATE_LIMIT','format argument count') end
            local estimate=#format
            for i=1,args.n do
                local value=args[i]; scalar(value)
                estimate=estimate+(rawType(value)=='string' and #value*4+16 or 256)
            end
            for width in rawString.gmatch(format,'%%[-+ #0]*(%d+)') do
                if tonumber(width)>65536 then reject('TEMPLATE_LIMIT','format width') end
                estimate=estimate+tonumber(width)
            end
            for precision in rawString.gmatch(format,'%%[-+ #0]*%d*%.(%d+)') do
                if tonumber(precision)>65536 then reject('TEMPLATE_LIMIT','format precision') end
                estimate=estimate+tonumber(precision)
            end
            if estimate>limits.stringBytes then reject('TEMPLATE_LIMIT','format allocation') end
            return text(rawString.format(format,...),limits.stringBytes)
        end
        local safeTable={
            concat=function(t,separator,first,last)
                ordinary(t); separator=separator or ''; text(separator,limits.stringBytes)
                first=first or 1; last=last or #t
                if rawMath.type(first)~='integer' or rawMath.type(last)~='integer' or first<1 or last>limits.tableItems then reject('TEMPLATE_LIMIT','concat range') end
                local bytes=0
                for i=first,last do
                    local value=t[i]
                    if rawType(value)~='string' and rawType(value)~='number' then reject('TEMPLATE_VALUE_UNSUPPORTED','concat expects scalar text') end
                    bytes=bytes+#tostring(value)+#separator
                    if bytes>limits.stringBytes then reject('TEMPLATE_LIMIT','concat allocation') end
                end
                return rawTable.concat(t,separator,first,last)
            end,
            insert=function(t,...)
                ordinary(t); if #t>=limits.tableItems then reject('TEMPLATE_LIMIT','table.insert size') end
                return rawTable.insert(t,...)
            end,
            remove=function(t,pos) ordinary(t); return rawTable.remove(t,pos) end,
        }
        local safeMath={pi=rawMath.pi}
        for _,name in ipairs {'abs','ceil','floor','max','min','modf','sqrt','sin','cos','tan','type','tointeger'} do safeMath[name]=rawMath[name] end
        safeMath.log=function(value,base)
            if rawType(value)~='number' or not finite(value) or value<=0 or
                (base~=nil and (rawType(base)~='number' or not finite(base) or base<=0 or base==1)) then
                reject('TEMPLATE_VALUE_UNSUPPORTED','math.log requires finite positive value and base other than one')
            end
            spend('operations',1,limits.operations)
            return scalar(rawMath.log(value,base))
        end
        local entries={
            string=frozen(safeStrings),table=frozen(safeTable),math=frozen(safeMath),
            type=rawType,tonumber=function(value,base) if rawType(value)=='string' then text(value,4096) end; return tonumber(value,base) end,
            tostring=function(value)
                if rawType(value)=='table' or rawType(value)=='function' then reject('TEMPLATE_CAPABILITY_DENIED','opaque value tostring unsupported') end
                return tostring(value)
            end,
            pairs=function(value) ordinary(value); return rawNext,value,nil end,
            ipairs=function(value) ordinary(value); return ipairs(value) end,
            assert=function(value,...)
                if not value then local message=(...); reject('TEMPLATE_ASSERT',rawType(message)=='string' and message or 'template assertion failed') end
                return value,...
            end,
            error=function(message) reject('TEMPLATE_ERROR',text(message,4096)) end,
            require=function(name)
                if name~='slk' then reject('TEMPLATE_CAPABILITY_DENIED','require only supports slk') end
                ensureSession(); return library
            end,
        }
        -- 能力回调必须不返回宿主表或函数；由可信适配器保证实际语义与同一会话绑定。
        for _,name in ipairs {'StringHash','abilityOrder'} do
            if trusted[name] then entries[name]=function(...)
                if name=='abilityOrder' then ensureSession() end
                local args=rawPack(...); for i=1,args.n do scalar(args[i]) end
                return scalar(trustedCall(trusted[name],session,rawUnpack(args,1,args.n)))
            end end
        end
        environment={}; protected[environment]=true
        local globals={}
        setmetatable(environment,{
            __index=function(_,key)
                if entries[key]~=nil then return entries[key] end
                if globals[key]~=nil then return globals[key] end
                reject('TEMPLATE_CAPABILITY_DENIED','unknown global '..tostring(key))
            end,
            __newindex=function(_,key,value)
                if rawType(key)~='string' or #key>256 or key:sub(1,1)=='_' or entries[key]~=nil then reject('TEMPLATE_CAPABILITY_DENIED','reserved global assignment') end
                globals[key]=value
            end,
            __metatable=false,
        })
        safeStringMeta={__index=frozen(safeStrings),__metatable=false}
        local function outputSink(concat,parts)
            if concat~=safeTable.concat then reject('TEMPLATE_CAPABILITY_DENIED','final output concat was shadowed') end
            if stats.outputSinkCalls~=0 then reject('TEMPLATE_CAPABILITY_DENIED','output sink can only be consumed once') end
            stats.outputSinkCalls=1
            ordinary(parts)
            local bytes=0
            for i=1,#parts do
                local value=parts[i]
                if rawType(value)~='string' and rawType(value)~='number' then reject('TEMPLATE_VALUE_UNSUPPORTED','output expects text or number') end
                local length=rawType(value)=='string' and #value or #tostring(value)
                if length>limits.scriptBytes-bytes then reject('TEMPLATE_LIMIT','whole script output bytes') end
                bytes=bytes+length
            end
            stats.outputParts=#parts; stats.scriptBytes=bytes
            return rawTable.concat(parts)
        end
        local function compileChunk(source,name,mode,env)
            if stats.templateLoads~=0 then reject('TEMPLATE_TOOL_IDENTITY','template chunk must be loaded once') end
            stats.templateLoads=1
            text(source,limits.chunkBytes)
            stats.generatedChunkBytes=#source
            local prefix='local __JASS__={} '
            local suffix='return table.concat(__JASS__)'
            if name~='@3_wave.j' or mode~='t' or env~=environment or
                rawString.sub(source,1,#prefix)~=prefix or rawString.sub(source,-#suffix)~=suffix then
                reject('TEMPLATE_TOOL_IDENTITY','fixed target generated chunk shape differs')
            end
            local prologue='local __FORGE_OUTPUT_SINK = ...; '
            local epilogue='return __FORGE_OUTPUT_SINK(table.concat,__JASS__)'
            if #source-#suffix+#prologue+#epilogue>limits.chunkBytes then reject('TEMPLATE_LIMIT','wrapped template chunk bytes') end
            -- 仅改固定生成器的最后一次输出；原模板块与静态 literal 保持原字节/顺序。
            local wrapped=prologue..rawString.sub(source,1,#source-#suffix)..epilogue
            stats.wrappedChunkBytes=#wrapped
            local fn,err=rawLoad(wrapped,name,mode,env)
            if not fn then return nil,err end
            return function()
                if stats.templateExecutions~=0 then reject('TEMPLATE_TOOL_IDENTITY','template chunk cannot execute twice') end
                stats.templateExecutions=1
                rawSetMeta('',safeStringMeta)
                local values=rawPack(rawPcall(fn,outputSink))
                rawSetMeta('',originalStringMeta)
                if not values[1] then rawError(values[2],0) end
                if stats.outputSinkCalls~=1 then reject('TEMPLATE_CAPABILITY_DENIED','early return bypassed final output sink') end
                return rawUnpack(values,2,values.n)
            end
        end
        debug.sethook(function()
            spend('instructions',1000,limits.instructions)
            if collectgarbage('count')*1024>limits.heapBytes then reject('TEMPLATE_LIMIT','Lua heap exceeds budget') end
        end,'',1000)
        -- 可信预编译用原字符串元表；只有实际模板执行切换受限元表，绝不预跑第二遍。
        local compiled,script,detail=trusted.compileWhole(request.preprocessed,environment,compileChunk)
        if not compiled then reject('TEMPLATE_EXECUTION_FAILED',rawType(detail)=='string' and detail:sub(1,4096) or 'target template compiler failed') end
        if stats.templateLoads~=1 or stats.templateExecutions~=1 or stats.outputSinkCalls~=1 then
            reject('TEMPLATE_TOOL_IDENTITY','trusted compiler did not consume the single guarded chunk')
        end
        text(script,limits.scriptBytes)
        if session then checkErrors(); trustedCall(session.refresh); checkErrors() end
        if firstError then rawError(firstError.code,0) end
        return {schema='forge-lua-template-result-v1',coverage='RestrictedWholeChunkAndSevenNativeBundle',
            metadataCoverage='AbsentLocaleCleanupOnly',metadataRequests=metadataLog,
            script=script,nativeMembers=bundle,memberOps=outputLog,stats=stats}
    end)
    -- finally：模板、目标库及预算异常都不能污染下一次可信调用的元表或hook。
    debug.sethook(previousHook,previousMask,previousCount)
    rawSetMeta('',originalStringMeta)
    if not ok then
        return {schema='forge-lua-template-result-v1',ok=false,errorCode=firstError and firstError.code or 'TEMPLATE_HOST_FAILED',
            error=firstError and firstError.message or tostring(result):sub(1,4096),coverage='RejectedNoBundle'}
    end
    result.ok=true
    return result
end
return Host
