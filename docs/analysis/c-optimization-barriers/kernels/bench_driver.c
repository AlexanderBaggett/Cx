#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
struct cbuf { char *data; size_t len; };
void fill_c(struct cbuf *b, char c); void fill_c_local(struct cbuf *b, char c);
void vsqrt(double *restrict o, const double *restrict in, int n);
float fsum(const float *a, int n);
void scale(float *dst, const float *src, int n, float k);
void scale_r(float *restrict dst, const float *restrict src, int n, float k);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(double*)a, y = *(double*)b; return x < y ? -1 : x > y; }
#define N 4096
#define REPS 20000
#define RUNS 9
volatile float sink;
int main(int argc, char **argv) {
  const char *which = argv[1];
  static char cb[N]; static double din[N], dout[N]; static float fa[N], fb[N];
  for (int i = 0; i < N; i++) { din[i] = i + 0.5; fa[i] = (float)(i % 7) * 0.25f; }
  struct cbuf b = { cb, N };
  double t[RUNS];
  for (int r = 0; r < RUNS; r++) {
    double t0 = now();
    for (int k = 0; k < REPS; k++) {
      if (!strcmp(which, "fill_c")) fill_c(&b, (char)k);
      else if (!strcmp(which, "fill_c_local")) fill_c_local(&b, (char)k);
      else if (!strcmp(which, "vsqrt")) vsqrt(dout, din, N);
      else if (!strcmp(which, "fsum")) sink = fsum(fa, N);
      else if (!strcmp(which, "scale")) scale(fb, fa, N, 1.5f);
      else if (!strcmp(which, "scale_r")) scale_r(fb, fa, N, 1.5f);
    }
    t[r] = (now() - t0) / ((double)REPS * N);
  }
  qsort(t, RUNS, sizeof t[0], cmpd);
  printf("%-14s median %.3f ns/elem  (min %.3f, max %.3f)\n", which, t[RUNS/2], t[0], t[RUNS-1]);
  return 0;
}
