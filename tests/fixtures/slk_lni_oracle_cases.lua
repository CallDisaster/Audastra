-- 原创数据；不是地图模板。由受限环境 loadfile 读取，所有被测文本仅交给 lni.classics。
return {
    { id = 'decimal_types', cppPolicy = 'supported', input = [=[
[P001]
integer=7
real=7.0
negativezero=-0.0
integerzero=-0
exponent=1e2
fraction=.5
trailingpoint=1.
leadingzero=0007
]=], ok = true, checks = {
        {path={'P001','integer'},kind='integer',value=7}, {path={'P001','real'},kind='float',value=7.0},
        {path={'P001','negativezero'},kind='float',negativeZero=true}, {path={'P001','integerzero'},kind='integer',value=0},
        {path={'P001','exponent'},kind='float',value=100.0}, {path={'P001','fraction'},kind='float',value=0.5},
        {path={'P001','trailingpoint'},kind='float',value=1.0}, {path={'P001','leadingzero'},kind='integer',value=7},
    }},
    { id = 'integer_bounds', cppPolicy = 'supported', input = [=[
[P001]
maximum=9223372036854775807
minimum=-9223372036854775808
exact=9007199254740993
rounded=9007199254740993.0
]=], ok=true, checks={
        {path={'P001','maximum'},kind='integer',value=9223372036854775807},
        {path={'P001','minimum'},kind='integer',value=-9223372036854775808},
        {path={'P001','exact'},kind='integer',value=9007199254740993},
        {path={'P001','rounded'},kind='float',value=9007199254740992.0},
    }},
    { id='quoted_strings', cppPolicy='supported', input=[=[
[P001]
double="a\tb\nc\r\\\"\'"
single='one\'two\"three\\'
utf8="原创类型样例"
empty=""
quotedcomment="fixture--data"
]=], ok=true, checks={
        {path={'P001','double'},kind='string',value='a\tb\nc\r\\"\''},
        {path={'P001','single'},kind='string',value='one\'two"three\\'},
        {path={'P001','utf8'},kind='string',value='原创类型样例'},
        {path={'P001','empty'},kind='string',value=''},
        {path={'P001','quotedcomment'},kind='string',value='fixture--data'},
    }},
    { id='dense_arrays', cppPolicy='supported', input=[=[
[P001]
yes=true
no=false
array={1,2.0,"",false}
empty={}
]=], ok=true, checks={
        {path={'P001','yes'},kind='boolean',value=true}, {path={'P001','no'},kind='boolean',value=false},
        {path={'P001','array'},kind='table',count=4}, {path={'P001','array',1},kind='integer',value=1},
        {path={'P001','array',2},kind='float',value=2.0}, {path={'P001','array',3},kind='string',value=''},
        {path={'P001','array',4},kind='boolean',value=false}, {path={'P001','empty'},kind='table',count=0},
    }},
    { id='metadata_layers', cppPolicy='supported', input=[=[
[ability]
[.dataa]
field=DataA
id=D001
key=dataa
type=0
repeat=4
profile=false
[P001]
[.dataa]
field=DataA
id=D002
key=dataa
type=2
[FontScope]
type=misc
[.height]
field=Height
id=H001
key=height
type=1
]=], ok=true, checks={
        {path={'ability','dataa','id'},kind='string',value='D001'},
        {path={'ability','dataa','repeat'},kind='integer',value=4},
        {path={'ability','dataa','profile'},kind='boolean',value=false},
        {path={'P001','dataa','type'},kind='integer',value=2},
        {path={'FontScope','type'},kind='string',value='misc'},
        {path={'FontScope','height','type'},kind='integer',value=1},
    }},
    {id='config_bare',cppPolicy='supported',input='[global]\ndata=fixture-data\ndata_ui=${YDWE}\ndata_meta=${DEFAULT}\n',ok=true,checks={
        {path={'global','data'},kind='string',value='fixture-data'},
        {path={'global','data_ui'},kind='string',value='${YDWE}'},
        {path={'global','data_meta'},kind='string',value='${DEFAULT}'},
    }},
    {id='numeric_section',cppPolicy='supported',input='[1001]\nname="number-section"\n',ok=true,checks={
        {path={'1001','name'},kind='string',value='number-section'}, {path={1001},absent=true},
    }},
    {id='crlf',cppPolicy='supported',input='[P001]\r\nvalue=3\r\n',ok=true,checks={{path={'P001','value'},kind='integer',value=3}}},
    {id='last_line_no_newline',cppPolicy='supported',input='[P001]\nvalue=3',ok=true,checks={{path={'P001','value'},kind='integer',value=3}}},
    {id='present_empty_file',cppPolicy='supported',input='',ok=true,checks={{path={},kind='table',count=0}}},
    -- 下面这些原版可能接受，但 C++ 的受限子集必须拒绝，不能强行补造相同语义。
    {id='bare_inline_comment',cppPolicy='must-reject',input='[global]\ndata=fixture--data\n',ok=true,checks={{path={'global','data'},kind='string',value='fixture'}}},
    {id='metadata_inline_comment',cppPolicy='must-reject',input='[ability]\n[.name]\ndisplayname=LABEL--comment\n',ok=true,checks={{path={'ability','name','displayname'},kind='string',value='LABEL'}}},
    {id='numeric_key',cppPolicy='must-reject',input='[P001]\n10=7\n',ok=true,checks={{path={'P001',10},kind='integer',value=7},{path={'P001','10'},absent=true}}},
    {id='numeric_prefix_key',cppPolicy='must-reject',input='[P001]\n1name=7\n',ok=false},
    {id='inheritance',cppPolicy='must-reject',input='[C]\nx=1\n[AB:C]\ny=2\n',ok=true,checks={{path={'AB','x'},kind='integer',value=1},{path={'AB','y'},kind='integer',value=2},{path={'AB:C'},absent=true}}},
    -- 现场 DLL 的点路径比公开旧 parser 更宽；首轮观察后固定为真实嵌套输出。
    {id='dotted_section',cppPolicy='must-reject',input='[A.B0]\nx=1\n',ok=true,checks={{path={'A','B0','x'},kind='integer',value=1},{path={'A.B0'},absent=true}}},
    {id='quoted_section',cppPolicy='must-reject',input='["P001"]\nx=1\n',ok=true,checks={{path={'P001','x'},kind='integer',value=1},{path={'"P001"'},absent=true}}},
    {id='angle_section',cppPolicy='must-reject',input='[A<00]\nx=1\n',ok=true,checks={{path={'A<00','x'},kind='integer',value=1}}},
    {id='duplicate_section',cppPolicy='must-reject',input='[P001]\nx=1\n[P001]\ny=2\n',ok=true,checks={{path={'P001','x'},absent=true},{path={'P001','y'},kind='integer',value=2}}},
    {id='duplicate_field',cppPolicy='must-reject',input='[P001]\nx=1\nx=2\n',ok=true,checks={{path={'P001','x'},kind='integer',value=2}}},
    {id='scope_child_collision',cppPolicy='must-reject',input='[FontScope]\ntype=misc\n[.type]\nfield=Type\nid=T001\nkey=type\ntype=3\n',ok=true,checks={{path={'FontScope','type'},kind='table',count=4},{path={'FontScope','type','type'},kind='integer',value=3}}},
    {id='missing_value',cppPolicy='must-reject',input='[P001]\nx=\n',ok=false},
    {id='nil_field',cppPolicy='must-reject',input='[P001]\nx=nil\n',ok=true,checks={{path={'P001','x'},absent=true}}},
    {id='integer_overflow_to_float',cppPolicy='must-reject',input='[P001]\nx=9223372036854775808\n',ok=true,checks={{path={'P001','x'},kind='float',value=9223372036854775808.0}}},
    {id='float_underflow',cppPolicy='must-reject',input='[P001]\nx=1e-999\n',ok=true,checks={{path={'P001','x'},kind='float',value=0.0}}},
    {id='float_overflow',cppPolicy='must-reject',input='[P001]\nx=1e999\n',ok=true,checks={{path={'P001','x'},kind='float',infinity='positive'}}},
    {id='hexadecimal',cppPolicy='must-reject',input='[P001]\nx=0x10\n',ok=true,checks={{path={'P001','x'},kind='integer',value=16}}},
    {id='decimal_escape',cppPolicy='must-reject',input='[P001]\nx="a\\000b"\n',ok=true,checks={{path={'P001','x'},kind='string',value='a\0b'}}},
    {id='unknown_escape',cppPolicy='must-reject',input='[P001]\nx="bad\\q"\n',ok=false},
    {id='trailing_comma',cppPolicy='must-reject',input='[P001]\nx={1,}\n',ok=true,checks={{path={'P001','x'},kind='table',count=1},{path={'P001','x',1},kind='integer',value=1}}},
    {id='nested_array',cppPolicy='must-reject',input='[P001]\nx={1,{2}}\n',ok=true,checks={{path={'P001','x',2,1},kind='integer',value=2}}},
    {id='sparse_table',cppPolicy='must-reject',input='[P001]\nx={3=2}\n',ok=true,checks={{path={'P001','x',3},kind='integer',value=2},{path={'P001','x',1},absent=true}}},
    {id='whole_line_comment',cppPolicy='must-reject',input='-- original comment\n[P001]\nx=1\n',ok=true,checks={{path={'P001','x'},kind='integer',value=1}}},
    {id='long_string',cppPolicy='must-reject',input='[P001]\nx=[=[line one\nline two]=]\n',ok=true,checks={{path={'P001','x'},kind='string',value='line one\nline two'}}},
    {id='numeric_expression',cppPolicy='must-reject',input='[P001]\nx=1 + 2\n',ok=false},
    {id='call_is_bare_data',cppPolicy='must-reject',input='[P001]\nx=io.open("never-created")\n',ok=true,checks={{path={'P001','x'},kind='string',value='io.open("never-created")'}}},
    {id='unquoted_prebuilt_string',cppPolicy='must-reject',input='[P001]\nx=literal\n',ok=true,checks={{path={'P001','x'},kind='string',value='literal'}}},
    {id='keyword_field_keys',cppPolicy='supported',input='[P001]\ntrue=1\nfalse=2\nnil=3\n',ok=true,checks={{path={'P001','true'},kind='integer',value=1},{path={'P001','false'},kind='integer',value=2},{path={'P001','nil'},kind='integer',value=3}}},
    {id='negative_fraction',cppPolicy='supported',input='[P001]\nx=-.5\ny=-2.5e-2\n',ok=true,checks={{path={'P001','x'},kind='float',value=-0.5},{path={'P001','y'},kind='float',value=-0.025}}},
    {id='bare_nan_is_string',cppPolicy='must-reject',input='[P001]\nx=nan\n',ok=true,checks={{path={'P001','x'},kind='string',value='nan'}}},
}
