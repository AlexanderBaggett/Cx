#include <math.h>
void vsqrt(double *restrict o, const double *restrict in, int n) {
  for (int i = 0; i < n; i++) o[i] = sqrt(in[i]);
}
