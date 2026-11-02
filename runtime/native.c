// Native execution of the game's own ARM64 code on an AArch64 Android host (docs/native-execution.md), in a build made
// with tools/build_game.py --native: the modules are the images tools/native/rewrite.py wrote, in which no guest
// instruction touches x18, TPIDR_EL0, TPIDRRO_EL0 or an SVC any more. Those live in the guest's Cpu, which the
// rewritten code reaches through bionic's TLS_SLOT_APP ([TPIDR_EL0 + 16]); the loader's key page holds the offsets of
// the Cpu fields (loader.c native_keys). The rest of the runtime is unchanged: host functions take the Cpu as they do
// for lifted code, and SVCs go to rt_svc.
//
// Four transitions, in assembly below:
// - native_run (rt_call, rt_original): saves the host's callee-saved registers, FPCR and the previous TLS_SLOT_APP on
//   the host stack, records that stack in c->host_sp, loads every guest register from c and jumps to c->pc.
// - native_return: guest code returned to RT_RET_HOST (x30 of rt_call). Stores the guest registers back into c and
//   returns from native_run.
// - native_hostfn_gate: guest code called a host function's address (the GOT holds RT_HOSTFN_BASE + 16*i, where each
//   slot is "adr x17, . ; b <stub>"). Stores the guest registers into c, c->pc = the slot, runs the host function on
//   the host stack below native_run's frame, loads the registers back and returns to x30.
// - native_svc_gate: a rewritten SVC site called c->gate (patch_nso.py site_gate: x30 is the return into the site's
//   block, x29 the guest's). Same as a host function, with rt_svc and the SVC number of that site (sdk.gates.tsv).
// x16 and x17 are the intra-procedure-call scratch registers: the host-function slots and the return sentinel use
// them, as a veneer would. Guest code never writes the hardware x18, so bionic's shadow call stack stays intact.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#if defined(RT_NATIVE) && defined(__aarch64__)

_Static_assert(offsetof(Cpu, x) == 0 && offsetof(Cpu, sp) == 256 && offsetof(Cpu, pc) == 264 &&
                   offsetof(Cpu, nzcv) == 272 && offsetof(Cpu, tpidr) == 280 && offsetof(Cpu, tpidrro) == 288 &&
                   offsetof(Cpu, fpcr) == 320 && offsetof(Cpu, fpsr) == 328 && offsetof(Cpu, gate) == 344 &&
                   offsetof(Cpu, host_sp) == 352 && offsetof(Cpu, v) == 512,
               "the assembly below uses these offsets");

void native_run(Cpu *c);
void native_return(void);
void native_hostfn_gate(void);
void native_svc_gate(void);
void native_hostfn_c(Cpu *c);
void native_svc_c(Cpu *c, uint64_t ret);

// native_run's frame on the host stack (192 bytes): x19-x30 at 0, d8-d15 at 96, the previous TLS_SLOT_APP and
// c->host_sp at 160, the host FPCR and c at 176.
__asm__(
    ".macro SAVE_GUEST c\n"   // every guest register but x16, x17 (the entry stores those) and x18 (in the Cpu)
    "  stp x0, x1, [\\c, #0]\n  stp x2, x3, [\\c, #16]\n  stp x4, x5, [\\c, #32]\n  stp x6, x7, [\\c, #48]\n"
    "  stp x8, x9, [\\c, #64]\n  stp x10, x11, [\\c, #80]\n  stp x12, x13, [\\c, #96]\n  stp x14, x15, [\\c, #112]\n"
    "  stp x19, x20, [\\c, #152]\n  stp x21, x22, [\\c, #168]\n  stp x23, x24, [\\c, #184]\n"
    "  stp x25, x26, [\\c, #200]\n  stp x27, x28, [\\c, #216]\n  stp x29, x30, [\\c, #232]\n"
    "  mov x0, sp\n  str x0, [\\c, #256]\n  mrs x0, nzcv\n  str x0, [\\c, #272]\n"
    "  mrs x0, fpcr\n  mrs x1, fpsr\n  stp x0, x1, [\\c, #320]\n"
    "  stp q0, q1, [\\c, #512]\n  stp q2, q3, [\\c, #544]\n  stp q4, q5, [\\c, #576]\n  stp q6, q7, [\\c, #608]\n"
    "  stp q8, q9, [\\c, #640]\n  stp q10, q11, [\\c, #672]\n  stp q12, q13, [\\c, #704]\n  stp q14, q15, [\\c, #736]\n"
    "  stp q16, q17, [\\c, #768]\n  stp q18, q19, [\\c, #800]\n  stp q20, q21, [\\c, #832]\n"
    "  stp q22, q23, [\\c, #864]\n  stp q24, q25, [\\c, #896]\n  stp q26, q27, [\\c, #928]\n"
    "  stp q28, q29, [\\c, #960]\n  stp q30, q31, [\\c, #992]\n"
    ".endm\n"
    ".macro LOAD_GUEST\n"   // x16 = the Cpu: every guest register but x16, x17 and x18 from it, sp included
    "  ldp q0, q1, [x16, #512]\n  ldp q2, q3, [x16, #544]\n  ldp q4, q5, [x16, #576]\n  ldp q6, q7, [x16, #608]\n"
    "  ldp q8, q9, [x16, #640]\n  ldp q10, q11, [x16, #672]\n  ldp q12, q13, [x16, #704]\n"
    "  ldp q14, q15, [x16, #736]\n  ldp q16, q17, [x16, #768]\n  ldp q18, q19, [x16, #800]\n"
    "  ldp q20, q21, [x16, #832]\n  ldp q22, q23, [x16, #864]\n  ldp q24, q25, [x16, #896]\n"
    "  ldp q26, q27, [x16, #928]\n  ldp q28, q29, [x16, #960]\n  ldp q30, q31, [x16, #992]\n"
    "  ldp x0, x1, [x16, #320]\n  msr fpcr, x0\n  msr fpsr, x1\n  ldr x0, [x16, #272]\n  msr nzcv, x0\n"
    "  ldr x0, [x16, #256]\n  mov sp, x0\n"
    "  ldp x0, x1, [x16, #0]\n  ldp x2, x3, [x16, #16]\n  ldp x4, x5, [x16, #32]\n  ldp x6, x7, [x16, #48]\n"
    "  ldp x8, x9, [x16, #64]\n  ldp x10, x11, [x16, #80]\n  ldp x12, x13, [x16, #96]\n  ldp x14, x15, [x16, #112]\n"
    "  ldp x19, x20, [x16, #152]\n  ldp x21, x22, [x16, #168]\n  ldp x23, x24, [x16, #184]\n"
    "  ldp x25, x26, [x16, #200]\n  ldp x27, x28, [x16, #216]\n  ldp x29, x30, [x16, #232]\n"
    ".endm\n"
    ".macro HOST_FRAME_POP\n"   // sp = native_run's frame: host registers back, the frame popped
    "  ldr x12, [sp, #176]\n  msr fpcr, x12\n"
    "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n  ldp x25, x26, [sp, #48]\n"
    "  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n"
    "  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n  add sp, sp, #192\n"
    ".endm\n"

    ".text\n.p2align 4\n.globl native_run\n.type native_run, %function\n"
    "native_run:\n"
    "  sub sp, sp, #192\n"
    "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n  stp x25, x26, [sp, #48]\n"
    "  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n"
    "  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
    "  mrs x9, tpidr_el0\n  ldr x10, [x9, #16]\n  ldr x11, [x0, #352]\n  stp x10, x11, [sp, #160]\n"
    "  mrs x12, fpcr\n  stp x12, x0, [sp, #176]\n"
    "  str x0, [x9, #16]\n  mov x12, sp\n  str x12, [x0, #352]\n"
    "  mov x16, x0\n  LOAD_GUEST\n"
    "  ldr x17, [x16, #264]\n  ldr x16, [x16, #128]\n  br x17\n"   // a function entry: x17 is scratch there
    ".size native_run, .-native_run\n"

    ".p2align 4\n.globl native_return\n.type native_return, %function\n"
    "native_return:\n"   // from RT_RET_HOST: x16 is the sentinel's scratch
    "  mrs x16, tpidr_el0\n  ldr x16, [x16, #16]\n  str x17, [x16, #136]\n  SAVE_GUEST x16\n"
    "  ldr x0, [x16, #352]\n  mov sp, x0\n  ldp x10, x11, [sp, #160]\n  str x11, [x16, #352]\n"
    "  mrs x9, tpidr_el0\n  str x10, [x9, #16]\n  HOST_FRAME_POP\n  ret\n"
    ".size native_return, .-native_return\n"

    ".p2align 4\n.globl native_hostfn_gate\n.type native_hostfn_gate, %function\n"
    "native_hostfn_gate:\n"   // x17 = the slot called, x16 the slot's scratch
    "  mrs x16, tpidr_el0\n  ldr x16, [x16, #16]\n  str x17, [x16, #264]\n  str x17, [x16, #136]\n"
    "  SAVE_GUEST x16\n"
    "  mov x0, x16\n  ldr x1, [x0, #352]\n  mov sp, x1\n  sub sp, sp, #16\n  str x0, [sp]\n"
    "  bl native_hostfn_c\n"
    "  ldr x16, [sp]\n  mrs x9, tpidr_el0\n  str x16, [x9, #16]\n"   // again: a fiber switch inside may have changed it
    "  LOAD_GUEST\n  ldr x17, [x16, #136]\n  ldr x16, [x16, #128]\n  ret\n"
    ".size native_hostfn_gate, .-native_hostfn_gate\n"

    ".p2align 4\n.globl native_svc_gate\n.type native_svc_gate, %function\n"
    "native_svc_gate:\n"   // x30 = the return into the site's block; every other register is the guest's
    "  stp x16, x17, [sp, #-16]!\n  mrs x16, tpidr_el0\n  ldr x16, [x16, #16]\n"
    "  ldr x17, [sp]\n  str x17, [x16, #128]\n  ldr x17, [sp, #8]\n  str x17, [x16, #136]\n  add sp, sp, #16\n"
    "  SAVE_GUEST x16\n"
    "  mov x0, x16\n  mov x1, x30\n  ldr x2, [x0, #352]\n  mov sp, x2\n  sub sp, sp, #16\n  str x0, [sp]\n"
    "  bl native_svc_c\n"
    "  ldr x16, [sp]\n  mrs x9, tpidr_el0\n  str x16, [x9, #16]\n"
    "  LOAD_GUEST\n  ldr x17, [x16, #136]\n  ldr x16, [x16, #128]\n  ret\n"
    ".size native_svc_gate, .-native_svc_gate\n");

// ---- SVC sites: the return address of each site's block (gates.tsv) -> its SVC number and the original site
typedef struct { uint64_t ret, site; uint32_t imm; } SvcSite;
static SvcSite *svc_sites;
static int nsvc, capsvc;

void native_add_svc_site(uint64_t ret, uint64_t site, uint32_t imm) {
    if (nsvc == capsvc) {
        capsvc = capsvc ? 2 * capsvc : 256;
        svc_sites = realloc(svc_sites, (size_t)capsvc * sizeof *svc_sites);
        if (!svc_sites) rt_fatal("native: SVC site table allocation");
    }
    svc_sites[nsvc++] = (SvcSite){ret, site, imm};
}

static int by_ret(const void *a, const void *b) {
    uint64_t x = ((const SvcSite *)a)->ret, y = ((const SvcSite *)b)->ret;
    return x < y ? -1 : x > y;
}

void native_svc_c(Cpu *c, uint64_t ret) {
    int lo = 0, hi = nsvc - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (svc_sites[mid].ret == ret) {
            c->pc = svc_sites[mid].site;
            rt_svc(c, svc_sites[mid].imm, svc_sites[mid].site);
            return;
        }
        if (svc_sites[mid].ret < ret) lo = mid + 1;
        else hi = mid - 1;
    }
    char at[96];
    rt_fatal("native: SVC gate called from %s, which is no rewritten SVC site", ldr_describe(ret, at, sizeof at));
}

void native_hostfn_c(Cpu *c) {
    GuestFn f = rt_dispatch_lookup(c->pc);
    if (!f) {
        char lr[96];
        rt_fatal("native: no host function at %#llx (called from %s)", (unsigned long long)c->pc,
                 ldr_describe(c->x[30], lr, sizeof lr));
    }
    f(c);
}

// ---- the host-function window: a two-word slot per host function, and the return sentinel
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((to - from) >> 2) & 0x3FFFFFF); }

static void far_jump(uint32_t *at, void (*target)(void)) {   // ldr x16, 8 ; br x16 ; .quad target
    at[0] = 0x58000050u;
    at[1] = 0xD61F0200u;
    memcpy(&at[2], &(uint64_t){(uint64_t)(uintptr_t)target}, 8);
}

void native_init(void) {
    static int done;
    if (done) return;
    done = 1;
    uint64_t base = RT_HOSTFN_BASE, common = RT_RET_HOST + 0x100;
    if (!plat_mem_commit(base, RT_HOSTFN_SIZE)) rt_fatal("native: cannot commit the host-function window");
    uint32_t *w = (uint32_t *)(uintptr_t)base;
    for (uint64_t a = base; a < RT_RET_HOST; a += 16) {
        uint32_t *s = &w[(a - base) / 4];
        s[0] = 0x10000011u;   // adr x17, .
        s[1] = branch(a + 4, common);
        s[2] = s[3] = 0xD4200000u;   // brk #0
    }
    far_jump(&w[(RT_RET_HOST - base) / 4], native_return);
    far_jump(&w[(common - base) / 4], native_hostfn_gate);
    __builtin___clear_cache((char *)w, (char *)w + RT_HOSTFN_SIZE);
    if (!plat_mem_protect(base, RT_HOSTFN_SIZE, PLAT_PROT_RX)) rt_fatal("native: cannot make the host-function window executable");
    qsort(svc_sites, (size_t)nsvc, sizeof *svc_sites, by_ret);
    rt_log("native: guest code runs natively; %d SVC sites, host functions at %#llx", nsvc, (unsigned long long)base);
}

#define TLS_SLOT_APP 2   // bionic: available to apps (tools/native/README.md)
static uint64_t *tls_slots(void) {
    uint64_t *tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

// rt_call and rt_original: run guest code at c->pc until it returns to RT_RET_HOST.
void native_call(Cpu *c) {
    uint64_t prev = tls_slots()[TLS_SLOT_APP];
    if (prev && ((const Cpu *)(uintptr_t)prev)->gate != (uint64_t)(uintptr_t)native_svc_gate)
        rt_fatal("native: bionic's TLS_SLOT_APP holds %#llx, which is not a guest Cpu: something else uses the slot",
                 (unsigned long long)prev);
    c->gate = (uint64_t)(uintptr_t)native_svc_gate;
    native_run(c);
}

#endif
