local j = require('jass.common')
local first = j.CreateTextTag()
assert(type(first) == 'userdata')
j.DestroyTextTag(first)
j.SetTextTagText(first, 'destroyed ordinal', 0.02)
local reused = j.CreateTextTag()
assert(rawequal(first, reused))

local tags = {reused}
j.SetTextTagPermanent(reused, true)
for i = 2, 100 do
    tags[i] = j.CreateTextTag()
    j.SetTextTagPermanent(tags[i], true)
end
local zero = tags[100]
assert(type(zero) == 'userdata' and j.GetHandleId(zero) == 0)
local failed = j.CreateTextTag()
assert(rawequal(zero, failed) and j.GetHandleId(failed) == 0)
j.SetTextTagText(failed, 'native zero ordinal', 0.02)
assert(not pcall(j.DestroyTextTag, 0))
assert(not pcall(j.DestroyTextTag, nil))
assert(not pcall(j.AddWeatherEffect, nil, 0))
assert(not pcall(j.EnableWeatherEffect, nil, true))
assert(not pcall(j.RemoveWeatherEffect, nil))
for i = 1, #tags do j.DestroyTextTag(tags[i]) end
j.SetTextTagVisibility(zero, false)
j.DestroyTextTag(zero)
first, reused, tags, zero, failed = nil, nil, nil, nil, nil
collectgarbage('collect')

local label = j.CreateTextTag()
j.SetTextTagText(label, 'Audastra texttag native bridge ready', 0.022)
j.SetTextTagPos(label, 0, 0, 120)
j.SetTextTagVisibility(label, true)
j.SetTextTagPermanent(label, true)
j.SetCameraPosition(0, 0)
j.DisplayTimedTextToPlayer(j.Player(0), 0, 0, 45, 'R50 TEXTTAG ORDINAL: zero/reuse/forgery/weather refusal passed')
print('R50_TEXTTAG_ORDINAL_PASSED')
