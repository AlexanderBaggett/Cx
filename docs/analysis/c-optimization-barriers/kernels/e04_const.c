void opaque(void);
int sum_twice(const int *p) {
  int a = *p;
  opaque();         /* const int* does not mean *p is immutable */
  return a + *p;
}
