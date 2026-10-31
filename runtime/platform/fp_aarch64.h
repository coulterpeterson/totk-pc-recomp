// FP control on AArch64: FPCR (RMode bits 23-22, FZ bit 24). Its layout is the guest's own FPCR, so unlike MXCSR it
// needs no translation. Inline for the same reason as fp_x86.h: lifted FP code reads and writes it per operation.
#pragma once
#include <stdint.h>

#define PLAT_FP_RC_MASK (3u << 22)   // rounding mode: 0 nearest, 1 up, 2 down, 3 toward zero
#define PLAT_FP_FTZ (1u << 24)       // flushes denormal inputs and tiny results (before rounding, as the guest does)

static inline uint32_t plat_fp_get(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return (uint32_t)v; }
static inline void plat_fp_set(uint32_t v) { __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)v)); }
