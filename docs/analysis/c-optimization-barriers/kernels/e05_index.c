void add_off_int(float *a, const float *b, int n, int off) {
  for (int i = 0; i < n; i++) a[i] += b[i + off];
}
void add_off_uns(float *a, const float *b, unsigned n, unsigned off) {
  for (unsigned i = 0; i < n; i++) a[i] += b[i + off];
}
void add_off_sz(float *a, const float *b, unsigned long n, unsigned long off) {
  for (unsigned long i = 0; i < n; i++) a[i] += b[i + off];
}
