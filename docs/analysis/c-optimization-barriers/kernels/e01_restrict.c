void scale(float *dst, const float *src, int n, float k) {
  for (int i = 0; i < n; i++) dst[i] = src[i] * k;
}
void scale_r(float *restrict dst, const float *restrict src, int n, float k) {
  for (int i = 0; i < n; i++) dst[i] = src[i] * k;
}
