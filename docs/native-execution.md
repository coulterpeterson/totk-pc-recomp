# Native execution on Android (scope)

The Android build lifts the game's ARM64 code to C and compiles it back to ARM64. On the AYN Thor (Snapdragon 8 Gen 2)
that code is about three times the instructions of the original and the frame waits on it: 22-23 ms of the 28.8 ms
critical path in performance mode is the main thread and five workers running game code ([android-plan.md](android-plan.md),
"Status"). The host is ARM64 like the Switch, so the game's own code could run unchanged, as MacNativeGames does on
Apple silicon and Eden's NCE does on this SoC. This document scopes that for this repository. Built so far: the probe
and milestone 1, the rewritten 1.0.0 modules (below).

## What runs natively, and what stays

Native execution replaces only the CPU path. The rest of the runtime stays as it is: the guest kernel
(`runtime/kernel.c`: threads, virtual cores, waits), the host functions (`runtime/hostfn.c` and the ~163 SDK
replacements), IPC and services, the NVN front end and the Vulkan back end, memory and the write watch. The lifter
stays for the desktop build and as the Android fallback; native is a second CPU back end for AArch64 hosts, chosen at
build time.

## The reference: MacNativeGames' rewriter

MacNativeGames (MIT, `refs/MacNativeGames/games/totk`) runs TotK 1.0.0-1.4.3 natively. Its rewriter
(`third_party/d2r/patch_nso.py`, from d2r-mobile, MIT, with TotK fixes including two for 1.0.0's `main`) runs offline
with capstone. It writes a patched image of each module with a patch area behind it, inside branch range, and sends
every site that cannot run unchanged to a trampoline there:

| Site | 1.0.0 count (main / subsdk0 / sdk) | Replacement |
|---|---|---|
| `svc` | 0 / 0 / 60 | gate: save registers, switch to the host stack, call the host, return |
| `mrs cntpct_el0` | 0 / 0 / 40 | gate (the host's `cntvct_el0` would do: both count at 19.2 MHz, below) |
| `mrs/msr tpidr_el0`, `mrs tpidrro_el0` | 796 / 0 / 128 | load or store of a per-thread slot |
| any use of x18 | about 29,500 / 10,200 / 12,700 | x18 lives in a per-thread slot; the instruction uses a borrowed scratch register |
| `br x18` / `blr x18` (jump tables, calls) | 109 / 2 / 6 | landing pads; a two-word call window |
| exclusive windows that use x18 | part of 69,700 exclusives | the whole window copied, so no store lands in the reservation |

Counts from this repository's scan of 1.0.0 (capstone over `.text`; the x18 count includes a few data words). After
the rewrite, guest code never touches the hardware x18 or TPIDR_EL0: both stay the host's. Exclusives without x18 run
unchanged. Host functions are entered through numbered thunks that switch to a host stack and call ordinary C.

## Android facts (`--probe-exec`, on the Thor)

`runtime/exec_probe.c`, run inside the app (`am start ... --es args '--probe-exec'`):
- Anonymous memory written and then made executable (mmap RW, mprotect RX) runs in the app's `untrusted_app` domain,
  also at a fixed address below 2^39, as the modules need. The process has no seccomp filter (`Seccomp: 0`).
- bionic uses x18 on the game's threads (the shadow call stack: `0x6e924f6000`), and a trampoline that saves it
  around guest-style code keeps it intact. The rewriter's design, where guest code never owns x18, is what Android
  needs as well.
- `CNTFRQ_EL0` is 19,200,000 Hz, the Switch's frequency, and `CNTVCT_EL0` reads at EL0. `CNTPCT_EL0` traps (SIGILL),
  so its 40 sites must be rewritten (to `cntvct_el0` or a gate).
- `TPIDRRO_EL0` reads 0, and `TPIDR_EL0` is bionic's thread pointer.

The one Apple dependency of the rewriter is how a trampoline finds its thread's slots: MacNativeGames reads
`tpidrro_el0`, Darwin's thread-specific data base, and adds a key. On Linux that register is 0. The Android rewrite
reads `TPIDR_EL0` and loads bionic's `TLS_SLOT_APP` (slot 2, reserved for apps since Android 10): the runtime keeps
each guest thread's slot block there, so `mrs xT, tpidr_el0; ldr xT, [xT, #16]` replaces the one Apple word. On the
Thor that slot is empty on the app's threads, and the sequence reads a slot through it (`--probe-exec`). The runtime
must check the slot is still empty when it creates each guest thread.

## What this repository has to write

| Part | Size (estimate) | Notes |
|---|---|---|
| Rewriter variant | ~1,100 lines Python | an adapted copy of MacNativeGames' `patch_nso.py` (MIT header, `THIRD_PARTY_NOTICES.md` row) with the Android slot base, plus a site class that sends the 874 `spin` back-edges of `recomp/patches-v0.txt` to a gate (`rt_spin_gate`); outputs under `build/` |
| Layout | small | `recomp/layout.py` gives each module room for its patch area: today `main` ends 1.6 MiB below `subsdk0` |
| Loader | ~300 lines C | map the patched images at the layout's bases, apply the load-time word patches (fps60, realtime; the `site` lines become word patches), seal code RX |
| Gate and thread entry | ~400 lines asm and C | an SVC gate whose frame is laid out as `Cpu`, so `kern_svc` is used unchanged; thread entry on the guest stack; host thunks that copy x0-x7/v0-v7 into a `Cpu` for the existing `GuestFn(Cpu *)` host functions and back; `rt_call` into guest code |
| Signals | ~150 lines | a per-thread alternate stack for every guest thread (the trampolines use the 112 bytes below sp, and a signal frame there would overwrite them); the write-watch SIGSEGV, the async preemption (`kern_ap_stub` uses x18 under `-ffixed-x18` today, and its safe-point test changes from "pc in lifted code" to "pc in guest text or patch area"); crash reports with guest pcs |
| Dropped on the native path | | `dispatch.c` and the lifted code, jump table recovery, the fiber host overrides (fibers become plain guest code), the indirect-call preemption checkpoint (SVCs and async preemption remain) |

## Milestones

1. **Slot base and rewrite** (done, 2026-11-02). `tools/native/rewrite.py --version v0 --verify` (README in
   `tools/native/`) writes the patched images, site tables and a layout with room for each patch area to
   `build/v0-native/`. Sites: `main` 30,222 (29,349 x18, 796 TPIDR_EL0 reads, 76 exclusive windows, 1 call window, 108
   jump tables), `sdk` 12,964 (60 SVC gates, 40 counter reads now `cntvct_el0` in place), `subsdk0` 10,201. The
   structure checker passes on all three, and the equivalence checker runs every site against Unicorn with 0
   divergent (`main` 93,511 trials) and catches all 300 deliberately broken blocks per module. Patch areas: `main`
   42.7 MiB, `sdk` 1.3 MiB, `subsdk0` 0.4 MiB; the modules end 137 MiB above `0x2000000000` (`main` 113 MiB, `subsdk0` 7 MiB, `sdk` 15 MiB), well below the
   kernel region.
2. **Boot** (done, 2026-11-02). `runtime/native.c` holds the four transitions, in assembly: `native_run`
   (`rt_call` and `rt_original`: host registers saved, the guest's loaded from its `Cpu`, a jump to `c->pc`),
   `native_return` (guest code returning to `RT_RET_HOST`), and the host-function and SVC gates, which store the
   guest registers in the `Cpu`, run the unchanged host function or `rt_svc` on the host stack, and load them back.
   The slot block is the `Cpu` itself: `TLS_SLOT_APP` points at it and the key page holds the offsets of `x[18]`,
   `tpidr`, `tpidrro` and the new `gate` field, so guest x18 and the TLS registers are where the host code already
   reads them. The host-function window becomes executable, a two-word stub per host function at the address the
   loader writes into the GOTs. The loader (`runtime/loader.c`, `RT_NATIVE`) copies each rewritten image over its
   module, writes the key page, reads the SVC sites and seals text and patch area executable. On the Thor the game
   boots to the title menu, and the save loads and plays outdoors (scripted input) at 30-34 fps uncapped in
   performance mode, about what the lifted build reaches there: the critical path still has 26 ms of running a frame,
   and the main thread uses about 9 ms of CPU a frame. Spin waits run without their gates and asynchronous
   preemption is off, which milestone 3 adds.
3. **Play** (done, 2026-11-02).
   - Spin waits: the rewrite sends the back-edge of each of the 881 spin waits in `recomp/patches-v0.txt` through the
     SVC gate (`tools/native/patch_nso.py` `site_spin`, checked by `structck.py`), and the runtime runs
     `rt_spin_gate` there, as for lifted code.
   - Patch sets: the 25 one-word sites (`patchsites.tsv`) are written into the code while their set is on, also when
     a set switches during play (`native.c` `native_patches_sync`, from `patches.c`).
   - Asynchronous preemption: an owner stopped in guest code gives its core up inside the interrupt handler, on its
     signal stack, and returns to where it stopped with its registers untouched (`PLAT_INTR_INLINE`); priority
     inheritance and Horizon's preemption disable are on again (they were off with no lifted code).
   - Crashes in guest code are reported with the guest pc, registers and frame chain (`plat_crash_guest`).
   - On the Thor, 30 minutes outdoors on the save (scripted input, the Android profile's 30 fps): no stop, no hang,
     29.3 fps over the second half (1% lows 19-23 fps), memory mappings level at about 35,000 of 65,530, and 101,615
     preemptions taken in guest code. Textures are re-uploaded 22 times per 60 frames (2.4 MiB), where the lifted
     build with placed pools has none: to look at in milestone 4.
4. **Measure** (done, 2026-11-02; performance mode, 120 Hz, uncapped, asynchronous replay, the save outdoors).
   - CPU: 30 s per build, per-thread CPU time over frames presented. Native: 36 fps, main thread 6.9 ms a frame,
     the five game workers 4.5-6.1 ms. Lifted: 40 fps, main thread 8.2 ms, workers 5.9-7.2 ms. Native execution
     takes 15-25% off the game threads' CPU; the frame rate is within the runs' spread (30-40 fps either way).
   - Neither build keeps its game threads busy (the main thread uses about 7-8 ms of a 25-28 ms frame), yet the
     critical path counts 15 ms of it as running. An off-CPU profile of the native main thread finds the gap: of its
     wall time, 41% guest waits (SVCs, core given up), 36% `nvnSyncWait`, and inside that 21.5% (about 6 ms a frame)
     after the fence was reached, in `vk_be_cpu_synced` -> `vk_texture_write_back_small`. That copies small
     GPU-rendered targets back to guest memory (TotK reads its grass map from CPU memory without an invalidate),
     one GPU copy and fence wait per target, each queued behind the GPU's current frame, with the core still held.
   - With that write-back off (`wb_fence_limit = 0`, a diagnostic: grass cutting needs it) the frame rate does not
     move (35.4 fps): the critical path's running time falls from 26.7 to 14.1 ms and its time waiting for the GPU
     rises from 1.1 to 8.7 ms. The write-back overlaps a wait the game makes anyway.
   - So the frame is bound by the graphics pipeline, not by game code: the game waits for GPU fences whose completion
     includes the replay (10-11 ms of CPU on the replay thread, 7.5 ms on the recorder) and the GPU (17-19 ms).
     Half render scale takes the GPU from 19.4 to 17.1 ms and adds about 1 fps: most GPU time does not scale with
     resolution.
   - Texture re-uploads are no native regression: the lifted build re-uploads 27-29 a 60-frame window under the same
     settings, the native build 6-8 in its 30-minute run.
   - The replay, measured (2026-11-02, `trace = perf`: `vk_sync.c` logs each submission's path through the pipeline):
     a submission waits 5-6 ms for the replay thread (the game hands two over back to back), is replayed in 12 ms,
     and completes 8.6-9.1 ms after its last part reached the GPU: about 27 ms from the game's submit, the frame
     interval. Every fence the game waits for is the submission before the newest. The replay thread is idle two
     thirds of the time and spends its busy time on CPU: 68% preparing draws (`prepare_draw`, of which
     `bind_resources` 26%), the recorder ring 6.6%, uniform snapshots 5.7%. The recorder's ring was never full.
   - The runtime had taken CPUs 6 and 7 (an A710 and the X3) from every thread: `perf.c` `cpu_placement` wrote the
     mask of the thread that presents first, which Android had limited to 0-5, to all threads even with one-per-core
     off. It now leaves the affinity alone; the frame rate did not move. With CPU 7 free the scheduler still puts the
     replay there about 5% of the time, and keeping the replay on it gave 11.9 ms against 12.1-12.3 ms: the replay is
     not bound by the core's clock, so the pin was not kept.
   - Next, in order: the replay's latency and throughput (the GPU starts a frame's work only as the replay hands
     parts over), the write-back as copies recorded with the work that renders those targets (no extra GPU round
     trip at the fence), then GPU work that does not scale with resolution ([android-plan.md](android-plan.md)).

## What to expect

- Speed: game code at native speed is about a third of today's 22-23 ms on the critical path, but the runtime part
  (host functions, the guest kernel, 14% of those threads' time) and the kernel's share (13%) stay. A critical path
  of roughly 15 ms is plausible, which leaves the GPU's 19 ms a frame (performance mode) as the limit: about 50 fps, and
  60 only with GPU work as well ([android-plan.md](android-plan.md), item 3 of the open list).
- Correctness: the original code runs, so the lifter's bug classes go away (FP and NEON semantics, exclusives,
  jump tables); new ones come from the rewrite and the trampolines, and the differential tests against Unicorn do not
  cover them. MacNativeGames' verifiers check the rewritten images.
- Cost: about 2,000 lines of new or adapted code, mostly in milestones 1 and 2, and a second CPU back end to keep
  working beside the lifter.

## Risks

- The slot base: a TLS slot another library takes later would corrupt guest registers silently; the start-up and
  thread-creation checks must catch it.
- Signal handlers of other libraries (ART's fault handler runs before ours through libsigchain) see guest stack
  pointers. They do not see a guest x18 or TPIDR_EL0, as the rewrite leaves those to the host.
- The patch area must stay within branch range of every site (±128 MiB), which holds for these module sizes.
- MacNativeGames' rewriter is a moving reference: pin the commit, as `tools/vendor/` does for the extractors.
