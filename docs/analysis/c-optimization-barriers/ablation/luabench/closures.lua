local function mk(n) local c = 0; return function(x) c = c + x + n; return c end end
local acc = 0
for i = 1, 1000000 do local f = mk(i); for j = 1, 10 do acc = acc + f(j) end end
local co = coroutine.wrap(function() for i = 1, 1000000 do coroutine.yield(i) end end)
for i = 1, 1000000 do acc = acc + co() end
assert(acc > 0)
