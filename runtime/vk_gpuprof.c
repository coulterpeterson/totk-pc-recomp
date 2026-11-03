// TOTK_TRACE=gpu: GPU time per pass of the replay. A timestamp is written at every full barrier the replay records
// (vk_exec.c barrier_to: every pass begins with one, copies and dispatches order themselves with one) and at the end of
// each command buffer part (vk_sync.c), outside any pass. The span from one timestamp to the next is a segment: the
// barrier, the pass it opens (if it opens one) and whatever is recorded before the next barrier. The completion thread
// reads a part's timestamps once its fence is reached and adds each segment to a table keyed by what it holds (the
// pass's targets: formats and extent, or "no pass"). Every 120 frames the table's top rows are logged, per frame, with
// the frame's total; one frame in every 1200 is also logged segment by segment, in order, and in that frame every draw
// inside a pass is timed on its own (a timestamp after each, whose wait for the draw serialises them): the draws are
// logged by index count times instances, so per-draw cost can be told from vertex work.
// A timestamp after ALL_COMMANDS waits for the work before it, which the full barrier next to it does anyway.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"

#define GP_Q 16384          // timestamps in the ring
#define GP_ROWS 512         // table rows (open addressing)
#define GP_LOG_FRAMES 120
#define GP_DUMP_EVERY 1200

typedef struct {
    uint64_t frame;
    int32_t kind;           // 1: the segment opens a pass, 0: another barrier, -1: a part's end
    uint32_t w, h, layers, ncolor;
    int32_t fmt[5];         // colour 0-3, depth; 0 none
    uint32_t draws, clears, copies, disp;   // recorded in the segment (filled at its end)
    uint32_t is_draw, icount, inst;         // a timed draw: the span ends at its timestamp; indices (or vertices), instances
} GpSeg;

typedef struct {
    int used;
    GpSeg key;              // kind, extent, layers and formats
    uint64_t ns, n, draws, clears, copies, disp;
} GpRow;

static VkQueryPool gp_pool;
static int gp_state;        // 0 not set up, 1 on, -1 off
static double gp_period;
static uint64_t gp_mask;
static GpSeg gp_seg[GP_Q];
static uint32_t gp_next;    // replay: next ring slot
static int gp_first = -1, gp_last = -1;    // replay: the current part's first and latest timestamp
static VkExecFrame gp_at;   // replay: counters at the latest timestamp
static GpSeg gp_pass;       // replay: the open pass's key (kind 0: none)
static uint32_t gp_reset_to;   // replay: draw timestamps may use slots below this one (reset at the pass's barrier)
#define GP_PASS_DRAWS 3000
static struct { uint64_t n, ns; } gp_db[24];   // completion thread: timed draws by log2(indices x instances)
static struct { uint64_t ns; uint32_t icount, inst; } gp_top[16];

static PlatMutex gp_lock = PLAT_MUTEX_INIT;   // the table (completion thread)
static GpRow gp_rows[GP_ROWS];
static uint64_t gp_frame0, gp_frames_seen, gp_last_frame, gp_total_ns;

typedef struct { uint32_t first, n; } GpRead;

static struct { int32_t bits; uint32_t n, in_pass, redundant; } gp_nb[32];   // replay thread writes, logged unlocked

void vk_gp_nvn_barrier(int32_t bits, int in_pass, int redundant) {
    for (int i = 0; i < 32; i++)
        if (gp_nb[i].bits == bits || !gp_nb[i].n) {
            gp_nb[i].bits = bits, gp_nb[i].n++, gp_nb[i].in_pass += in_pass != 0, gp_nb[i].redundant += redundant != 0;
            return;
        }
}

static int gp_on(void) {
    if (gp_state) return gp_state > 0;
    gp_state = -1;
    if (!(rt_trace & RT_TRACE_GPU)) return 0;
    VkCtx *c = vk_ctx();
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    if (!bits || c->props.limits.timestampPeriod <= 0) {
        rt_log("gpu: no timestamps on the graphics queue: no per-pass GPU time");
        return 0;
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = GP_Q};
    if (vkCreateQueryPool(c->device, &qi, NULL, &gp_pool) != VK_SUCCESS) {
        rt_log("gpu: timestamp query pool failed: no per-pass GPU time");
        return 0;
    }
    gp_period = c->props.limits.timestampPeriod;
    gp_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    gp_state = 1;
    rt_log("gpu: GPU time per pass, logged every %d frames", GP_LOG_FRAMES);
    return 1;
}

static uint32_t delta(uint64_t now, uint64_t then) { return now >= then ? (uint32_t)(now - then) : 0; }

// The segment that ends here gets what was recorded in it.
static void seg_close(void) {
    if (gp_last < 0) return;
    GpSeg *s = &gp_seg[gp_last];
    s->draws = delta(vk_exec_frame.draws, gp_at.draws);
    s->clears = delta(vk_exec_frame.clears, gp_at.clears);
    s->copies = delta(vk_exec_frame.copies, gp_at.copies);
    s->disp = delta(vk_exec_frame.dispatches, gp_at.dispatches);
}

static void stamp(VkCommandBuffer cb, const GpSeg *s) {
    if (gp_first >= 0 && gp_next % GP_Q == 0) return;   // a part does not wrap the ring: its later segments merge
    uint32_t q = gp_next++ % GP_Q;
    seg_close();
    gp_seg[q] = *s;
    gp_seg[q].frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    gp_at = vk_exec_frame;
    vkCmdResetQueryPool(cb, gp_pool, q, 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, q);
    if (gp_first < 0) gp_first = (int)q;
    gp_last = (int)q;
}

void vk_gp_barrier(VkExec *ex, int pass) {
    if (!gp_on() || !ex->cb) return;
    GpSeg s = {.kind = pass};
    if (pass) {
        s.w = ex->pass_w, s.h = ex->pass_h, s.layers = ex->pass_layers, s.ncolor = ex->pass_ncolor;
        for (uint32_t i = 0; i < 4 && i < ex->pass_ncolor; i++) s.fmt[i] = (int32_t)ex->pass_color[i];
        s.fmt[4] = (int32_t)(ex->pass_depth ? ex->pass_depth : ex->pass_stencil);
    }
    stamp(ex->cb, &s);
    gp_pass = pass ? s : (GpSeg){0};
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    uint32_t q = gp_next % GP_Q;
    if (pass && frame >= GP_DUMP_EVERY && frame % GP_DUMP_EVERY == 0 && q && q + GP_PASS_DRAWS <= GP_Q) {
        vkCmdResetQueryPool(ex->cb, gp_pool, q, GP_PASS_DRAWS);
        gp_reset_to = gp_next + GP_PASS_DRAWS;
    } else gp_reset_to = 0;
}

void vk_gp_draw(VkExec *ex, uint32_t count, uint32_t instances) {
    if (gp_state <= 0 || !gp_pass.kind || gp_next + 1 >= gp_reset_to || !ex->in_pass) return;
    uint32_t q = gp_next++ % GP_Q;
    seg_close();
    gp_seg[q] = gp_pass;
    gp_seg[q].frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    gp_seg[q].is_draw = 1, gp_seg[q].icount = count, gp_seg[q].inst = instances;
    gp_at = vk_exec_frame;
    vkCmdWriteTimestamp2(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, q);
    gp_last = (int)q;
}

static GpRow *row_of(const GpSeg *s) {
    uint64_t h = (uint64_t)(s->kind + 7) * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)s->w << 32 | s->h) * 0xC2B2AE3D27D4EB4Full;
    for (int i = 0; i < 5; i++) h = (h ^ (uint64_t)(uint32_t)s->fmt[i]) * 0x100000001B3ull;
    h ^= s->layers;
    for (uint32_t k = 0; k < GP_ROWS; k++) {
        GpRow *r = &gp_rows[(h + k) % GP_ROWS];
        if (!r->used) {
            r->used = 1;
            r->key = (GpSeg){.kind = s->kind, .w = s->w, .h = s->h, .layers = s->layers, .ncolor = s->ncolor};
            memcpy(r->key.fmt, s->fmt, sizeof s->fmt);
            return r;
        }
        if (r->key.kind == s->kind && r->key.w == s->w && r->key.h == s->h && r->key.layers == s->layers &&
            !memcmp(r->key.fmt, s->fmt, sizeof s->fmt))
            return r;
    }
    return NULL;
}

static int describe(char *b, size_t cap, const GpSeg *s) {
    if (s->kind <= 0) return snprintf(b, cap, "%s", s->kind < 0 ? "part end" : "no pass");
    int n = snprintf(b, cap, "%ux%u", s->w, s->h);
    if (s->layers > 1) n += snprintf(b + n, cap - (size_t)n, "x%u", s->layers);
    n += snprintf(b + n, cap - (size_t)n, " c");
    for (uint32_t i = 0; i < 4 && i < s->ncolor; i++) n += snprintf(b + n, cap - (size_t)n, "%s%d", i ? "," : "", s->fmt[i]);
    if (s->ncolor > 4) n += snprintf(b + n, cap - (size_t)n, ",+%u", s->ncolor - 4);
    if (!s->ncolor) n += snprintf(b + n, cap - (size_t)n, "-");
    n += snprintf(b + n, cap - (size_t)n, " d%d", s->fmt[4]);
    return n;
}

static int row_cmp(const void *a, const void *b) {
    const GpRow *x = *(const GpRow *const *)a, *y = *(const GpRow *const *)b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

static void table_log(void) {   // gp_lock held
    GpRow *list[GP_ROWS];
    int n = 0;
    for (int i = 0; i < GP_ROWS; i++)
        if (gp_rows[i].used && gp_rows[i].n) list[n++] = &gp_rows[i];
    qsort(list, (size_t)n, sizeof *list, row_cmp);
    double f = (double)gp_frames_seen;
    rt_log("gpu: %.2f ms a frame in %llu frames, top segments (VkFormat numbers; per frame: ms, segments, draws, clears, "
           "copies, dispatches):", gp_total_ns / 1e6 / f, (unsigned long long)gp_frames_seen);
    for (int i = 0; i < n && i < 30; i++) {
        char d[96];
        describe(d, sizeof d, &list[i]->key);
        rt_log("gpu:   %6.2f ms %5.1f %6.1f %4.1f %4.1f %4.1f  %s", list[i]->ns / 1e6 / f, list[i]->n / f,
               list[i]->draws / f, list[i]->clears / f, list[i]->copies / f, list[i]->disp / f, d);
    }
    uint64_t dn = 0, dns = 0;
    for (int i = 0; i < 24; i++) dn += gp_db[i].n, dns += gp_db[i].ns;
    if (dn) {
        rt_log("gpu: timed draws (one frame, serialised): %llu, %.2f ms, by indices x instances:", (unsigned long long)dn,
               dns / 1e6);
        for (int i = 0; i < 24; i++)
            if (gp_db[i].n)
                rt_log("gpu:   %8u-%-8u %5llu draws %7.3f ms, %6.1f us each", i ? 1u << (i - 1) : 0, (1u << i) - 1,
                       (unsigned long long)gp_db[i].n, gp_db[i].ns / 1e6, gp_db[i].ns / 1e3 / (double)gp_db[i].n);
        for (int i = 0; i < 16 && gp_top[i].ns; i++)
            rt_log("gpu:   slowest %2d: %7.1f us, %u indices x %u", i + 1, gp_top[i].ns / 1e3, gp_top[i].icount,
                   gp_top[i].inst);
        memset(gp_db, 0, sizeof gp_db);
        memset(gp_top, 0, sizeof gp_top);
    }
    for (int i = 0; i < 32 && gp_nb[i].n; i++)
        rt_log("gpu:   game barriers with bits %#x: %.1f a frame, %.1f ending a pass, %.1f after nothing", gp_nb[i].bits,
               gp_nb[i].n / f, gp_nb[i].in_pass / f, gp_nb[i].redundant / f);
    memset(gp_nb, 0, sizeof gp_nb);
    memset(gp_rows, 0, sizeof gp_rows);
    gp_total_ns = 0;
    gp_frames_seen = 0;
}

static void gp_read_cb(void *arg) {   // completion thread, in submission order
    GpRead *r = arg;
    uint32_t n = r->n;
    uint64_t *v = malloc(sizeof *v * n);
    if (!v) rt_fatal("vk: out of memory");
    VkResult res = vkGetQueryPoolResults(vk_ctx()->device, gp_pool, r->first, n, sizeof *v * n, v, sizeof *v,
                                         VK_QUERY_RESULT_64_BIT);
    if (res == VK_SUCCESS) {
        plat_mutex_lock(&gp_lock);
        for (uint32_t i = 0; i + 1 < n; i++) {
            const GpSeg *s = &gp_seg[r->first + i];
            uint64_t a = v[i] & gp_mask, b = v[i + 1] & gp_mask;
            if (b < a) continue;
            uint64_t ns = (uint64_t)((double)(b - a) * gp_period);
            if (s->frame != gp_last_frame) {
                if (!gp_frame0) gp_frame0 = s->frame;
                gp_last_frame = s->frame;
                if (gp_frames_seen == GP_LOG_FRAMES) table_log();
                gp_frames_seen++;
            }
            gp_total_ns += ns;
            GpRow *row = row_of(s);
            if (row) row->ns += ns, row->n += !s->is_draw, row->draws += s->draws, row->clears += s->clears,
                     row->copies += s->copies, row->disp += s->disp;
            // A timed draw's timestamp ends the span it was recorded in: this span is that draw (with its state).
            const GpSeg *nx = &gp_seg[r->first + i + 1];
            if (nx->is_draw) {
                {
                    uint64_t dns = ns, work = (uint64_t)nx->icount * (nx->inst ? nx->inst : 1);
                    int k = 0;
                    while (k < 23 && work >> k) k++;
                    gp_db[k].n++, gp_db[k].ns += dns;
                    for (int t = 0; t < 16; t++)
                        if (dns > gp_top[t].ns) {
                            memmove(&gp_top[t + 1], &gp_top[t], sizeof gp_top[0] * (size_t)(15 - t));
                            gp_top[t].ns = dns, gp_top[t].icount = nx->icount, gp_top[t].inst = nx->inst;
                            break;
                        }
                }
            }
            if (s->frame >= gp_frame0 + 600 && s->frame % GP_DUMP_EVERY == 0 && !s->is_draw) {
                char d[96];
                describe(d, sizeof d, s);
                rt_log("gpu: frame %llu segment %.3f ms, %u draws, %u clears, %u copies, %u dispatches: %s",
                       (unsigned long long)s->frame, ns / 1e6, s->draws, s->clears, s->copies, s->disp, d);
            }
        }
        plat_mutex_unlock(&gp_lock);
    }
    free(v);
    free(r);
}

void vk_gp_part_end(VkCommandBuffer cb, uint64_t fence) {
    if (gp_state <= 0 || gp_first < 0) return;
    GpSeg s = {.kind = -1};
    stamp(cb, &s);
    GpRead *r = malloc(sizeof *r);
    if (!r) rt_fatal("vk: out of memory");
    r->first = (uint32_t)gp_first, r->n = (uint32_t)(gp_last - gp_first + 1);
    vk_completion_add(fence, gp_read_cb, r);
    gp_first = gp_last = -1;
}

void vk_gp_part_drop(void) {
    seg_close();
    gp_first = gp_last = -1;
}
