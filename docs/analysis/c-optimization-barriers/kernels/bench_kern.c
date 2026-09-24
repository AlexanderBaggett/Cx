#include <stddef.h>
#include <math.h>
struct cbuf { char *data; size_t len; };
void fill_c(struct cbuf *b, char c) { for (size_t i = 0; i < b->len; i++) b->data[i] = c; }
void fill_c_local(struct cbuf *b, char c) { char *d = b->data; size_t n = b->len; for (size_t i = 0; i < n; i++) d[i] = c; }
void vsqrt(double *restrict o, const double *restrict in, int n) { for (int i = 0; i < n; i++) o[i] = sqrt(in[i]); }
float fsum(const float *a, int n) { float s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
void scale(float *dst, const float *src, int n, float k) { for (int i = 0; i < n; i++) dst[i] = src[i] * k; }
void scale_r(float *restrict dst, const float *restrict src, int n, float k) { for (int i = 0; i < n; i++) dst[i] = src[i] * k; }
