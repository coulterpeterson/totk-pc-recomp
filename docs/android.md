# Android port: analysis

What a native Android build (aarch64, bionic, NDK, SDL3 Activity) needs, from the code as it stands. Nothing here is
built or measured on a phone. A statement marked (assumption) is not backed by code in this repository or by a
measurement; check it before relying on it. The existing Linux host (`runtime/platform/linux.c`, the `#else` branches of
`tools/build_game.py`) is the starting point: Android is a second Linux target, not a third OS layer.

## Recommendation

- CPU: keep lifting to C and compile it with the NDK for aarch64 (option a). Do not run guest ARM64 natively (option b).
- Generate on the PC, never on the phone. A PC-side script lifts, cross-compiles and packs a private APK the player
  installs with adb. No recompiled code is distributed.
- Hard parts, in order: BC texture support on GPUs without it, the 39-bit address space, write tracking for the GPU
  upload, shader translation without .NET on the device, RAM.

## 1. x86-64-only parts and their aarch64 equivalents

| Part | Where | aarch64 equivalent |
|---|---|---|
| FP control register (MXCSR) | `platform/fp_x86.h`, included from `platform.h` under `__x86_64__`; `cpu.h` `rt_fpcr_sync` (`__builtin_ia32_stmxcsr`) | Add `fp_aarch64.h` with `mrs/msr fpcr`. `cpu.h` already has an `__aarch64__` branch (`rt_get_fpcr`, `rt_set_fpcr`) that reads the host FPCR directly. |
| Rounding mode and FTZ in the portable FP lowering | `simd.h` `simd_rm_set_rc`, `simd_rm_enter`, `simd_rm_rz`, `simd_rm_leave` (`_mm_getcsr`, `_mm_setcsr`); `#include <immintrin.h>` at the top | Replace with FPCR RMode bits 23:22 and FZ bit 24. These are the guest's own bits, so on an aarch64 host the MXCSR translation (`rc[]` table) goes away. |
| FTZ tininess differs from ARM | comment above `FREDO` in `simd_fp.h` (x86 detects tininess after rounding, ARM before) | Gone on aarch64: host FZ is the guest's FZ, the `FN(slow)` fallback under `FPCR_FZ` is not needed there. Keep it behind `#ifdef __x86_64__`. |
| `"+x"` asm constraint | `simd_fp.h` `FN(vflush)` (xmm register) | `"+w"` on aarch64. |
| `_mm_pause` | `platform.h` `plat_pause`, `vk_rec.c` (three loops, includes `<immintrin.h>` directly) | `__asm__ volatile("yield")`. `vk_rec.c` should call `plat_pause`. |
| 128-bit exclusive pair via cmpxchg16b | `cpu.h` `ldxp128`, comment names CX16 | Native on aarch64 (LDXP/STXP, or LSE CASP); clang lowers `__atomic` on `__uint128_t` to it. No change. |
| Fibers | `linux.c` `plat_fiber_swap` and `plat_fiber_entry` (x86-64 asm: rbx, rbp, r12-r15, MXCSR, x87 CW) | New asm: x19-x30, d8-d15, FPCR, sp. bionic has no `makecontext`/`swapcontext` (assumption), so hand-written asm stays the route, as it is now. |
| Thread interruption (async preemption) | `linux.c` `intr_handler` reads and writes `gregs[REG_RIP]`, `gregs[REG_RSP]`; `kernel.c` `kern_ap_stub` (two asm versions, Win64 and SysV, saves GPRs, flags, YMM0-15, MXCSR) | Use `uc_mcontext.pc` and `.sp`. New `kern_ap_stub` saves x0-x17, x30, NZCV, FPCR, v0-v7 and v16-v31 and jumps back through `kern_ap_rip()`. Skip x18 (bionic's shadow call stack register, assumption). No red zone on AArch64 Linux, so the 136-byte skip of the SysV stub is not needed (assumption). Signal number `SIGRTMIN + 4` is fine: bionic's `SIGRTMIN` already skips its reserved signals (assumption). |
| Fault handler decode | `linux.c` `fault_handler`: `gregs[REG_ERR]` bits 1 and 4 tell write and execute faults | aarch64 has no `REG_ERR`; read ESR from the `esr_context` record in `uc_mcontext.__reserved` (WnR bit 6 gives write). The write test decides whether `memory.c` `thaw_fault` runs, so this is on the critical path. |
| Crash report | `fault_handler` uses `dladdr` | Present in bionic. Unchanged. |
| SHA-256 | `sha256.c` x86 SHA-NI path under `__x86_64__`, with a portable path beside it | Portable path is already selected. Optional: ARMv8 SHA2 intrinsics. |
| NV12 interleave | `ov_movie.c` (`_mm_loadu_si128`, `_mm_unpacklo_epi8`) | No guard visible in the lines read; add a NEON or scalar path. |
| CPU check | `main.c` `check_cpu` demands AVX2/FMA/BMI2 | Check for ASIMD, and for what the chosen `-march` needs. `sysinfo.c` already guards `<cpuid.h>`. |
| Compile flags | `build_game.py` `GEN_FLAGS`, `RT_FLAGS` carry `-march=x86-64-v3` | `--target=aarch64-linux-android<api>` with `-march=armv8.2-a+crc` or similar (assumption: nearly every phone since 2019 has v8.2). Generated code uses `musttail` (`gen.py` `MUSTTAIL`), which clang supports on aarch64. |
| Link | Linux branch of `build_game.py`: `-no-pie`, an executable, `-lvulkan -lSDL3 ...` | Android needs a shared library `libmain.so` loaded by SDLActivity: `-shared -fPIC`, `-llog -landroid`, `-lvulkan` from the NDK sysroot, no `-lpthread`. Lifted code addresses are immediates, so PIC costs nothing in guest addresses; `dispatch.c` keeps host function offsets in 32 bits against an `anchor`, which holds while the image stays under 4 GiB (`TABLE_SPAN_LIMIT` is 1 GiB of table). |
| Glibc-isms in `linux.c` | `pthread_setaffinity_np`, `posix_spawn(p)` of `xdg-open`, `/sys/class/drm`, `/sys/class/hwmon`, `execv("/proc/self/exe")` in `plat_reexec`, `getrandom` | `pthread_setaffinity_np` is missing in older bionic (assumption): use `sched_setaffinity(gettid())`. `plat_open_folder`, `plat_font_path`, `plat_clipboard_set` need Android stubs or SDL calls. `plat_reexec` cannot work (see 3). |

The lifted C itself (`gen.py`, `lifter.py`) emits no x86 code with `simd='c'`, which `Module.__init__` defaults to and
`gen.py` always uses. Vector code goes through GCC/clang vector types (`simd.h` `SIMD_VT`, `SHUF`, `CVT`), which
clang lowers to NEON. `tests/diff/*.py` build `Lifter(base, 'c')` only.

An alternative exists: `Lifter(base, 'asm')` ("AArch64 hosts" in the `lifter.py` docstring) passes FP and SIMD
instructions through as inline asm, bit-exact by construction. Nothing exercises it at TotK scale in this repository:
`gen.py` has no flag for it and the differential tests do not cover it. Treat it as a later speed option, not as the
baseline.

## 2. CPU strategy

**(a) Lift to C, compile for aarch64.** Everything downstream already works: the lifter, jump tables
(`recomp/jumptables.py`), patches, `rt_dispatch`, the kernel model (`kernel.c`), the differential tests against Unicorn.
Changes are in section 1, none touches the lifter's integer and control-flow output. Cost: compile time and size, and
the lifted code runs slower than native guest code (assumption: 2-4x per instruction; the real number is unknown until
an aarch64 build runs). On Windows the full compile takes about 200 s on 24 threads (`docs/building.md`, step 5), so
it is a PC-side job on any machine.

**(b) Run the guest's ARM64 natively, as MacNativeGames does on macOS.** Required: rewrite every site where guest code
touches the OS (68,968 in 1.4.3 `main`, `docs/architecture.md`, "Coverage oracle"): SVC, `mrs` of TPIDR_EL0 and the
counters, `x18`, exclusive windows. On Android specifically:
- TPIDR_EL0 is bionic's thread pointer (assumption); guest TLS reads must be rewritten, as the lifter does with
  `c->tpidr` (`lifter.py` system register table).
- x18 is the shadow call stack register in bionic's own code (assumption). Guest code uses x18 freely. Every host call
  needs a trampoline that swaps it, and a signal handler that interrupts guest code runs with the guest's x18.
- The code must be mapped executable from anonymous memory after patching (W^X): permitted for the app domain in
  principle (ART's JIT does it) but not verifiable here (assumption).
- Guest and host share one register file. Async preemption, the 3-core model and `rt_spin_gate` (`kernel.c`, `patches.c`
  `spin`) would all be rebuilt around raw signals.
- Native speed is the gain. The whole port's testing (Unicorn differential harness) does not apply.

**Recommendation: (a).** The repository's value is the lifter and its oracle; (b) is a separate project with a separate
bug surface and more Android-specific unknowns (x18, executable anonymous memory, seccomp). Speed is the one reason for
(b). Decide it with data after milestone 4: if the lifted game on a phone's big cores is far below 30 fps and the
profile (`tools/prof_report.py`, `tools/crit_path.py`) shows lifted code dominating, try `simd='asm'` before (b).

## 3. Memory

What the code requires:
- Guest address equals host address, all below 2^39 (`host.h` `RT_ADDRESS_LIMIT`; `layout.py`, `ADDRESS_LIMIT`).
- Modules at `FIRST_BASE = 0x7000000000` (448 GiB), kernel region 0x7010000000, host function window 0x7020000000
  (`host.h`), all reserved first in `memory.c` `mem_init` with `plat_mem_reserve` (`MAP_FIXED_NOREPLACE`,
  `linux.c`).
- A 6 GiB stack and heap block searched from 4 GiB upward in 1 GiB steps, and a 64 GiB alias region above it, both
  `PROT_NONE`, `MAP_NORESERVE`, write-watched (`memory.c` `LOW_SIZE`, `ALIAS_SIZE`).
- `linux.c` `plat_mem_reserve` also maps a 128 MiB `pstate` byte map (`PS_LIMIT / PG`), `MAP_NORESERVE`.

Android problems:
1. **Address space.** Phone kernels commonly use 39-bit user space (512 GiB) (assumption; some use 48). With 39 bits,
   mmap places ART, libraries, thread stacks and driver mappings top-down from the top, and 0x7000000000 is the top
   64 GiB, where those land. `mem_init` answers a taken range with `reexec_or_fail` -> `plat_reexec` -> `execv` of the
   executable. That cannot work: an Android app process is forked from zygote, and the exec of `/proc/self/exe` would
   run `app_process`, not the app. Fix: a new module base chosen for Android in `recomp/layout.py` (`FIRST_BASE`;
   AGENTS.md: that file alone decides bases, and the lifter bakes absolute addresses into the C, so the Android build
   is a separate lift) at a low, quiet range such as 128-256 GiB, and the 6 GiB and 64 GiB blocks below it. Check with
   `/proc/self/maps` on target devices; keep the search loops in `mem_init`, but fail with a message instead of
   re-executing. Reserve in a constructor or `JNI_OnLoad`, as early as possible.
2. **Limits.** `RLIMIT_AS` must be unlimited for 70+ GiB of `PROT_NONE` reservations (assumption: it is on stock
   Android; some OEM builds may differ). `vm.max_map_count` is 65530 by default; `memory.c` freezes idle 2 MiB regions
   with `mprotect` (`mem_freeze`) and commits and decommits at 4 KiB granularity, which splits mappings. Count VMAs
   in play (`/proc/self/maps` lines) before trusting it.
3. **Write tracking** (replaces `MEM_WRITE_WATCH`). `linux.c` uses a userfaultfd in async write-protect mode and
   `PAGEMAP_SCAN` (`ww_open`, `plat_mem_written`), both Linux 6.7. Android GKI is 6.1 (Android 14) and 6.6
   (Android 15); 6.12 on the newest (assumption), so most phones lack it. Without it `plat_mem_written` returns
   `UINT64_MAX` and the back end copies whole pools every submit: unusable with shadow storage (about 1.1 GB of
   pools, `docs/building.md`). Options, cheapest to build first:
   - *mprotect tracking.* Watched pages are kept read-only after each scan. The SIGSEGV handler (the same
     `plat_mem_fault_hook` slot `memory.c` already uses for `thaw_fault`) sets a dirty bit, makes the page writable
     and returns. `plat_mem_written` reads the bits and re-protects. Kernel writes into guest memory (file reads) fail
     with an error, not a fault; `mem_thaw` exists for that. Cost per first write of a page per frame is a fault
     (assumption: several microseconds); the frame's count of written pages decides if it is acceptable. Also
     splits VMAs (point 2).
   - *Card marking in the lifted code.* Every guest store goes through `ST8`...`ST64`, `ST128` and the atomic
     helpers in `cpu.h` and `simd.h`. Setting a byte per page in a dirty table there needs no signals and no VMA
     splitting; costs one store per guest store, and host code that writes guest memory (IPC, file reads,
     `memcpy` in HLE functions) must mark explicitly. Generator change in `lifter.py`, so it needs the differential
     tests rerun.
   - *Soft-dirty bits* (`/proc/self/clear_refs`, pagemap bit 55): needs `CONFIG_MEM_SOFT_DIRTY` on arm64, which I
     believe older kernels lack (assumption). Probe at runtime, do not plan on it.
   - Keep the userfaultfd path first in order, then fall back. A userfaultfd syscall may also be denied to apps by
     seccomp or SELinux (assumption; ART's own use of userfaultfd suggests it is allowed on 14+).
4. **Transparent huge pages**: `linux.c` calls `madvise(MADV_NOHUGEPAGE)` on watched ranges; harmless.
5. **RAM.** `memory.c` `DEFAULT_POOL` is 0xCD500000 (about 3.2 GiB application pool) plus
   `res_heap_extra_bytes()`; `HEAP_CAP` 4 GiB. On top: Vulkan textures and render targets (UMA, same RAM), shadow
   copies of CPU-visible pools (about 1.1 GB, `docs/building.md`), the dispatch table (about the size of the guest
   `.text`, `dispatch.c` header comment), the lifted code (file-backed, evictable), the shader cache. My estimate
   is 6-7 GiB resident (assumption), so 12 GiB devices are comfortable, 8 GiB marginal under Android's low-memory
   killer, 6 GiB not realistic. zram helps with idle pages only. A foreground app can be killed without warning:
   save handling is the game's, but the process must not hold pages it can drop (`plat_mem_decommit` is
   `MADV_DONTNEED`, fine).
6. `plat_file_map` maps the RomFS read-only (`linux.c`, `MAP_SHARED`); fine on Android for a file the app can open.

## 4. Graphics

Required by code (`vk_device.c`):
- Vulkan 1.3 (`pick_device`: devices below 1.3 are skipped).
- Extensions `k_required_ext`: swapchain, push descriptor, `VK_EXT_depth_clip_control`. `VK_EXT_external_memory_host` is
  optional (milestone 2, below).
- Features in `create_device`: timelineSemaphore, dynamicRendering, synchronization2,
  shaderStorageImageWriteWithoutFormat, depthClipControl, shaderInt64, samplerAnisotropy, independentBlend,
  geometryShader, **textureCompressionBC**.
- Optional and handled: GPL (`VK_EXT_graphics_pipeline_library`, dropped if `vk_caps_gpl` refuses), extended dynamic
  state 1-3, vertex input dynamic state, custom border colour, conditional rendering, transform feedback,
  provoking vertex, robustness2, fragment shader interlock, attachment feedback loop (`g_vk.feedback` falls back
  to pass restart), device fault, memory budget, float64 (`vk_caps.c`).

Likely availability on phones. None of this was checked here (assumption); read the Vulkan Hardware Database and run
`vulkaninfo` on each target before choosing devices.
- Vulkan 1.3 itself: Adreno 7xx and 8xx, Mali-G7xx Valhall (5th gen) on current drivers, Xclipse 920 and later.
  Older Adreno 6xx and Mali-G5x/G7x Bifrost drivers often stop at 1.1/1.2.
- `VK_EXT_external_memory_host`: Turnip (open Mesa) has it; for Qualcomm's and Arm's proprietary drivers I do not know.
  No longer a requirement (milestone 2 below); the probe app logs it per device.
- `VK_EXT_graphics_pipeline_library`: Adreno recent drivers (assumption), Mali newer drivers (assumption). Optional.
- **BC textures: the largest gap.** `textureCompressionBC` is demanded, and `vk_formats.c` maps the game's BC1-BC7
  formats straight to Vulkan BC formats. Adreno reports it; Mali Valhall mostly does not (assumption). The existing
  fallback goes the other way (`vk_astc.c`, `vk_formats.c` note: no native ASTC -> decode and encode to BC3). Without
  BC the port needs the mirror: BC1-5/6H/7 decoded by compute into RGBA8 or re-encoded to ASTC, plus the
  copy and mip paths in `vk_texture.c` that assume block sizes. Memory cost of RGBA8 is large, so ASTC 4x4
  re-encoding or `VK_FORMAT_BC` only on GPUs that have it, with a clear "unsupported GPU" message for the rest.
- ASTC native (`textureCompressionASTC_LDR`, `g_vk.has_astc_ldr`) is a plus, as stated: ASTC sources upload as they
  are. It does not remove the BC need because most game textures are BC (assumption; count the RomFS texture formats).
- Mobile GPUs are `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`; `vk_mem_class_log` then chooses "uma". Shadow storage
  (`shadow_pool`, `TOTK_SHADOW`, default on for non-Windows) needs no host-pointer import. So the hard
  `VK_EXT_external_memory_host` requirement can be relaxed when shadow mode is on: `k_required_ext` and
  `import_pool` become conditional. On UMA, shadow copies are plain extra RAM (section 3.5).
**Milestone 2 as implemented (`vk_device.c`, `vk_memory.c`, `android_main.c`).**
- `VK_EXT_external_memory_host` is optional. `emh_usable` checks it; `missing_required` skips a device without it only
  when shadow storage is off (`vk_mem_shadow_default`: `TOTK_SHADOW`, default on except Windows). `create_device`
  enables the extension when present (desktop behaviour unchanged), else logs `vk: <gpu>: VK_EXT_external_memory_host
  not available ...; using shadow storage`; `vk_mem_set_host_import(0)` then forces `shadow_on` and `mirror_mode`
  logs `mem: ... shadow storage is the only CPU-visible pool path`. `import_pool` already routed to `shadow_pool`.
  `min_imported_host_alignment` stays 0 and is only read on the import path.
- BC stays required. The code has no BC decoder or transcoder (only ASTC to RGBA8/BC3 in `vk_astc.c`, `tex_astc.c`),
  so no honest cheap fallback exists. A device without `textureCompressionBC` is rejected with
  `required feature missing ... textureCompressionBC` and a line pointing here. Not built speculatively.
- Test switches (no cfg row, set and not `0`): `TOTK_FORCE_NO_EMH=1` hides the extension, `TOTK_FORCE_NO_BC=1`
  makes the BC check fail (the last one can only exercise the error path).
- Probe (`android_main.c`) logs one logcat line each: external_memory_host, graphics_pipeline_library (extension and
  feature), textureCompressionBC, textureCompressionASTC_LDR, shaderInt64, maxStorageBufferRange, each memory heap.

**BC fallback design (not implemented; needed when the sweep finds phones without BC).** Per BC texture upload in
`vk_texture.c`, where `tex_vk_format` returns `VK_FORMAT_BC*`: (1) when `!features.textureCompressionBC`, pick the
target by `vk_astc_native`-style probing: ASTC 4x4 if sampled (same 8/16-byte blocks, 4x4 footprint, so the mip and
copy layout in `vk_texture.c` stay valid) else RGBA8 (BC6H to RGBA16F; 4x or 8x the memory). (2) Transcoding by a
compute pass modelled on `vk_astc.c` (`astc_decode_spv.h` path): BC1-5 to RGBA8 are simple per-block decodes; BC7
(8 modes) and BC6H (14 modes) are the bulk. BC to ASTC re-encode is lossy for BC7 and needs an encoder; RGBA8 decode
is exact and is the first step. (3) `TexVkFormat` gets a `bc_decode` flag like `astc_bw`, and `tex_vk_format_supported`
accepts the substitute. Size: BC1-5 ~300 lines of GLSL, BC6H/7 ~600 more. Count BC formats by mip bytes in the RomFS
first, and test against the CPU decode of the same blocks.
- Present: `gfx_present.c` already passes `caps.currentTransform` as `preTransform`. Rotation handling in the
  renderer and the FSR present (`vk_fsr.c`) must be checked on a rotated surface. Surface loss on background must
  destroy and rebuild the swapchain (`gfx_window_sdl.c` handles focus only, no
  `SDL_EVENT_DID_ENTER_BACKGROUND` / `WILL_ENTER_FOREGROUND`).
- Tile-based GPUs: the replay (`vk_exec*.c`) restarts passes for feedback loops (`VK_FEEDBACK_RESTART`) and issues
  full barriers; on tilers this is expensive. Expect GPU time, not CPU, to bind first. Unmeasured.

**Shader translator.** The Ryujinx translator runs as `totk_xlate` (.NET 10 NativeAOT), started by `gfx_shader.c`
(`plat_child_start(xlate_exe, "--serve")`) and looked up beside the executable. Options:
1. *Pre-translate on the PC, ship none at runtime.* `tools/prepare_shaders.py` already makes 29,587 variants for 1.4.3
   and `tools/pack_shader_cache.py` folds them into `shader-cache.pack`; `gfx_shader.c` reads the pack first. Add the
   SPIR-V -> device compile step to the first-run warm (`--warm-shaders`). A shader outside the list (the list is
   MacNativeGames', so complete for a normal playthrough only by assumption) gets "stage is not translated"
   (`gfx_shader.c`) and the draw is skipped. Cheapest; start here. Make the list wider with a PC run that logs
   misses in play.
2. *Translator as a child binary on the device.* .NET NativeAOT for `linux-bionic-arm64` exists as a runtime
   identifier (assumption: usable with the .NET 10 SDK; untested). An app cannot exec files from its data directory
   on current Android (SELinux, assumption), so the binary ships as `libtotk_xlate.so` in the APK's native library
   directory and `plat_child_start` execs that path. Android's `plat_child_start` uses `posix_spawn`, which bionic has.
3. *Reimplement the translator in C.* Large. No.
Do 1; add 2 only if misses matter.

The driver's pipeline cache is per driver (assumption); a driver update invalidates it, so the first-run warm must be
restartable and run in the foreground (Android may kill background work).

## 5. Packaging and game data

Constraints: `AGENTS.md` and `installer.md` keep game data and game-derived code out of the repository and releases.
The installer builds on the player's machine. Compiling the lifted code on a phone is not realistic:
`build_game.py` records 120-180 MiB per 3 MiB chunk and 568 MiB for `main_table.c` at `-O1` (comment above
`JOB_MEM`), and the install needs about 1.3 GB of generated C and objects (`installer.md`, step 6). There is no NDK
on a phone.

Proposed split:
- **PC side (the "Android installer", extends `installer/` and `tools/build_game.py`).** Steps 1-4 of the installer
  run as today with an Android layout and target: extract ExeFS/RomFS, lift, compile with the NDK toolchain,
  link `libmain.so` against a prebuilt runtime library (`--runtime-lib` already splits this), prepare the RomFS
  patches and the shader pack, then assemble an APK: SDL3's Java glue, `libmain.so`, `libSDL3.so`, `libopus.so`,
  `libvpx`, `libzstd`. Sign it with a local debug key. The player runs `adb install`. The APK is the player's own
  build and is never published. This mirrors `--runtime-lib` (CI prebuilds the runtime, the install adds game-derived
  objects).
- **Native library placement.** Android does not let an app `dlopen` code from its writable data directory
  on current targets (assumption), so `libmain.so` has to be inside the APK. Size is unknown until a build exists
  (the Windows exe size is not recorded in this repository); APKs are zip files and 4 GiB per entry is the
  zip limit. If the library is too large, split it into several `.so` files by module (`main`, `subsdk0`, `sdk`;
  `dispatch.c` already tolerates several modules, but all of them must stay within 4 GiB of each other for the 32-bit
  `anchor` offsets) or build with a lower `-O`.
- **Game data on the device.** About 20 GiB after setup (`installer.md`). The Storage Access Framework gives
  content URIs, not paths; native code needs file descriptors. `plat_file_open` takes a path, so add a
  `plat_file_open_fd` or open `/proc/self/fd/N` for the RomFS image (positional reads, `plat_file_pread`, work on a
  descriptor). Two ways to deliver the files:
  - `adb push` to `/sdcard/Android/data/<package>/files/` (app-specific external storage, no permission): simplest;
    Android 11+ hides that directory from file managers, but adb can write it (assumption).
  - An in-app picker with SAF that copies a PC-prepared package into app storage once: needs the same 20 GiB
    free, plus time. The prepared package, not the raw NSP/XCI, because extraction needs `hactool` and the
    Python 3.14 scripts (`tools/extract.py`), which do not run on the device.
  Keys (`prod.keys`) stay on the PC; the Android build never needs them.
- Settings: `cfg.c` reads `totk.ini` and `TOTK_*` variables; Android apps have no shell environment, so use the file.

## 6. Milestones

| # | Milestone | Size | First verifiable test |
|---|---|---|---|
| 1 | aarch64 Linux build (glibc, any ARM Linux box or VM). Items of section 1: FP control, `simd.h` rounding, fiber asm, `kern_ap_stub`, `intr_handler`, `fault_handler`, `plat_pause`, flags, `ov_movie.c`, `sha256.c`. | M | `tests/diff/*.py` (lift, simd, atomic) and `tests/jumptables_check.py` (0 disagreements on 1.2.1 and 1.4.3) pass on the aarch64 host; the probes built from `kernel_probe.c` pass. |
| 2 | Vulkan requirements relaxed behind capability checks: BC optional with a software or compute fallback decision, `external_memory_host` optional under shadow mode, GPL optional (already). A probe APK or `vulkaninfo` sweep over target devices decides which BC path is needed. | M | A desktop GPU run with `TOTK_SHADOW=1` and `TOTK_FORCE_NO_EMH=1` reaches (BC cannot be forced off: no fallback, see section 4) the same frames as before (`--probe-vkexec`, `--probe-nvn-tex`); the sweep lists the extensions and features of each phone. |
| 3 | Android memory layout: new `FIRST_BASE`, no `plat_reexec`, early reservation, mprotect or card-marking write tracking behind `plat_mem_written`. | M | A native test app (SDL Activity, no game) runs `mem_init`, reserves the full layout, writes pages and gets them back from `mem_written_pages`; `/proc/self/maps` shows VMA count. |
| 4 | NDK build and APK skeleton: `build_game.py --target android-arm64`, `libmain.so`, SDL3 Activity, logcat as the log, headless (`gfx_null.c`). | M | Real ExeFS lifted for the Android layout; the app boots the guest to the first `nn::` service calls and prints the same early log lines as the PC build (`totk.log`). |
| 5 | Storage: fd-based RomFS and ExeFS access, PC-side packer, the existing hash checks. | S | On the phone `main.c` start checks verify the RomFS hashes (`expected-v*.json` set), no file path outside app storage. |
| 6 | Graphics bring-up: surface, lifecycle (background, foreground, rotation), swapchain rebuild, shader pack, BC/ASTC path from step 2. | L | Title screen renders with `TOTK_FLIGHT` on; a frame dump (`TOTK_DUMP`) matches the PC dump for the same input within texture-path tolerance. |
| 7 | Playable: touch overlay and gamepad (`input_sdl.c` is keyboard, mouse and SDL gamepad only), audio (SDL3, already), thermal and core placement (`plat_thread_affinity`, the three virtual cores onto big cores). | L | A five-minute walk from a save without a crash, frame rate and frame-time log from the performance overlay (`perfmon.c`), temperature read through Android APIs or `/sys/class/thermal` where allowed. |
| 8 | Speed: measure, then `simd='asm'`, `-O` levels, PGO, shader warm, memory trimming. | L | A profile (`tools/prof_report.py`) before and after on the same save: frame time at 30 fps target on named devices. |

## 7. Risk statement
Likely not achievable, or not on most devices:
- Phones without BC support and without a transcoding path: the game's texture set is mostly BC (assumption). If
  step 2 cannot be done within the RAM budget, those phones are out.
- 6 GiB and smaller devices (the guest pool alone is about 3.2 GiB).
- Full speed. The Switch game is CPU-heavy (three virtual cores with hand-modelled scheduling, `kernel.c`) and the
  lifted code is slower than native; sustained clocks on a phone drop under heat. A stable 30 fps on a flagship is a
  hope, not a result. 60 fps and the PC port's resolution scaling are out of reach on most hardware.
- A byte-exact match of the PC render path on tilers: feedback loops and the full-barrier restart path cost more
  there (`g_vk.feedback`).
- Start-to-finish on-device install. The compile cannot happen on the phone, and the translator may not run there.
- A published APK: it contains game-derived code (`installer.md`).

Unknown until tried: the real VA size and address-space layout on each device, whether `userfaultfd` is allowed to
apps, how many pages the game dirties per frame (decides mprotect against card marking), whether `VK_EXT_external_memory_host`
exists on Qualcomm's and Arm's proprietary drivers, the size of the aarch64 `libmain.so`, and the frame rate. The
cheapest way to learn most of these is milestones 1 to 3, none of which needs game data to run.
