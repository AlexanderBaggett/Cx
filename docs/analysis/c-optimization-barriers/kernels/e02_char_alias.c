#include <stddef.h>
struct cbuf { char *data; size_t len; };
struct ibuf { int  *data; size_t len; };
/* store through char* may modify b->data and b->len */
void fill_c(struct cbuf *b, char c) { for (size_t i = 0; i < b->len; i++) b->data[i] = c; }
/* store through int* cannot modify size_t/pointer fields under TBAA */
void fill_i(struct ibuf *b, int c)  { for (size_t i = 0; i < b->len; i++) b->data[i] = c; }
