local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end
assert(fib(35) == 9227465)
