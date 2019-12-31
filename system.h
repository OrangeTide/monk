#ifndef SYSTEM_H_
#define SYSTEM_H_
#include <stdint.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;

int system_init(void);
void system_done(void);
int system_loadfile(const char *filename);
int system_tick(int n);
#endif
