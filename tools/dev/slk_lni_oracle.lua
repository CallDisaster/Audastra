-- 仅运行仓库原创 fixture；绝对 DLL 显式加载，宿主完成实际模块身份核验后才开始解析。
local dllPath, fixturePath = assert(arg[1]), assert(arg[2])
package.path, package.cpath = '', ''
local loader, loadError = package.loadlib(dllPath, 'luaopen_lni')
assert(loader, loadError)
local lni = loader()
assert(type(lni) == 'table' and type(lni.classics) == 'function', 'target lacks lni.classics')
assert(type(math.type) == 'function' and type(string.pack) == 'function', 'Lua 5.3 typed export required')
io.stdout:write('SLK_LNI_ORACLE_READY\n'); io.stdout:flush()
assert(io.stdin:read('*l') == 'RUN', 'module identity handshake was not approved')
local cases = assert(loadfile(fixturePath, 't', {}))()
assert(type(cases) == 'table' and #cases > 0 and #cases <= 128, 'fixture count outside bounds')

local function hex(value)
    return (value:gsub('.', function(c) return string.format('%02x', string.byte(c)) end))
end
local function quote(value)
    return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end
local function kind(value)
    return type(value) == 'number' and math.type(value) or type(value)
end
local function count(value)
    local n = 0; for _ in pairs(value) do n = n + 1 end; return n
end
local function export(value, depth, budget)
    assert(depth <= 12, 'oracle export nesting exceeds fixture budget')
    budget.nodes = budget.nodes + 1; assert(budget.nodes <= 4096, 'oracle export node budget')
    local t = kind(value)
    if t == 'integer' then return '{"kind":"integer","decimal":' .. quote(tostring(value)) .. '}' end
    if t == 'float' then
        return '{"kind":"float","hex":' .. quote(string.format('%a', value))
            .. ',"ieee754le":' .. quote(hex(string.pack('<d', value))) .. '}'
    end
    if t == 'boolean' then return '{"kind":"boolean","value":' .. tostring(value) .. '}' end
    if t == 'string' then
        budget.bytes = budget.bytes + #value; assert(budget.bytes <= 1024 * 1024, 'oracle export string budget')
        return '{"kind":"string","byteCount":' .. #value .. ',"hex":' .. quote(hex(value)) .. '}'
    end
    assert(t == 'table', 'uncovered oracle value type ' .. t)
    local keys = {}; for key in pairs(value) do keys[#keys + 1] = key end
    table.sort(keys, function(a, b)
        local ak, bk = kind(a), kind(b)
        if ak ~= bk then return ak < bk end
        if ak == 'integer' or ak == 'float' or ak == 'string' then return a < b end
        if ak == 'boolean' then return not a and b end
        error('unexpected fixture key type ' .. ak)
    end)
    local entries = {}
    for _, key in ipairs(keys) do
        entries[#entries + 1] = '{"key":' .. export(key, depth + 1, budget)
            .. ',"value":' .. export(value[key], depth + 1, budget) .. '}'
    end
    return '{"kind":"table","count":' .. #keys .. ',"entries":[' .. table.concat(entries, ',') .. ']}'
end
local function verify(case, ok, value)
    local failures = {}
    if ok ~= case.ok then failures[#failures + 1] = 'success state differs from authored expectation' end
    if ok and case.ok then
        for index, check in ipairs(case.checks or {}) do
            local actual = value
            for _, key in ipairs(check.path) do
                if type(actual) ~= 'table' then actual = nil; break end
                actual = rawget(actual, key)
            end
            local passed = true
            if check.absent then passed = actual == nil
            else
                passed = kind(actual) == check.kind
                if check.value ~= nil then passed = passed and actual == check.value end
                if check.count ~= nil then passed = passed and type(actual) == 'table' and count(actual) == check.count end
                if check.negativeZero then passed = passed and actual == 0 and 1 / actual == -math.huge end
                if check.infinity then passed = passed and actual == (check.infinity == 'positive' and math.huge or -math.huge) end
            end
            if not passed then failures[#failures + 1] = 'check ' .. index .. ' differs' end
        end
    end
    return failures
end

local failed, seen = 0, {}
for _, case in ipairs(cases) do
    assert(type(case.id) == 'string' and case.id:match('^[a-z0-9_]+$') and not seen[case.id], 'invalid or duplicate fixture id')
    seen[case.id] = true
    assert(type(case.input) == 'string' and #case.input <= 16384, 'fixture input budget')
    local ok, value = pcall(lni.classics, case.input, 'original/' .. case.id .. '.ini')
    local failures = verify(case, ok, value)
    if #failures > 0 then failed = failed + 1 end
    local messages = {}; for _, message in ipairs(failures) do messages[#messages + 1] = quote(message) end
    local typed = ok and export(value, 0, {nodes=0,bytes=0}) or 'null'
    io.stdout:write('{"caseId":' .. quote(case.id) .. ',"cppPolicy":' .. quote(case.cppPolicy)
        .. ',"inputHex":' .. quote(hex(case.input)) .. ',"parseOk":' .. tostring(ok)
        .. ',"expectationPassed":' .. tostring(#failures == 0) .. ',"failures":[' .. table.concat(messages, ',')
        .. '],"errorHex":' .. (ok and 'null' or quote(hex(tostring(value)))) .. ',"typed":' .. typed .. '}\n')
end
io.stdout:write('{"summary":true,"luaVersion":' .. quote(_VERSION) .. ',"caseCount":' .. #cases
    .. ',"failedExpectations":' .. failed .. '}\n')
io.stdout:flush()
if failed > 0 then os.exit(1) end
