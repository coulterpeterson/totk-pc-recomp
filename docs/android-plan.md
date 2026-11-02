# Android ARM64 port: plan for the AYN Thor Max

Final draft after one review pass. Target: AYN Thor Max (Snapdragon 8 Gen 2: 1x Cortex-X3, 2x A715 + 2x A710 in one
2.8 GHz cpufreq policy, 3x A510; Adreno 740; 16 GB RAM; Android 13, likely a 5.15 GKI kernel). Build host: Apple M4.
Marks: (verified) checked in code, by a compile or in a primary source; (unverified) still to check on the device.

## Status (2026-11-01)

Done, and how it was checked:
- Guest acquire/release ordering made seq_cst (`cpu.h`, `lifter.py`): codegen checked for x86-64 and aarch64, the atomic
  differential test passes on both.
- The fork's aarch64 work merged; TotK 1.0.0 added as a game version (boot checkpoints, patch table, spin waits).
- NDK build of `libtotk.so` (API 33), APK packaging without Gradle, data install over adb, the Android device profile,
  and the shader translator built for bionic and shipped in the APK (docs/building.md, "Android").
- aarch64 Linux (Docker on the M4): the probes pass and the game runs 120 s headless into the world.
- arm64 Android 16 emulator, inside the app: `--selftest`, `--probe-mem`, `--probe-kernel` pass; the game runs 90 s
  headless (3526 frames, 56 threads, the save written).

- Graphics on ARM: the title screen renders on arm64 Linux with Mesa's lavapipe, also called directly as a driver
  library (`TOTK_VK_DRIVER`, the route a Turnip build takes on Android through `gpu_driver/`). On the Android 16
  emulator Vulkan comes up through volk and a library copied from `gpu_driver/` loads from private storage.

- On the AYN Thor (Adreno 740, Qualcomm driver 0676.53, Android 13), stock driver, version 1.0.0: boots, renders the
  title screen and the prologue upright and full screen, and plays at a steady 30 fps in the opening cave (scripted
  input, `tools/input/load_save.txt` plus a held stick): 18 of 20 four-second windows had every frame at 33 ms, the
  other two 7 and 9 frames at 50 ms (pacing, no pipeline or shader waits). GPU about 20 ms a frame, render thread about
  18-20 ms, working set 3.1 GB, VRAM 2.6 GB. The driver has BC formats, so the CPU BC fallback is not used there.
  Fixes it needed: shaderInt64 no longer required, dynamic vertex input off on Qualcomm's driver (skinned models were
  streaks), the swapchain in the identity transform (the panel is portrait) and rebuilt after the resize to immersive
  mode, the process ended when the game returns, TMPDIR set, resumable data pushes for wireless debugging.

- Outdoors on the player's own save (an Eden 1.4.2 save; its header's version set to 1.0.0's, `0x0046C3C8`, it loads):
  Qualcomm's driver 0676.53 draws black tiles over moving characters and trees that change from frame to frame and
  crashed once in its pipeline compiler; neither BC decoding, the subgroup size nor the feedback-loop path explains
  them. With the player's Turnip v26.0.0 R8 in `gpu_driver/`, loaded through libadrenotools, the frame is clean.
- Speed there, Turnip: 20 fps in the game's whole-vsync steps (every frame 3 vsyncs). No thread is busy (the busiest
  39% of a core after the window thread's SDL polling was fixed). The frame is replay (about 17 ms) then GPU (about
  27 ms) one after the other, about 43 ms, rounded up to 50. Asynchronous replay (`replay_async = 1`) frees the game's
  submitting thread but does not shorten that chain; the real-time step gives about 23 fps but needs the equipment
  cooldown fix 1.0.0's patch table lacks.

- The 20 fps came mostly from the game's own choice: its frame interval follows the time between its GPU timestamp
  reports, which the completion thread stamped when the GPU had finished their submission. Stamped at replay time
  (`gpu_time = record`, the Android profile's default) the game asks for two vsyncs on every frame: 23-28 fps outdoors,
  GPU about 24-27 ms. Asynchronous replay adds nothing there (alternating runs: 25.8/24.5 sync, 25.8/23.5 async).
- What is left of a 39 ms frame is the game's threads waiting on each other, not any thread being busy: the main
  thread uses about 30-40% of a core with about 300 context switches and 1400 page faults a frame (the mprotect write
  watch; Android 13's kernel 5.15 has no asynchronous userfaultfd), on the performance cores at their full 2.8 GHz
  (standard power holds the prime core at 1.84 of 3.19 GHz). Opening 64 KiB a fault instead of 4 KiB cut the faults by
  only 12% and doubled the replay's uploads, so it was dropped. Runs of the same save differ by up to 4 fps.

- Placed pools (VK_EXT_map_memory_placed on Turnip) work since the write watch tracks them in 64 KiB units: the stop
  was the process running out of memory mappings (65530), because the kernel never merges the driver's mappings
  after page-sized protection changes. Five minutes outdoors on the player's save, second half counted: placed pools
  29.9 fps (33 of 36 four-second windows at 30), no texture re-uploads, replay 11.6 ms, GPU 21.1 ms, 3.0 GB; shadow
  storage 26.7 fps, 54 re-uploads (50 MiB) every 60 frames, 4.0 GB. `hang_report = <s>` logs every guest thread's
  wait and host stack when presents stop.
- Uncapped (`fps_limit = 60`, `realtime = 1`) it stays near 30, also with mailbox present (27.8 fps), so the frame
  really takes about 34 ms of work, not a vsync step. Measured on 2026-11-02:
  - The presenting thread's "host" time was half a wait: an off-CPU profile put 47% of its time in
    vkGetSemaphoreCounterValue (Turnip's timeline lock, which the completion thread's wait holds). It now reads the
    completion thread's value; the wait is gone, the frame rate is not changed.
  - The critical path now (`tools/crit_path.py`): the replay 12.1 ms, then the game's main thread 11.5 ms and its
    workers about 8 ms of game code, one after the other. Asynchronous replay does not shorten it (28.6 fps): the
    game's main thread waits for its render thread every frame.
  - The presenting and game threads run on the A715/A710 cores at 2.7-2.8 GHz, never on the X3 (1.84 GHz in standard
    mode) or the A510s; they wait for a CPU less than 3% of the time. The recorder waits for a CPU 8% of the time.
  - The GPU runs at 550 MHz, 74% busy, 21-23 ms of part time a frame.
  - Write-watch faults on the game's threads are down to about 150/s on the busiest (placed pools); the game threads'
    time is 71% our code (lifted game code and the runtime), 17% kernel, 6% clock_gettime (a clock read per SVC and
    per spin-wait pass).

- Game code speed (2026-11-02): the main thread's protection faults came from the write-watch queries re-arming the
  heap beside the pools; tracking only the pools' pages (`plat_mem_watch_only`) took its faults from 17800 to 2500 a
  second and its CPU from about 10.5 to 9.5 ms a frame. Keeping 16 guest registers in C locals instead of 6
  (`gen.py --reg-locals 16`) made no difference measurable in alternating runs (main thread 2.70-2.92 s per 10 s
  either way), so it stays at 6. At 30 fps the main thread uses about 9.2 ms of CPU a frame and the replay 10-12 ms:
  no thread alone is over 16.7 ms. The frame is the chain between them (the presenting thread waits for the main
  thread, which waits for its workers and for virtual cores), and on the 60 Hz display every frame between 16.7 and
  33.3 ms shows as 33.3: the frame rate cannot show a gain until the whole chain fits 16.7 ms.

- At 120 Hz (2026-11-02; the game reads the panel's 120.000 Hz): 29.0 fps with `fps_limit = 60` and the real-time step,
  30.4 with asynchronous replay added, 30.2 with `fps_limit = 40`, four minutes each, no stops. The game's present
  interval stays at 2 throughout, but the critical path shows that as a result, not a cause: of the 33.0 ms between
  presents, 27.3 ms is guest threads running game code (the main thread 12.3 ms, then five of its workers 2.5-4.2 ms
  each, one after the other), 2.6 ms waits for a virtual core, 1.8 ms wake latency and 0.6 ms sleep. The replay is off
  the path and the game does not wait for a vsync. Holding the game's interval controller at 1 would not shorten the
  frame. 60 fps needs the game code on that chain about twice as fast.
- What those six threads spend their time on (12 s of cpu-cycles with call graphs, the threads named by the critical
  path's recording, same settings): 63% lifted game code, 14% our runtime (`rt_dispatch` 4%, `hostfn_entry` 2%,
  `vslow_s` 2%: vector float ops with a NaN lane redone lane by lane, which an ARM host could compute natively), 13%
  kernel, 7% clock reads (mostly the `trace = sched` accounting the measurement itself turns on). No guest function is
  above 1.5%. Per instruction, 29% of the lifted code's samples are loads and stores of the guest registers in the
  `Cpu` (general registers 17%, vector registers 9%, FPCR and the rest 3%): registers outside a function's six locals,
  syncs at calls, and every vector register, which never has a local. `Cpu *restrict c` changes no instruction (clang
  does not use it against guest pointers made from integers once `c` has escaped to a call). The lifted code is 163 MiB
  for 91 MiB of guest modules, code and data together: about 3x the instructions of the original.
- So a cheaper lifter can take perhaps 10-15% off the chain (vector registers and FPCR in locals, native NaN lanes,
  a faster `rt_dispatch`), which makes 35-40 fps at 120 Hz possible, not 60. Twice as fast needs the game's own ARM64
  code to run as it is (native execution, "2. CPU" item 5), and 60 fps also needs the GPU's 21 ms under 16.7.
- First lever, measured: on an AArch64 host the FP helpers now run the host's own instruction for add, subtract,
  multiply, divide, square root, fused multiply-add, min and max, scalar and vector (`runtime/simd_fp.h` FN(hw)), with
  the guest's RMode, FZ and DN in the host FPCR (`runtime/cpu.h` rt_fpcr_sync): no FPCR load and no NaN check per op,
  and NaN lanes no longer redone lane by lane. `tests/diff/simd.py` passes on aarch64 (the full run and passes over the
  FP families with 48 cases an encoding). Two alternating pairs of runs (`trace = perf,sched`, a crit recording at
  world 150 s): game threads' running time on the critical path 26.12 and 26.70 ms against 26.55 and 26.90 ms before,
  30 fps either way. About 1%, within the runs' noise.
- What else is on the critical path besides running: waits for a virtual core 2.0-2.5 ms, wake latency 1.8-2.0 ms,
  the main thread in host calls 1.4-2.0 ms. 40 fps (25 ms) needs about 8 ms off the 33.
- Those waits, measured (2026-11-02): about 40 address-arbiter and 18 lock hand-offs a frame, median wake latency
  22 us, and 65% of them end within 50 us of the waiter going to sleep. The core waits come from the Switch's three
  cores: the other p44 game threads and a p32 thread hold them meanwhile. The host time is all `nvnSyncWait`: the main
  thread waiting for the GPU. `wait_spin = 50` (`runtime/kernel.c` spin_unlocked: spin for the wake, then for G,
  without the futex call for a spinning waiter) took wake latency from 2.21 to 1.27 ms a frame and core waits from 2.79
  to 2.52 (three alternating pairs), but the game threads' running time rose from 26.0 to 27.1 ms and the interval
  stayed 33.4 ms. It stays a setting, off by default.
- In every configuration so far the interval settles near 33.3 ms, and what the critical path saves in one kind of
  time reappears in another.
- Clocks and heat (six minutes each, 120 Hz uncapped, asynchronous replay, sampled every 4 s): in standard power mode
  the mid cores hold 2.7-2.8 GHz (their maximum) at up to 91 C, the X3 stays at 1.84 of 3.19 GHz and the GPU at 550 of
  680 MHz, 72-77% busy, and every 60 s window is 29.8-30.7 fps: nothing throttles. In performance mode the X3 runs at
  3.19 GHz and the GPU at 615-680 MHz, 67-80% busy, and the windows are 34.5-37 fps (one late window 29.9, with the
  clocks unchanged: the scripted walk reaches a heavier view). Critical path there: interval 28.5-28.9 ms
  (34.7-35.2 fps), game threads running 22.1-23.2 ms, GPU 19.0 ms, replay 9.7 ms. `wait_spin = 50` gives nothing in
  performance mode either (34.3 and 34.1 fps against 35.2, 34.7 and 34.7).
- So 40 fps needs about 4 ms off the chain in performance mode, and 60 fps needs the chain and the GPU both under
  16.7 ms. For play, 30 fps with the game's own pacing stays the default: uncapped frames between 25 and 33 ms pace
  unevenly, and the real-time step needs the equipment cooldown fix 1.0.0's patch table lacks.

Open, in order:
1. 40 fps in performance mode: about 4 ms off a 28.8 ms chain (the main thread's 8-10 ms of game code, the replay's
   9.7 ms off the chain only with asynchronous replay), and 1.0.0's equipment cooldown fix for the real-time step.
   For 60 fps: native execution of the guest code ("2. CPU" item 5, scoped in [native-execution.md](native-execution.md)),
   a separate project, and the GPU's 19 ms.
2. The replay (10-12 ms, off the chain with asynchronous replay): per-draw work, the recorder's ring and drains.
3. GPU: images created mutable without a format list (likely no UBWC compression on Turnip), the full barrier before
   every pass, clears and discards as load and store ops, render scale.
4. 1.0.0's equipment cooldown fix; performance mode with the main and presenting threads on the X3.
5. Start-up texture streaming on the presenting thread; the second screen's focus; a save importer.

## 0. Starting point

- Upstream closed Android as out of scope (`valentderah/totk-pc-recomp#161`, an Odin 2 Portal Max with the same SoC):
  the port needed `VK_EXT_external_memory_host`, which Turnip does not have (verified, Mesa `features.txt`).
- Fork `Astrr00/totk-pc-recomp-android`, branch `claude/android-native-port-6irzy5`, 5 commits on upstream 0.2.8
  (we are on 0.3.1): `docs/android.md` analysis; the aarch64 Linux runtime build (FPCR sync and `simd.h` rounding
  on aarch64, `plat_pause`, `ov_movie.c`, `sha256.c`); Android module bases (0x2000000000) in `layout.py` and
  `host.h`; no `plat_reexec` on Android; an mprotect write tracker in `linux.c`; `VK_EXT_external_memory_host`
  optional under shadow storage; a Vulkan probe APK. Not yet run on a phone.
- Our tree, as it is:
  - `cpu.h` has an `__aarch64__` branch, but it was written for the asm SIMD mode: in the `simd='c'` mode the build
    uses, `rt_fpcr_sync` is a no-op and `simd.h` is excluded. The fork's `cpu.h` commit fixes this (verified).
  - `simd='asm'` exists in `lifter.py` but is not wired: `gen.py` always builds `Module(..., 'c')` and its `Emitter`
    raises on an asm instruction; no test covers it; its batches read `c->x[]` and `c->nzcv` from memory, which the
    register and flag locals of `regs.py` and `flags.py` keep elsewhere (verified). It is a generator project.
  - DMB/DSB lower to a seq_cst fence (fine). LDAR/STLR lower to C11 acquire/release (`cpu.h` `lda##N`, `stl##N`),
    which does not keep a guest `stlr; ldar` pair in order: clang with RCpc (`-mcpu=cortex-x3`) emits `ldapr`, which
    may pass the `stlr`, and the C model lets the compiler reorder them too (verified by compiling). x86 has the same
    store-to-load hole today (both are plain `mov`). Fix: lower LDAR/STLR to `__ATOMIC_SEQ_CST`.

Decision: rebase the fork's 5 commits onto our main (authorship kept), contact Astrr00 to share the work, and keep
every Android change behind `__ANDROID__`, `--target android` or new files, so upstream merges stay cheap.

## 1. Expectations

Eden on Android already runs the game's CPU code natively (NCE), and its shaders come from the same kind of
translator. The recomp still detiles textures, tracks dirty pages and keeps mirrors. What it can do better:
- no Maxwell command emulation: NVN commands replay straight into Vulkan;
- shaders translated ahead of time and pipelines warmed at first start, so no compile stutter;
- game patches: frame-rate fix, dynamic resolution, `render_scale`;
- frame pacing done on purpose (section 5).
What it cannot do: run guest CPU code faster than native. If Eden is GPU-bound on the Adreno 740 at the user's spots,
the main gains are less stutter and steadier pacing, not a higher average. Milestone 0 measures which it is.

## 2. CPU

Baseline: lift to C (`simd='c'`), compile for aarch64 with the NDK. Order of work:
1. Correctness first: LDAR/STLR to seq_cst (`recomp/lifter.py`, `cpu.h`), then `tests/diff/atomic.py` and
   `tests/excl_mt.py` on aarch64. Fiber switch asm in `linux.c` (x19-x30, d8-d15, FPCR, sp), `kern_ap_stub` for
   AArch64, `intr_handler` on `uc_mcontext.pc`/`.sp`, `fault_handler` reading ESR from `esr_context`,
   `sched_setaffinity` in place of `pthread_setaffinity_np`.
2. Host-code ordering audit: x86's TSO hides missing barriers in hand-written runtime code. Check cross-thread flags
   and rings (`kernel.c` preemption flags and `ap_tlr`, `vk_rec.c` ring, `vk_backend.c` replay queue, the
   preemption check in `dispatch.c` `rt_dispatch`), then run the kernel probe and `excl_mt.py` under ThreadSanitizer.
3. Flags: `-march=armv8.2-a+lse` until item 1 lands (LSE makes the CAS-based exclusive monitor one instruction);
   `-mcpu=cortex-x3` only after it, or with `+norcpc`.
4. Speed levers, after milestone 5 shows the CPU binds:
   - AAPCS64 lets lifted functions take guest x0-x7 as C arguments, or use `preserve_none`, instead of syncing
     through `Cpu` at every call: the `regs.py` cost is the sync at calls, not the register count.
   - `simd='asm'` wired through `gen.py` with spills of register and flag locals around batches, tests extended.
   - PGO on a recorded play session.
5. Native execution of guest code (MacNativeGames' model, as Eden's NCE does on this SoC), or a hybrid of native
   guest code with this runtime's HLE and Vulkan back end, is the alternative to measure at milestone 5, not a
   separate project. NCE retires most of the Android unknowns the fork lists against it (x18, TPIDR, W^X, seccomp).

Core placement: three virtual cores on the X3 and two mid cores, found by MIDR (`CPU part` in `/proc/cpuinfo`: X3
0xd4e, A715 0xd4d), not by frequency, which does not tell A715 from A710; replay, recorder and shader threads on the
remaining mid cores; an `APerformanceHint` session (API 33) for the core threads, with vendor support (unverified).

## 3. Build host (macOS) and packaging

- Extraction with MacNativeGames' extractor (Mac-native); check `tools/extract.py` and `build_hactool.py` on Darwin.
- Lift with `gen.py --target android` into `generated/<ver>-android/` (`layout.py` decides the bases).
- Compile with NDK r28+ clang, `--target=aarch64-linux-android33`. A shared library relocates `<mod>_fn_ptrs[]`
  (hundreds of thousands of pointers; the Linux build uses `-no-pie` to avoid that): link with RELR
  (`-Wl,--pack-dyn-relocs=relr`), hidden visibility or `-Bsymbolic` for runtime symbols.
- Shader translator built for `osx-arm64` so `prepare_shaders.py` and `pack_shader_cache.py` run on the Mac.
- APK: SDL3 `android-project` (SDLActivity), a small loader `libmain.so` that `dlopen`s the game library from
  internal storage. Android 10 forbids `execve` from app data, not `dlopen` (docs; unverified on device). This
  avoids rebuilding and reinstalling the APK per game build and the size question of a very large `.so`; pushes go
  through `adb push` + `run-as`. Fallback: the game library inside the APK.
- A translator on the device needs an extracted file to `execve`, so it conflicts with
  `extractNativeLibs="false"`: use the pre-built shader pack; only if misses matter, ship `libtotk_xlate.so` with
  extraction on.
- The APK and the game library contain game-derived code: built on the player's Mac, never published.

## 4. Memory

- Address space (fork, kept): modules at 0x2000000000, kernel and host-function windows above, the 6 GiB stack/heap
  and 64 GiB alias blocks below, reserved in `JNI_OnLoad` of the loader; a clear error in place of `plat_reexec`.
  Check `/proc/self/maps` of a real app process on the Thor (39-bit VA: ART and the driver live near the top).
- Pools: shadow storage is the default (about 1.1 GB extra, affordable on 16 GB; about 0.2 ms a frame in
  `docs/building.md`). The cost to attack is write tracking, which stays needed with any pool scheme: textures are
  deswizzled from guest memory and re-uploaded by page write generation (`vk_texture.c`, `vk_memory.c`), and
  `mem_freeze` depends on it too.
- Tracking, decided by data: count distinct written pages per frame with the existing `mem: dirty` census on the PC
  build. Few pages: the fork's mprotect tracker, after timing one fault round-trip inside an app process (ART's
  sigchain handles every SIGSEGV first). Many pages: card marking in the `ST*` macros of `cpu.h`/`simd.h` (one store
  per guest store, no signals, no VMA splits; host writers mark explicitly; lifter change, differential tests).
  userfaultfd write-protect only if a later kernel and the SELinux policy allow it.
- Zero-copy aliasing, Turnip only, later: `VK_EXT_map_memory_placed` maps the GPU allocation at the guest address with
  cached, IO-coherent memory (verified in Mesa `tu_knl_kgsl.cc`). Not dma-buf + `MAP_FIXED`: exported KGSL buffers
  come from `/dev/dma_heap/system` without the IO-coherent and cached flags. Saves the 1.1 GB and the copies, keeps
  texture tracking.
- The stock Qualcomm driver: assume no `external_memory_host` and no `map_memory_placed` until the probe says
  otherwise; it gets shadow storage. Guest pools must sit in a HOST_CACHED memory type, or guest code reading them
  crawls on write-combined memory.

## 5. Graphics and presentation

- Drivers: the stock Qualcomm driver and Mesa Turnip loaded through libadrenotools (BSD-2), with a picker in the
  launcher. Turnip has BC, GPL, `map_memory_placed` and `headless_surface` (verified). The stock driver probably has no
  BC (yuzu patched it in through adrenotools' bcenabler); a compute BC decoder (fork's design) only if the stock driver
  must work without it.
- Check the whole required list on both drivers, not BC alone: Vulkan 1.3, push descriptor, `depthClipControl`,
  `shaderStorageImageWriteWithoutFormat`, `geometryShader`, `shaderInt64`, timeline semaphores, dynamic rendering,
  and the memory types.
- ASTC is native on Adreno, so the compute ASTC decoder is skipped.
- Frame pacing, which is what "choppy" usually means: present at 30 fps on the Thor's high-refresh panel through
  Swappy (Android Frame Pacing) or `VK_GOOGLE_display_timing`, with `ANativeWindow_setFrameRate`. Milestone 4, not
  polish.
- Tilers: the replay's pass restarts for feedback loops (`VK_FEEDBACK_RESTART`) and full barriers cost more on Adreno.
  Measure per pass (`TOTK_FLIGHT`, AGI or Snapdragon Profiler) and fix the top ones: load/store ops, fewer pass
  splits, scoped barriers.
- Resolution: `render_scale` about 0.67-0.75 of 1600x900, FSR 1 to the 1080p panel.
- Shaders: the pack from the Mac plus `--warm-shaders` in the foreground at first start, with progress, restartable.
- Lifecycle: on resume the window is new, so recreate the `VkSurfaceKHR` and the swapchain; set
  `SDL_HINT_ANDROID_BLOCK_ON_PAUSE`; pause guest cores in the background. The low-memory killer targets a backgrounded
  ~8 GB process, so the game may be killed when the user switches apps.

## 6. Device integration

- Game data (about 20 GB) in the app's external files dir on internal UFS, not the microSD card: the RomFS is
  mmapped and read at random. App-specific dirs bypass FUSE on Android 12+.
- Input: the built-in controller is an Android gamepad (SDL3). Audio: SDL3 on AAudio. Settings: `totk.ini` in the
  data dir (no env vars on Android).
- Later: the bottom screen through an Android `Presentation` for the menu and the perf overlay.

## 7. Milestones

| # | Milestone | Where | Done when |
|---|---|---|---|
| 0 | Facts: profile Eden on the user's save (CPU- or GPU-bound); stock and Turnip caps and memory types (fork's probe APK + libadrenotools); `uname -r`, page size, `/proc/self/maps` of an app; one SIGSEGV round-trip timed in an app; distinct written pages per frame on the PC build | Thor, PC | a table in `docs/android.md` that picks the paths in sections 4 and 5 |
| 1 | aarch64 correctness: the fork rebased, LDAR/STLR seq_cst, fibers, `kern_ap_stub`, signals, ordering audit | M4 VM for Unicorn tests; NDK executable over `adb shell` on the Thor | diff tests and `jumptables_check.py` (0 disagreements on 1.2.1 and 1.4.3) pass on aarch64; kernel probe and `excl_mt.py` pass on the Thor; TSan clean |
| 2 | NDK build of the whole game as an `adb shell` executable, headless (`gfx_null.c`), then with a Turnip headless surface | Mac, Thor | guest boots to the first `nn::` calls with the PC's log; first GPU timings with no APK work |
| 3 | APK: SDLActivity, loader `libmain.so`, `dlopen` from internal storage, `JNI_OnLoad` reservation, data dir | Thor | the same boot inside an app process |
| 4 | Graphics bring-up: surface, shadow pools and the chosen tracker, shader pack, driver picker, frame pacing, lifecycle | Thor | title screen, a save loads, paced 30 fps output |
| 5 | Measure against Eden, same save and spots | Thor | frame-time logs, CPU or GPU bound per scene; decide on section 2 item 4 vs item 5 (native/hybrid) |
| 6 | Speed: the levers milestone 5 picks, tiler fixes, core placement, ADPF, Turnip aliasing | Thor | stable 30 fps in the field areas |
| 7 | Polish: launcher, second screen, mod and cheat parity, 30-minute thermal run | Thor | a 1-hour session without a crash |

## 8. Risks

- Lifted CPU code on the X3 + two mid cores may stay under 30 fps in busy areas; milestone 5 decides between generator
  work and native execution.
- If Eden is GPU-bound, the recomp's average frame rate gain may be small; pacing and no stutter remain.
- Stock driver gaps and bugs; Turnip is the escape, and Eden users on this device already use it.
- `vm.max_map_count` and fault cost if the mprotect tracker is used heavily.
- Upstream drift: a fork-level effort with no upstream support.
