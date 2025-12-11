#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int tputchar(int x, int y, int ch);
int tputstr(int x, int y, const char *str);
int tprintf(int x, int y, const char *format, ...);

#ifdef __cplusplus
}
#endif
