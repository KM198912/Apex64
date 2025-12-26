#pragma once
#include <stdint.h>
#include <stddef.h>  /* for uintptr_t */

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *s);
int strncmp(const char *s1, const char *s2, size_t n);
int strcmp(const char *s1, const char *s2);

/* Allocate a duplicate of a string using kernel allocator */
char *strdup(const char *s);
int atoi(const char *s);