// --probe-mem: the guest address space and write tracking, without game files. mem_init has reserved the whole layout
// by now. The probe commits heap pages, writes some and checks that mem_written_pages returns exactly those, then
// loses no write with four threads writing while the main thread drains, covers a kernel write (file read) and a
// frozen region, and prints the VMA count of /proc/self/maps at each step (the cost of page-protection tracking,
// compared with vm.max_map_count). TOTK_WW=mprotect selects page-protection tracking where userfaultfd works too.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "host.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define HEAP_BYTES (64ull << 20)
#define WRITERS 4
#define PER_WRITER 256
#define ROUNDS 2000

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FAIL mem: %s\n", what); } } while (0)

static int vmas(void) {
    int n = 0;
#ifdef __linux__
    FILE *f = fopen("/proc/self/maps", "r");
    for (int c; f && (c = fgetc(f)) != EOF;) n += c == '\n';
    if (f) fclose(f);
#endif
    return n;
}

static uint64_t heap;

// Pages returned for the whole heap, drained.
static uint64_t drain(uint64_t *pages, uint64_t max) { return mem_written_pages(heap, HEAP_BYTES, pages, max); }

static volatile int writing;
static void writer(void *arg) {
    uint64_t base = heap + (uint64_t)(uintptr_t)arg * PER_WRITER * PAGE;
    for (uint64_t r = 1; r <= ROUNDS; r++)
        for (uint64_t i = 0; i < PER_WRITER; i += 1 + r % 3)
            __atomic_store_n((uint64_t *)(base + i * PAGE), r, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&writing, 1, __ATOMIC_SEQ_CST);
}

int probe_mem(void) {
    static uint64_t pages[WRITERS * PER_WRITER + 16], *shadow;
    const RtGuestLayout *L = mem_layout();
    const char *ww = getenv("TOTK_WW");
    printf("mem: write tracking requested: %s\n", ww ? ww : "default");
    printf("mem: stack/heap block %#llx, alias %#llx+%#llx, kernel %#llx, host functions %#llx\n",
           (unsigned long long)L->aslr.base, (unsigned long long)L->alias.base, (unsigned long long)L->alias.size,
           (unsigned long long)L->kernel.base, (unsigned long long)L->hostfn.base);
    CHECK(L->alias.base + L->alias.size <= RT_ADDRESS_LIMIT && L->kernel.base + L->kernel.size <= RT_ADDRESS_LIMIT,
          "regions below 2^39");
    printf("mem: VMAs after mem_init: %d\n", vmas());

    CHECK(mem_set_heap_size(HEAP_BYTES, &heap) == 0, "heap commit");
    printf("mem: VMAs with %llu MiB of heap committed: %d\n", (unsigned long long)(HEAP_BYTES >> 20), vmas());
    CHECK(drain(pages, 16) == 0, "a committed, unwritten heap reports no written pages");

    static const uint64_t idx[] = {0, 5, 6, 1000, 16383};
    for (int i = 0; i < 5; i++) *(volatile uint64_t *)(heap + idx[i] * PAGE) = 0x1234 + (uint64_t)i;
    uint64_t n = drain(pages, 16);
    CHECK(n == 5, "five written pages");
    for (uint64_t i = 0; i < n && i < 5; i++) CHECK(pages[i] == heap + idx[i] * PAGE, "written page address");
    printf("mem: VMAs with 5 pages written and tracking re-armed: %d\n", vmas());
    CHECK(drain(pages, 16) == 0, "a drain starts over");
    *(volatile uint64_t *)(heap + 6 * PAGE + 8) = 1;
    n = drain(pages, 16);
    CHECK(n == 1 && pages[0] == heap + 6 * PAGE, "a page written again is reported again");
    *(volatile uint64_t *)(heap + 7 * PAGE) = 1;
    CHECK(mem_written_peek(heap, HEAP_BYTES, pages, 16) == 1 && drain(pages, 16) == 1, "peek does not reset");

#ifdef __linux__
    // The kernel's write into a guest buffer must not fail and must count as a write.
    int fd = open("/dev/zero", O_RDONLY);
    char *buf = (char *)(heap + 40 * PAGE);
    memset(buf, 1, 16);
    drain(pages, 16);
    mem_thaw((uintptr_t)buf, PAGE);
    CHECK(fd >= 0 && read(fd, buf, PAGE) == (ssize_t)PAGE && buf[0] == 0, "file read into watched memory");
    n = drain(pages, 16);
    CHECK(n == 1 && pages[0] == heap + 40 * PAGE, "a kernel write counts as written");
    if (fd >= 0) close(fd);
#endif

    // A frozen region (not the first: the file read above holds that one thawed): the first write thaws it and still
    // shows up.
    uint64_t fz = heap + 0x200000;
    CHECK(mem_freeze(fz) == 1 && mem_frozen(fz), "freeze");
    *(volatile uint64_t *)(fz + 3 * PAGE) = 9;
    CHECK(!mem_frozen(fz) && *(volatile uint64_t *)(fz + 3 * PAGE) == 9, "a write thaws a frozen region");
    n = drain(pages, 16);
    CHECK(n == 1 && pages[0] == fz + 3 * PAGE, "the write that thawed is reported");

    // Four writers against a draining reader. shadow[] is the value of each page as of its last report, read after
    // the report; a write that was never reported leaves the page different from it at the end.
    shadow = calloc(WRITERS * PER_WRITER, sizeof *shadow);
    PlatThread *t[WRITERS];
    writing = WRITERS;
    for (uintptr_t i = 0; i < WRITERS; i++) t[i] = plat_thread_new(writer, (void *)i, "mem writer");
    uint64_t reports = 0;
    for (int last = 0; last < 2; last += !__atomic_load_n(&writing, __ATOMIC_SEQ_CST)) {
        n = mem_written_pages(heap, WRITERS * PER_WRITER * PAGE, pages, WRITERS * PER_WRITER);
        for (uint64_t i = 0; i < n; i++)
            shadow[(pages[i] - heap) / PAGE] = *(volatile uint64_t *)pages[i];
        reports += n;
    }
    for (int i = 0; i < WRITERS; i++) plat_thread_join(t[i]);
    int stale = 0;
    for (uint64_t i = 0; i < WRITERS * PER_WRITER; i++) stale += shadow[i] != *(volatile uint64_t *)(heap + i * PAGE);
    printf("mem: %d writes lost of %d pages, %llu page reports; VMAs now: %d\n", stale, WRITERS * PER_WRITER,
           (unsigned long long)reports, vmas());
    CHECK(!stale, "no write lost under concurrent writers");
    free(shadow);

    printf("mem: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
