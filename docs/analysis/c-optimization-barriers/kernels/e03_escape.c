void ext(int *);
void opaque(void);
int local_escape(void) {
  int x = 1;
  ext(&x);          /* address escapes */
  int a = x;
  opaque();         /* may modify x through the escaped pointer */
  return a + x;
}
int global_counter;
void count_calls(int n) {
  for (int i = 0; i < n; i++) { global_counter++; opaque(); }
}
static int static_counter;
int count_static(int n) {
  for (int i = 0; i < n; i++) { static_counter++; opaque(); }
  return static_counter;
}
