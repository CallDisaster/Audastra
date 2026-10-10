-- 原创语义夹具；不是作者地图或模板。只使用显式传入的string.rep生成有界长串。
local base = {
    _id='P001', _type='ability', _code='B001', _max_level=3, levels=3,
    name='default', dataa={1.0}, tip={'base','second'}, app={'app'},
    enabled=1, pair_1=0, pair_2=0, only=0.0,
}
local equalOld = {
    _id='Q001', _parent='P001', _type='ability', _code='B001', _obj=true,
    w2lobject='static', _max_level=3, levels=3, name='default',
    dataa={1.0,1.0,1.0}, tip={'base','second','second'}, app={'app','app','app'},
    enabled=1, pair_1=0, pair_2=0, only=0.0,
}
local function meta(field, id, tp, repeated, profile)
    return {field=field,id=id,key=field:lower(),type=tp,['repeat']=repeated,profile=profile}
end
local function new(id) return {kind='new',parent='P001',id=id or 'Q001'} end
local function write(field,value) return {kind='write',field=field,value=value} end
local function at(step,path,kind,value) return {step=step,path=path,kind=kind,value=value} end
local function obj(step,field,kind,value) return at(step,{'objects','ability','Q001',field},kind,value) end
local function element(step,field,index,kind,value) return at(step,{'objects','ability','Q001',field,index},kind,value) end
local function patch(scope,path,value) return {scope=scope,path=path,value=value} end
local function idx(index)
    local name='dataa_'..index
    return patch('metadata',{'ability',name},meta(name,'Dx0'..index,0))
end
return {
    schema='slk-creation-original-inputs-v1',
    defaults={ability={P001=base,P002={_id='P002',_type='ability',_code='B001',_max_level=1,levels=1,name='standard-two'}}},
    old={ability={P001={_id='P001',_type='ability',_code='B001',name='map-parent-edit'},
        P002={_id='P002',_type='ability',_code='B001',name='standard-two'}}},
    metadata={ability={
        name=meta('Name','Name',3), levels=meta('Levels','alev',0),
        dataa=meta('DataA','Dcom',2,4,false), tip=meta('Tip','Atip',3,0,true),
        app={field='App',id='Aapp',key='app',type=3,['repeat']=4,profile=true,appendindex=true},
        pair_1={field='Pair_1',id='Pa01',key='pair_1',type=0,index=1},
        pair_2={field='Pair_2',id='Pa02',key='pair_2',type=0,index=2},
        enabled=meta('Enabled','Aena',0), w2lobject=meta('W2LObject','W2lo',3),
    }, B001={dataa=meta('DataA','Dspe',2,4,true),tip=meta('Tip','Asca',3),only=meta('Only','Aone',2)}},
    cases={
        {id='initial_new',cppPolicy='supported',ops={new()},save=1,remove=0,errors=0,checks={
            obj(1,'_parent','string','P001'),obj(1,'_create','boolean',true),obj(1,'w2lobject','string','static'),
            obj(1,'_code','string','B001'),element(1,'dataa',3,'float',1.0),element(1,'app',3,'string','app'),
            obj(1,'name','string','default'),at(1,{'objects','ability','P001','name'},'string','map-parent-edit'),
        }},
        {id='scalar_types',cppPolicy='supported',ops={new(),write('DataA1',1.0),write('DataA2',7),write('DataA3',true),
            write('Enabled',false),write('Name','TRIGSTR_7'),write('Pair',8),write('Pair_2',9),write('Only',0.1),write('Tip','changed')},save=1,remove=0,errors=0,checks={
            element(2,'dataa',1,'float',1.0),element(3,'dataa',2,'integer',7),element(4,'dataa',3,'boolean',true),
            obj(5,'enabled','boolean',false),obj(6,'name','string','TRIGSTR_7'),obj(7,'pair_1','integer',8),
            obj(8,'pair_2','integer',9),obj(9,'only','float',0.1),element(10,'tip',1,'string','changed'),
        }},
        {id='drop_then_grow',cppPolicy='supported',ops={new(),write('Tip2','custom'),write('Levels',2),write('Levels',4)},save=1,remove=0,errors=0,checks={
            element(2,'tip',3,'string','second'),{step=3,path={'objects','ability','Q001','tip',3},absent=true},
            element(4,'tip',3,'string','custom'),element(4,'tip',4,'string','custom'),
        }},
        {id='grow_without_drop',cppPolicy='supported',ops={new(),write('Tip2','custom'),write('Levels',4)},save=1,remove=0,errors=0,checks={element(3,'tip',4,'string','second')}},
        {id='common_fill_first',cppPolicy='supported',ops={new(),write('Levels',1),write('DataA1',9.0),write('Levels',4)},save=1,remove=0,errors=0,checks={
            element(4,'dataa',1,'float',9.0),element(4,'dataa',4,'float',1.0),
        }},
        {id='zero_and_regrow',cppPolicy='supported',ops={new(),write('Levels',0),write('Levels',3)},save=1,remove=0,errors=0,checks={
            obj(2,'_max_level','integer',0),{step=2,path={'objects','ability','Q001','dataa'},kind='table',count=0},element(3,'dataa',3,'float',1.0),
        }},
        {id='dense_extra_trimmed',cppPolicy='supported',ops={new(),write('DataA4',8),write('Levels',2),write('Levels',4)},save=1,remove=0,errors=0,checks={
            element(2,'dataa',4,'integer',8),element(4,'dataa',4,'float',1.0),
        }},
        {id='indexed_gap_repeat',cppPolicy='reject',patches={idx(1),idx(3),patch('defaults',{'ability','P001','dataa_1'},0),patch('defaults',{'ability','P001','dataa_3'},0)},
            ops={new(),write('DataA_3',7)},save=1,remove=0,errors=0,checks={obj(2,'dataa','integer',7),obj(2,'dataa_3','integer',0)}},
        {id='indexed_continuous',cppPolicy='supported',patches={idx(1),idx(2),idx(3),patch('defaults',{'ability','P001','dataa_1'},0),
            patch('defaults',{'ability','P001','dataa_2'},0),patch('defaults',{'ability','P001','dataa_3'},0)},
            ops={new(),write('DataA_3',7)},save=1,remove=0,errors=0,checks={obj(2,'dataa_3','integer',7),element(2,'dataa',1,'float',1.0)}},
        {id='indexed_gap_fallback',cppPolicy='supported',patches={idx(1),idx(3),patch('defaults',{'ability','P001','dataa_1'},0),
            patch('defaults',{'ability','P001','dataa_3'},0),{scope='metadata',path={'ability','dataa','repeat'},remove=true}},
            ops={new(),write('DataA_3',7)},save=1,remove=0,errors=0,checks={obj(2,'dataa_3','integer',7),element(2,'dataa',1,'float',1.0)}},
        {id='old_generated_equal',cppPolicy='supported',patches={patch('old',{'ability','Q001'},equalOld)},ops={new()},save=0,remove=0,errors=0,checks={
            {step=0,path={'objects','ability','Q001'},absent=true},obj(1,'_create','boolean',true),
        }},
        {id='old_generated_changed',cppPolicy='supported',patches={patch('old',{'ability','Q001'},equalOld),patch('old',{'ability','Q001','name'},'old-name')},
            ops={new()},save=1,remove=0,errors=0,checks={obj(1,'name','string','default')}},
        {id='old_generated_removed',cppPolicy='supported',patches={patch('old',{'ability','Q001'},equalOld)},ops={},save=0,remove=1,errors=0,checks={
            {step=0,path={'objects','ability','Q001'},absent=true},
        }},
        {id='ordinary_collision',cppPolicy='reject',patches={patch('old',{'ability','Q001'},{_id='Q001',_type='ability',_code='B001',_parent='P001',_obj=true,name='kept'})},
            ops={new(),write('Name','changed')},save=0,remove=0,errors=1,checks={obj(2,'name','string','kept'),{step=2,path={'objects','ability','Q001','w2lobject'},absent=true}}},
        {id='standard_collision',cppPolicy='reject',ops={new('P002'),write('Name','changed')},save=0,remove=0,errors=1,checks={
            at(2,{'objects','ability','P002','name'},'string','standard-two'),
        }},
        {id='long_string_truncated',cppPolicy='reject',ops={new(),write('Name',string.rep('x',1024))},save=1,remove=0,errors=1,checks={
            {step=2,path={'objects','ability','Q001','name'},kind='string',bytes=1023},
        }},
        {id='sparse_level',cppPolicy='reject',ops={new(),write('DataA5',9)},save=1,remove=0,errors=0,checks={
            element(2,'dataa',5,'integer',9),{step=2,path={'objects','ability','Q001','dataa',4},absent=true},
        }},
        {id='unknown_field',cppPolicy='reject',ops={new(),write('FooBar',3)},save=1,remove=0,errors=0,checks={obj(2,'FooBar','integer',3)}},
        {id='floating_levels',cppPolicy='reject',ops={new(),write('Levels',2.0)},save=1,remove=0,errors=0,checks={
            obj(2,'levels','float',2.0),obj(2,'_max_level','float',2.0),{step=2,path={'objects','ability','Q001','dataa',3},absent=true},
        }},
        {id='false_profile_default',cppPolicy='supported',patches={patch('defaults',{'ability','P001','tip'},{'first',false})},
            ops={new(),write('Levels',1),write('Levels',3)},save=1,remove=0,errors=0,checks={element(3,'tip',2,'string','first'),element(3,'tip',3,'string','first')}},
    },
}
