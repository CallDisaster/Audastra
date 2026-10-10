-- 此前缀只用于生成的触发器单元；不模拟 Warcraft 的调度器或 native。
assert(math.maxinteger and math.maxinteger >= 4611686018427387904, "LUA_INTEGER_WIDTH_UNSUPPORTED")
local function __forge_i32(value)
    -- 每个整数表达式边界保留低32位；先转Lua整数，避免乘法被double舍入。
    assert(type(value) == "number", "LUA_INTEGER_RANGE")
    local integer = math.tointeger(value)
    assert(integer ~= nil, "LUA_INTEGER_RANGE")
    local bits = integer & 0xffffffff
    if bits >= 0x80000000 then return bits - 0x100000000 end
    return bits
end
local function __forge_idiv(left, right)
    left, right = __forge_i32(left), __forge_i32(right)
    -- 1.27a外层错误/SEH处置未取证；这些边界保持明确的有意严格差异。
    assert(right ~= 0, "LUA_INTEGER_DIVIDE_ZERO: classic VM outer error handling is unverified")
    assert(left ~= -2147483648 or right ~= -1,
        "LUA_INTEGER_OVERFLOW: classic VM division exception handling is unverified")
    local quotient = math.abs(left) // math.abs(right)
    if (left < 0) ~= (right < 0) then return -quotient end
    return quotient
end
local function __forge_concat(left, right)
    assert(left == nil or type(left) == "string", "LUA_STRING_TYPE")
    assert(right == nil or type(right) == "string", "LUA_STRING_TYPE")
    if left == nil then return right end
    if right == nil then return left end
    return left .. right
end
local function __forge_real(value)
    assert(type(value) == "number" and value == value
        and value ~= math.huge and value ~= -math.huge, "LUA_REAL_NONFINITE")
    return value + 0.0
end
local function __forge_array(default)
    -- 代理表让已存在索引的写入也经过同一路径；false 与nil不用于存在性猜测。
    -- 此处模拟元素结果，不模拟经典string/handle引用计数或通知回调。
    local values = {}
    local allocated = false
    return setmetatable({}, {
        __index = function(_, key)
            key = __forge_i32(key)
            if key < 0 or key > 8191 then return default end
            local value = values[key]
            if value == nil then return default end
            return value
        end,
        __newindex = function(_, key, value)
            key = __forge_i32(key)
            -- 首次[-1]写可能走原版空指针路径，不把未证异常假装为普通越界。
            assert(allocated or key ~= -1,
                "LUA_ARRAY_UNALLOCATED_NEGATIVE_ONE: classic VM exception handling is unverified")
            allocated = true
            if key < 0 or key > 8191 then return end
            values[key] = value
        end,
        __metatable = false
    })
end
