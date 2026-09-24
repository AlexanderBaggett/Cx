#include <setjmp.h>
#include <stdarg.h>
jmp_buf env;
int work(int);
static int guarded(int x) { if (setjmp(env)) return -1; return work(x); }
static int sum_va(int n, ...) { va_list ap; va_start(ap, n); int s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
static int plain(int x) { return work(x) + 1; }
int caller(int x) { return guarded(x) + sum_va(3, x, x, x) + plain(x); }
