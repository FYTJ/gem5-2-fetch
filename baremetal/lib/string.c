#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;
  while (n--) {
    *d++ = *s++;
  }
  return dest;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  while (n--) {
    *p++ = (unsigned char)c;
  }
  return s;
}

char *strcpy(char *dest, const char *src) {
  char *out = dest;
  while ((*dest++ = *src++) != '\0') {
  }
  return out;
}

int strcmp(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p != '\0') {
    p++;
  }
  return (size_t)(p - s);
}
