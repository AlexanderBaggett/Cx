local t = {}
for i = 1, 200000 do t[#t+1] = string.format("%d:%s", i, tostring(i * 3)) end
local s = table.concat(t, ",")
local n = 0
for w in s:gmatch("(%d+):") do n = n + #w end
local r = s:gsub("1", "one"):upper():lower()
assert(n > 0 and #r > 0)
for i = 1, 30 do local x = s:find("199999:", 1, true); assert(x) end
