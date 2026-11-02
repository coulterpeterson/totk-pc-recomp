// --probe-exec: what running the guest's own ARM64 code natively would need from an AArch64 Linux or Android host,
// without game files (docs/native-execution.md). Each line is one fact:
// - anonymous memory made executable after it was written (mmap RW, mprotect RX), at a fixed address below 2^39 as the
//   guest modules need, runs; Android's SELinux decides this per app domain (execmem);
// - a routine that changes x18, called through a trampoline that saves and restores it, returns to host code intact;
// - the counters guest code reads with MRS (CNTVCT_EL0, CNTPCT_EL0, CNTFRQ_EL0) and the thread registers
//   (TPIDR_EL0, TPIDRRO_EL0) are readable at EL0, and what they hold: a SIGILL is caught and reported.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"

#if defined(__aarch64__) && defined(__linux__)
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FAIL exec: %s\n", what); } } while (0)

static sigjmp_buf ill_jmp;
static void on_ill(int sig) { siglongjmp(ill_jmp, sig); }

// Calls fn with x18 saved in x19 (callee-saved, so fn keeps it) and put back after: bionic's code may use x18 as its
// shadow call stack pointer, and guest code treats x18 as an ordinary register.
static uint64_t call_saving_x18(void *fn) {
    register uint64_t r __asm__("x0");
    __asm__ volatile("mov x19, x18\n\tblr %1\n\tmov x18, x19"
                     : "=r"(r)
                     : "r"(fn)
                     : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "x19", "x30", "memory", "cc");
    return r;
}

static void *code_at(uintptr_t want, const uint32_t *words, size_t n) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *p = mmap((void *)want, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (want ? MAP_FIXED_NOREPLACE : 0),
                   -1, 0);
    if (p == MAP_FAILED) {
        printf("exec: mmap at %#lx: %s\n", (unsigned long)want, strerror(errno));
        return NULL;
    }
    memcpy(p, words, n * sizeof *words);
    __builtin___clear_cache((char *)p, (char *)p + n * sizeof *words);
    if (mprotect(p, page, PROT_READ | PROT_EXEC)) {
        printf("exec: mprotect RX at %p: %s\n", p, strerror(errno));
        munmap(p, page);
        return NULL;
    }
    return p;
}

#define READ_SYSREG(name, out, ok)                                                                                    \
    do {                                                                                                              \
        if (!sigsetjmp(ill_jmp, 1)) {                                                                                 \
            __asm__ volatile("mrs %0, " name : "=r"(out));                                                            \
            ok = 1;                                                                                                   \
        } else {                                                                                                      \
            ok = 0;                                                                                                   \
        }                                                                                                             \
    } while (0)

int probe_exec(void) {
    static const uint32_t ret42[] = {0xd2800540u /* mov x0, #42 */, 0xd65f03c0u /* ret */};
    static const uint32_t x18[] = {0xd28000f2u /* mov x18, #7 */, 0xaa1203e0u /* mov x0, x18 */, 0xd65f03c0u /* ret */};

    void *a = code_at(0, ret42, 2);
    CHECK(a, "anonymous RW memory made RX");
    if (a) {
        uint64_t r = call_saving_x18(a);
        printf("exec: anonymous code at %p returned %llu\n", a, (unsigned long long)r);
        CHECK(r == 42, "anonymous code result");
    }
    uintptr_t fixed = 0x3f00000000ull;   // below 2^39, above this host's guest modules (layout.h)
    void *b = code_at(fixed, x18, 3);
    CHECK(b == (void *)fixed, "code at a fixed address below 2^39");
    if (b) {
        uint64_t before, after;
        __asm__ volatile("mov %0, x18" : "=r"(before));
        uint64_t r = call_saving_x18(b);
        __asm__ volatile("mov %0, x18" : "=r"(after));
        printf("exec: fixed code at %p set x18 to %llu; host x18 %#llx before, %#llx after\n", b, (unsigned long long)r,
               (unsigned long long)before, (unsigned long long)after);
        CHECK(r == 7 && before == after, "x18 saved around guest-style code");
    }

    struct sigaction sa = {0}, old;
    sa.sa_handler = on_ill;
    sigaction(SIGILL, &sa, &old);
    uint64_t v = 0;
    int ok = 0;
    READ_SYSREG("cntfrq_el0", v, ok);
    printf("exec: CNTFRQ_EL0 %s %llu Hz (the Switch counts at 19200000)\n", ok ? "reads" : "traps", (unsigned long long)v);
    READ_SYSREG("cntvct_el0", v, ok);
    printf("exec: CNTVCT_EL0 %s %#llx\n", ok ? "reads" : "traps", (unsigned long long)v);
    READ_SYSREG("cntpct_el0", v, ok);
    printf("exec: CNTPCT_EL0 %s %#llx\n", ok ? "reads" : "traps", (unsigned long long)v);
    READ_SYSREG("tpidr_el0", v, ok);
    printf("exec: TPIDR_EL0 %s %#llx (bionic's thread pointer)\n", ok ? "reads" : "traps", (unsigned long long)v);
    READ_SYSREG("tpidrro_el0", v, ok);
    printf("exec: TPIDRRO_EL0 %s %#llx\n", ok ? "reads" : "traps", (unsigned long long)v);
    sigaction(SIGILL, &old, NULL);

    printf("probe-exec: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
#else
int probe_exec(void) {
    printf("probe-exec: skipped (AArch64 Linux or Android only)\n");
    return 0;
}
#endif
