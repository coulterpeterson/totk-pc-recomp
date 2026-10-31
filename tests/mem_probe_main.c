// Entry and stand-ins for tests/mem_probe.py: runtime/memory.c, platform/linux.c and mem_probe.c run without the
// loader, the lifted code or SDL.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL_clipboard.h>

#include "dispatch.h"
#include "host.h"
#include "layout.h"

const RtLayoutEntry rt_layout[] = {{"main", MOD_MAIN_BASE, MOD_MAIN_SIZE}};
const int rt_layout_count = 1;
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
uint64_t res_heap_extra_bytes(void) { return 0; }
const char *cfg_get(const char *name) { return getenv(name); }
void cfg_init(void) {}
bool SDL_SetClipboardText(const char *s) { (void)s; return 0; }

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(3);
}

int main(void) {
    mem_init();
    return probe_mem();
}
