local N = 2000000
local t = {}
for i = 1, N do t[i] = (i * 7919) % 1000003 end
table.sort(t)
local h = {}
for i = 1, N // 2 do h["k" .. (i % 50000)] = (h["k" .. (i % 50000)] or 0) + t[i] end
local sum = 0
for k, v in pairs(h) do sum = sum + v end
assert(sum > 0)
