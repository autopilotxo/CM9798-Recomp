#pragma once
#include <stdint.h>
int x86_dis(const uint8_t *p, int avail, uint32_t addr, char *mn, char *ops);
int x86_dis_line(const uint8_t *p, int avail, uint32_t addr, char *mn, char *ops);
