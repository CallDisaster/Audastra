-- 原创完整模板与地图成员；无私图内容、第三方实现或真实资源副本。
local pack=string.pack
local function w3i()
    return pack('<i4i4i4',25,1,6059)..'Transaction oracle\0Original\0Whole chunk\0\0'
        ..pack('<ffffffff',0,0,1024,0,1024,1024,0,1024)
        ..pack('<i4i4i4i4i4i4I4',0,0,0,0,8,8,0)..'L'..pack('<i4',-1)..'\0\0\0\0'
        ..pack('<i4',1)..'\0\0\0\0'..pack('<i4fffBBBB',0,0,3000,0,0,0,0,0)
        ..'\0\0\0\0\0L'..pack('<BBBB',255,255,255,255)..pack('<i4i4i4i4i4i4',0,0,0,0,0,0)..'\255'
end
local function oldUnit()
    return pack('<i4i4i4',2,0,1)..'hfooZ009'..pack('<i4',2)
        ..'unam'..pack('<i4',3)..'Original obsolete unit\0Z009'
        ..'W2lo'..pack('<i4',3)..'static\0Z009'
end
local function item(id,code,expect)
    local value={id=id,code=code,members={['war3map.w3i']=w3i()},expect=expect}
    return value
end
local changed=item('wholechunk-new-fill-repeat-require',
    "<? local s=require 'slk'; local o=s.ability.AHhb:new('Z101'); o.Name='Original created ability'; o.Levels=1; o.DataA1=17.0 ?>HEAD:<?= o:get_id() ?><? local same=require 'slk'; same.ability.Z101.Levels=3; assert(o.Levels==3 and o.DataA1==17.0 and o.DataA2==s.ability.AHhb.DataA2 and o.DataA3==s.ability.AHhb.DataA3) ?>:TAIL",
    {ok=true,script='HEAD:Z101:TAIL',members={'war3map.w3a'},roundtrip=true})
changed.members['war3mapmisc.txt']='[Misc]\nGoldTextHeight=0.125\n'
local removed=item('other-class-generated-residual-delete',"<? local s=require 'slk' ?>unchanged script",
    {ok=true,script='unchanged script',members={'war3map.w3u'},emptyNative='war3map.w3u'})
removed.members['war3map.w3u']=oldUnit()
local scoped=item('cross-block-control-and-local',
    '<? local ok,n=assert(true,3); for i=1,n do ?><?= i ?><? end ?>',
    {ok=true,script='123',members={}})
local nochange=item('require-without-object-effects',"<? local a=require 'slk'; local b=require 'slk'; assert(a==b) ?>no changes",
    {ok=true,script='no changes',members={}})
local cases={changed,removed,scoped,nochange}
cases[#cases+1]=item('byte-positive-and-negative-valid-boundaries',
    "<? local a={string.byte('AB',1,-1)}; local b={string.byte(string.rep('x',4096),1,4096)}; local c={string.byte('AB',-9223372036854775807-1,9223372036854775807)} ?><?= #a ?>:<?= #b ?>:<?= #c ?>",
    {ok=true,script='2:4096:2',members={}})
local locale=item('existing-locale-member-denied',"<? require 'slk' ?>",
    {ok=false,errorCode='TEMPLATE_METADATA_UNSUPPORTED'})
locale.members['w3x2lni\\locale\\w3i.lng']=''
cases[#cases+1]=locale
local bad={
    {'duplicate-id-accumulated-error',"local s=require 'slk'; s.ability.AHhb:new('Z101'); s.ability.AHhb:new('Z101')",'TEMPLATE_TOOL_ERRORS'},
    {'target-string-truncation-error',"local o=require('slk').ability.AHhb:new('Z101'); o.Name=string.rep('x',1024)",'TEMPLATE_TOOL_ERRORS'},
    {'unknown-global',"os.execute('never executed')",'TEMPLATE_CAPABILITY_DENIED'},
    {'import-denied',"import('fixture.txt')('never written')",'TEMPLATE_CAPABILITY_DENIED'},
    {'early-refresh-denied',"require('slk'):refresh()",'TEMPLATE_CAPABILITY_DENIED'},
    {'readonly-read-array-write',"local o=require('slk').ability.AHhb:new('Z101'); local t=o.DataA_1; t[1]=999",'TEMPLATE_CAPABILITY_DENIED'},
    {'table-insert-protected-array',"local o=require('slk').ability.AHhb:new('Z101'); table.insert(o.DataA_1,999)",'TEMPLATE_CAPABILITY_DENIED'},
    {'table-insert-protected-library',"table.insert(require('slk'),123)",'TEMPLATE_CAPABILITY_DENIED'},
    {'string-method-dump-denied',"local value=('x').dump",'TEMPLATE_CAPABILITY_DENIED'},
    {'string-method-allocation-bound',"local value=('x'):rep(1000000000)",'TEMPLATE_LIMIT'},
    {'string-byte-positive-result-bound',"local t={string.byte(string.rep('ab',4096),1,8192)}",'TEMPLATE_LIMIT'},
    {'string-byte-negative-result-bound',"local t={string.byte(string.rep('ab',4096),1,-1)}",'TEMPLATE_LIMIT'},
    {'unknown-field-denied',"local o=require('slk').ability.AHhb:new('Z101'); o.UnknownFixtureField=1",'TEMPLATE_FIELD_UNSUPPORTED'},
    {'partial-effects-then-error',"local o=require('slk').ability.AHhb:new('Z101'); o.Name='pending'; error('original failure')",'TEMPLATE_ERROR'},
    {'metatable-denied',"getmetatable('x')",'TEMPLATE_CAPABILITY_DENIED'},
    {'debug-denied',"debug.getupvalue(function() end,1)",'TEMPLATE_CAPABILITY_DENIED'},
    {'instruction-bound',"while true do end",'TEMPLATE_LIMIT'},
}
for _,entry in ipairs(bad) do
    local value=item(entry[1],'<? '..entry[2]..' ?>',{ok=false,errorCode=entry[3]})
    if entry[1]=='instruction-bound' then value.limits={instructions=10000} end
    cases[#cases+1]=value
end
-- 大数据只在执行fixture时由原创字符构造；报告不把它们展开成hex。
local literal=string.rep('L',8*1024*1024)..'...END'
cases[#cases+1]=item('large-static-literal-eight-mib',literal,
    {ok=true,script=literal,members={},compactScript=true,creations=0})
local prefix,suffix=string.rep('A',4*1024*1024),string.rep('B',4*1024*1024)
local largeCode=prefix.."<? local o=require('slk').ability.AHhb:new('Z102'); o.Name='Original large-script ability' ?>"
    ..string.rep('<?= o:get_id() ?>',1300).."<? assert(o.Name=='Original large-script ability') ?>"..suffix
cases[#cases+1]=item('large-wholechunk-thirteen-hundred-expressions',largeCode,
    {ok=true,script=prefix..string.rep('Z102',1300)..suffix,members={'war3map.w3a'},compactScript=true,creations=1})
local guards={
    {'early-return-denied','<? do return {} end ?>'},
    {'shadowed-final-concat-denied','<? local table={concat=function() end} ?>text'},
    {'reserved-sink-name-denied','<? local __FORGE_OUTPUT_SINK=1 ?>'},
    {'template-varargs-denied','<? local x=... ?>'},
    {'ordinary-concat-stays-one-mib',"<? local s=string.rep(string.rep('x',4096),256); local x=table.concat({s,'x'}) ?>",'TEMPLATE_LIMIT'},
    {'whole-output-budget','OUTPUT', 'TEMPLATE_LIMIT',{scriptBytes=5}},
    {'generated-chunk-budget','OUTPUT','TEMPLATE_LIMIT',{chunkBytes=64}},
}
for _,entry in ipairs(guards) do
    local value=item(entry[1],entry[2],{ok=false,errorCode=entry[3] or 'TEMPLATE_CAPABILITY_DENIED'})
    value.limits=entry[4]
    cases[#cases+1]=value
end
return cases
