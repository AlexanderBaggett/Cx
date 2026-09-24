#include <stdio.h>
struct declared { char tag; double value; char flag; double weight; short id; };
struct sorted   { double value; double weight; short id; char tag; char flag; };
int main(void) { printf("declared=%zu sorted=%zu\n", sizeof(struct declared), sizeof(struct sorted)); }
