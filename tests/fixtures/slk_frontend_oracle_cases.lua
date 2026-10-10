-- 原创地图成员；不含游戏资源、私图内容或第三方实现。
local pack = string.pack
local function w3i(dataSet)
    local p = {
        pack('<i4i4i4', 25, 1, 6059), 'Frontend oracle\0Original fixture\0No map template\0\0',
        pack('<ffffffff', 0, 0, 1024, 0, 1024, 1024, 0, 1024),
        pack('<i4i4i4i4i4i4I4', 0, 0, 0, 0, 8, 8, 0), 'L',
        pack('<i4', -1), '\0\0\0\0', pack('<i4', dataSet), '\0\0\0\0',
        pack('<i4fffBBBB', 0, 0, 3000, 0, 0, 0, 0, 0), '\0\0\0\0\0', 'L',
        pack('<BBBB', 255, 255, 255, 255),
        -- players, forces, upgrades, technologies, random units, random items
        pack('<i4i4i4i4i4i4', 0, 0, 0, 0, 0, 0), '\255',
    }
    return table.concat(p)
end
local function nativeUnit()
    return pack('<i4i4i4', 2, 0, 1) .. 'hfooZ001' .. pack('<i4', 2)
        .. 'unam' .. pack('<i4', 3) .. 'TRIGSTR_7\0Z001'
        .. 'uhpm' .. pack('<i4i4', 0, 873) .. 'Z001'
end
local function members(dataSet)
    return {
        ['war3map.w3i'] = w3i(dataSet),
        ['war3map.wts'] = 'STRING 7\n{\nOriginal native WTS marker\n}\n',
        ['war3map.w3u'] = nativeUnit(),
    }
end
local function case(id, dataSet, overlay, build, checks)
    local files = members(dataSet)
    for name, value in pairs(overlay) do files[name] = value end
    return {id=id, version=dataSet == 2 and 'Melee' or 'Custom', members=files,
        buildCalls=build, checks=checks or {}}
end
return {
    case('custom-native-only', 1, {}, 0),
    case('custom-empty-misc', 1, {['war3mapmisc.txt']=''}, 1),
    case('custom-misc-values', 1, {['war3mapmisc.txt']='[Misc]\nGoldTextHeight=0.125\nBuildingAngle=123.5\n'}, 1, {
        {path={'misc','Misc','goldtextheight'}, kind='float', value=0.125},
        {path={'misc','Misc','buildingangle'}, kind='float', value=123.5},
    }),
    case('custom-unit-txt', 1, {['units\\humanunitstrings.txt']='[hfoo]\nName=Original TXT marker\n'}, 1, {
        {path={'unit','hfoo','name'}, kind='string', value='Original TXT marker'},
    }),
    case('melee-misc-values', 2, {['war3mapmisc.txt']='[Misc]\nGoldTextHeight=0.25\nBuildingAngle=45.5\n'}, 1, {
        {path={'misc','Misc','goldtextheight'}, kind='float', value=0.25},
        {path={'misc','Misc','buildingangle'}, kind='float', value=45.5},
    }),
}
