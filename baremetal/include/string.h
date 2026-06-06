#ifndef BAREMETAL_STRING_H
#define BAREMETAL_STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
char *strcpy(char *dest, const char *src);
int strcmp(const char *a, const char *b);
size_t strlen(const char *s);

#endif
