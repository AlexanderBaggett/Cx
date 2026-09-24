int helper(int x) { return x * 3 + 1; }
int api(int x) { return helper(x) + helper(x + 1); }
