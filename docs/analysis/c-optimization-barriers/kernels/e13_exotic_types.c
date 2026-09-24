double _Complex cmul(double _Complex a, double _Complex b) { return a * b; }
double _Complex cdiv(double _Complex a, double _Complex b) { return a / b; }
long double ldmul(long double a, long double b) { return a * b; }
void ldscale(long double *restrict o, const long double *restrict in, int n) { for (int i = 0; i < n; i++) o[i] = in[i] * 2.0L; }
void dscale(double *restrict o, const double *restrict in, int n) { for (int i = 0; i < n; i++) o[i] = in[i] * 2.0; }
_BitInt(256) bmul(_BitInt(256) a, _BitInt(256) b) { return a / b; }
