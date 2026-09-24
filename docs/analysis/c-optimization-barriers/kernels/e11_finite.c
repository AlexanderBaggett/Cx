/* no side effects; C11 lets the compiler assume a loop with a non-constant
   controlling expression terminates */
void spin(unsigned i, unsigned n) { while (i != n) i += 2; }
