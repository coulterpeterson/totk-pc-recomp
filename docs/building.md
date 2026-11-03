# Building from source

Windows 10 or later, an x86-64 CPU with SSE4.2, and a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`. The
default build targets x86-64-v3 (AVX2/FMA); a CPU without them builds with `--cpu v2` instead and runs slower (see
"Build"). Allow about 60 GB for the work folder. Extraction wants about 55 GB free while it runs and keeps about 35 GB
afterwards.

Players who only want to play can use the [installer](installer.md) instead: it builds the game on their machine
without this toolchain.

## Toolchain

Versions are the ones the build was verified with; newer patch releases are fine.

| Tool | Version | Install | Used for |
|---|---|---|---|
| MSYS2 UCRT64 | clang/lld 23.1.3 | msys2.org installer to `C:\msys64`, packages below | compiling the generated C and the runtime |
| Visual Studio Build Tools | 2026, C++ workload, Windows SDK 10.0.26100 | winget `Microsoft.VisualStudio.BuildTools` with `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended` | the linker .NET NativeAOT calls |
| .NET SDK | 10.0 | winget `Microsoft.DotNet.SDK.10` | the shader translator (`totk_xlate.exe`) |
| Vulkan SDK | 1.4.363 | winget `KhronosGroup.VulkanSDK` | validation layers, `vulkaninfo` |
| uv | 0.13 | standalone installer from astral.sh | the two Python environments |
| hactool | 1.4.0 @ `1d64a83` | `.venv\Scripts\python tools\build_hactool.py`, see below | NCA and update extraction |

MSYS2 packages, after `pacman -Syuu` (run it twice; the first pass updates the core and exits):

```sh
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-{clang,lld,ninja,gcc,make,pkgconf,sdl3,opus,libvpx,zstd,vulkan-devel,\
vulkan-memory-allocator,vulkan-validation-layers,shaderc,spirv-tools} base-devel git unzip
```

`ffmpeg` from MSYS2 is a GPLv3 build and is never linked; the movies go through libvpx.

PATH:

- Add `C:\Program Files (x86)\Microsoft Visual Studio\Installer` to the user PATH: NativeAOT needs `vswhere.exe`, or
  the publish fails at the link step (MSB3073).
- Another `cmake` or `ninja` earlier on PATH (a 32-bit winlibs MinGW, for example) is picked up by accident. The build
  scripts call Ninja from `C:\msys64\ucrt64\bin` themselves; from a shell, put `C:\msys64\ucrt64\bin` first.

### hactool

`tools/build_hactool.py` fetches SciresM/hactool at `1d64a83` into `toolchain\src\hactool`, applies the three patches in
`tools/hactool/` and builds a static `toolchain\hactool\hactool.exe` with MSYS2 UCRT64 gcc (a few seconds):

- `portable.patch`: the parts of MacNativeGames' `build_hactool()` that run on Windows, the `MNG_TITLE_KEYS` variable and
  the two BKTR fixes in `nca.c` for the subsection table padding and the subsection layout of newer cumulative updates;
- `unicode.patch`: a manifest with `<activeCodePage>UTF-8</activeCodePage>`, so paths under a non-ASCII user profile
  reach it intact;
- `mbedtls-asm-clobbers.patch`: the `memory` and `cc` clobbers the bundled mbedtls 2.6.1's x86-64 multiply asm lacks.
  Without them a clang build (the Linux release) fails every RSA signature check and the update merge stops.

hactool reads `-k <prod.keys>`; MSYS2 shells set `HOME`, which hides its default key location, so pass `-k` there.

## Build

From the repository root, in PowerShell. Every step that depends on the game version takes it as `<ver>`: `v143` for
1.4.3, `v121` for 1.2.1 or `v110` for 1.1.0. The tools default to `v121` when `--version` is left out. Saves load on any
of them.

1. Python environments (once). `.venv` runs the lifter and the build; `.venv-py314` runs the extraction scripts, which
   need the stdlib `compression.zstd` of Python 3.14:

   ```powershell
   uv venv --python 3.12 .venv
   uv pip install --python .venv/Scripts/python.exe -r tools/requirements.txt
   uv venv --python 3.14 .venv-py314
   uv pip install --python .venv-py314/Scripts/python.exe -r tools/requirements-py314.txt
   ```

2. Copy `config.example.toml` to `config.local.toml` and set the paths to your base game, the update and your console's
   keys file. `config.local.toml` is ignored by git. The tools read the keys file themselves.

3. Extract and verify. This writes `build\extracted\v0` (1.0.0) and `build\extracted\<ver>` for each update in
   `updates`, identified by its hash; a hash mismatch stops the run. With another `[work] dir` in `config.local.toml`,
   `extracted` goes there instead, and `build_game.py` reads the ExeFS and RomFS from there:

   ```powershell
   .venv-py314\Scripts\python tools\extract.py
   ```

4. Shader translator (no game files; again after a change to its sources, and `tools\make_release.py` stops on an older
   one). This publishes `build\xlate\totk_xlate.exe`:

   ```powershell
   .venv\Scripts\python tools\build_xlate.py
   ```

5. Lift the modules to C, write the RomFS overlay (the frame-rate fix), then compile and link
   `build\<ver>\totk.exe`:

   ```powershell
   .venv\Scripts\python recomp\gen.py --exefs build\extracted\<ver>\exefs --out generated\<ver> --patches recomp\patches-<ver>.txt
   .venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver>
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```

   `--cpu v2` targets x86-64-v2 (SSE4.2, no AVX2/FMA) for older CPUs; the default `v3` needs AVX2. The v2 build links
   `runtime/fma.c`, a correctly rounded software fma the FP lowering needs, and runs slower.

   Lifting takes about 15 s, a clean compile about 200 s on 24 threads; later builds recompile only what changed.
   A compiler cache in front of clang shortens it further: with `SCCACHE` set to an sccache executable, `build_game.py`
   writes it into the ninja file ahead of `cc`, and every lift rewrites the generated chunks with the same contents, so
   a second build of the same version hits all of their objects:

   ```powershell
   $env:SCCACHE = "path\to\sccache.exe"
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```
   `--patches` builds in the frame-rate, resolution and other patch sets, switchable at run time. The link copies
   `SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to the exe.
   For Xbox button prompts, run `make_romfs_patches.py` twice more, with `--prompts xbox` and `--prompts xbox-label`:
   they write `build\<ver>\romfs-patches-xbox` and `romfs-patches-xbox-label` ([controls.md](controls.md), "Xbox
   button prompts"), which `build_game.py` copies next to `totk.exe` like `romfs-patches`.

6. Prepare the shaders (once per build folder and translator). It needs `refs\MacNativeGames` (`tools\fetch_refs.sh`)
   for the shared shader list:

   ```powershell
   .venv-py314\Scripts\python tools\prepare_shaders.py build\<ver> --version <ver>
   ```

   Without this step the first launch compiles shaders and pipelines during play and stutters until the caches fill.
   The AMD driver keeps its pipeline cache per exe path and holds a limited amount: preparing another build folder can
   evict this one's. `totk.exe --warm-shaders` rebuilds it from the shader cache.
   When the driver's shader compiler crashes on a variant (NVIDIA 615.78.08 on an RTX 5080 does, issue #30),
   `prepare_shaders.py` runs the driver step again with `--warm-shaders`, up to three times, without the variants the
   driver was building at the crash. `shader-prepare-skip.txt` next to the exe lists them with the GPU and driver
   version; they build at their first draw instead, another driver version builds them again, and so does deleting the
   file.

7. Run. The game keeps running when its window loses the focus (`assume_focus = 1` in `totk.ini`, the default; the
   Game tab switches it). With 0 it pauses, as it does on the Switch when Home is pressed.

   ```powershell
   build\<ver>\totk.exe
   ```

Settings live in `totk.ini` next to `totk.exe` ([totk.ini.example](../totk.ini.example) lists them). Saves go to
`build\profile` for `v121` and `build\profile-<ver>` for other versions (`build_game.py --profile` changes it).

### Portable folder

`.venv\Scripts\python tools\package_build.py --version <ver>` writes `build\<ver>-portable`, which runs on another of
your machines without MSYS2, Python or .NET. `romfs.bin` stays out unless you pass `--with-romfs`; otherwise put it in
`romfs\` next to the exe or point `TOTK_ROMFS` at its folder. Run `totk.exe --prepare-shaders` once in the copied
folder. The folder holds code and data derived from your game: for your own machines only.

## Linux (in progress)

The runtime builds and runs on x86-64 Linux with a window, input and menu on SDL3 (`*_sdl.c`) in place of the Win32
files (`*_win.c`); `runtime/platform/linux.c` implements `platform.h`. Status: the selftest and the probes pass; 1.2.1
plays at 60 fps on RADV (RX 7800 XT) and NVIDIA (RTX 3060); 1.4.3 builds, probes and plays on RADV (RX 6600, and the
Vega 7 of a Ryzen 5 4600G at about 34 fps). Long play sessions and the Steam Deck itself are not verified yet.

Requirements:

- Linux 6.7 or later: the write watch uses userfaultfd write protection with `PAGEMAP_SCAN`. SteamOS 3.8 qualifies.
- A Vulkan 1.3 driver with `VK_EXT_external_memory_host` (RADV, NVIDIA).
- Packages on Arch:

  ```sh
  pacman -S --needed clang lld ninja make sdl3 opus libvpx zstd vulkan-headers vulkan-icd-loader \
      vulkan-validation-layers shaderc spirv-tools dotnet-sdk uv git
  ```

  Vulkan Memory Allocator is a single header (AUR `vulkan-memory-allocator`). Without it, `build_game.py` fetches the
  version the Windows release pins (3.4.0) into `toolchain/vma/` on first use and checks its hash.

The steps are the Windows ones with POSIX paths: `.venv/bin/python` in place of `.venv\Scripts\python`, and `totk` in
place of `totk.exe`. `build_game.py` takes clang, lld and Ninja from PATH (or the ones in `<dir>/bin` with
`--toolchain <dir>`) and links against the system libraries, with `libstdc++` static and an `$ORIGIN` rpath;
`build_hactool.py` builds `hactool` with make. `tools/prepare_shaders.py` reads the shader list from
`refs/MacNativeGames` at `origin/HEAD`, which must be the pinned commit with its history (the list format changed later).

The probes open a window, so from an SSH session they need the desktop session's display, or they stop at `gpu 0
skipped: no graphics+compute+present queue family`: on Wayland `XDG_RUNTIME_DIR=/run/user/$(id -u)
WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland` (the socket name is in `$XDG_RUNTIME_DIR`), on X11 `DISPLAY=:0`.

Memory: on Windows the GPU imports guest memory and `GetWriteWatch` tracks the CPU's writes to it. On Linux,
re-protecting imported memory invalidates the driver's userptr buffer on every submit (about 1.3 ms per 16 MiB on
amdgpu), and importing pins the pages, which drops their write protection. Each CPU-visible pool therefore gets a
host-visible copy (shadow storage, `runtime/vk_memory.c`): the pages the CPU wrote go in before each submit, and the GPU's
writes come back at its fence. In play that costs about 0.2 ms a frame and doubles the memory of those pools (about
1.1 GB). `TOTK_SHADOW=1` turns it on under Windows too. The Steam Deck's RADV has `VK_EXT_external_memory_host` as
well: the shadow copies are chosen for the measured cost above, not for a missing extension.

The Deck's GPU (Van Gogh) has no native ASTC, so its textures go through the compute decoder (`runtime/vk_astc.c`,
`TOTK_ASTC`) as on desktop AMD GPUs without it; frame times measured there include it.

## Android (in progress)

The game builds into an APK for arm64 Android 13 or later on a PC or Mac, from the 1.0.0 base game, 1.2.1 or 1.4.3.
Nothing is compiled on the device. The APK holds code compiled from your game: install it on your own devices only.
Plan and status: [android-plan.md](android-plan.md); analysis: [android.md](android.md).

Status: the game runs headless (`gpu = null` in `totk.ini`) inside the app on an arm64 Android 16 emulator, 90 s
into the world with its save written, and `--selftest`, `--probe-mem` and `--probe-kernel` pass there. The Vulkan
path has run on no Android GPU yet: the emulator's SwiftShader lacks `VK_KHR_push_descriptor`. On a GPU without BC
formats (Qualcomm's own driver) the game's BC textures are decoded on the CPU at upload (`runtime/tex_bc.c`; the log
says `no BC formats`), at four to eight times their memory: the title screen alone loads dozens of 3686x3686 BC1
textures, 69 MiB each decoded (the log's `BC on the CPU` line counts it), so a 5.5 GB container ran out before the
title. `TOTK_FORCE_NO_BC=1` takes that path on any GPU. Loading Mesa Turnip, which has BC formats, through libadrenotools is the planned
better answer. On aarch64 Linux with Mesa's software driver (lavapipe) the game renders its title screen, which checks
the whole graphics path on ARM short of an Adreno.

Requirements on the build machine (verified on an Apple M4 Mac):

- Android SDK with platform 35 and build-tools, NDK r30 (`sdkmanager "ndk;30.0.16248370"`), a JDK, `adb`.
- Docker with arm64 Linux containers, for the shader translator (`tools/android/xlate.Dockerfile`).
- The Python environments, `config.local.toml` and `tools/extract.py` as above. `tools/build_hactool.py` builds hactool
  on macOS too.

Steps, with `v0` (1.0.0) as the example version:

1. Libraries for Android (SDL 3.4.18, Opus, libvpx, zstd, Vulkan headers, VMA) into `toolchain/android/arm64-v8a`:

   ```sh
   .venv/bin/python tools/build_android_deps.py
   ```

2. Lift with the Android address layout (modules at 0x2000000000, below the 39-bit address space's busy top), and
   compile and link `build/v0-android/libtotk.so` with the NDK:

   ```sh
   .venv/bin/python recomp/gen.py --exefs build/extracted/v0/exefs --out generated/v0-android --target android --patches recomp/patches-v0.txt
   .venv/bin/python tools/build_game.py --version v0 --gen generated/v0-android --target android
   ```

3. The shader translator for Android (`build/xlate-android/totk_xlate`), in an arm64 container with the NDK's
   `toolchains/llvm/prebuilt/<host>` folder mounted (its sysroot and runtime libraries are the same on every host). In a
   git worktree, mount the main checkout's `.git` as well:

   ```sh
   docker build --platform linux/arm64 -t totk-xlate-android -f tools/android/xlate.Dockerfile tools/android
   docker run --rm --platform linux/arm64 -v "$PWD:$PWD" -w "$PWD" \
       -v "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64:/ndk:ro" totk-xlate-android \
       sh -c 'git config --global --add safe.directory "*" && python3 tools/build_xlate.py --android /ndk'
   ```

4. The APK, `build/v0-android/totk.apk`, then install it and push the game data (the 17 GB `romfs.bin` once) into
   `/storage/emulated/0/Android/data/com.totkrecomp/files`:

   ```sh
   .venv/bin/python tools/build_apk.py --version v0
   .venv/bin/python tools/install_android.py --version v0
   ```

The APK's launcher icon is the game's own: `tools/extract.py` takes it from the base game's Control NCA into
`<work>/extracted/v0/control/`, and `tools/build_apk.py` packs it (without it the APK has Android's default icon).

Start "TotK Recomp" on the device. The log goes to logcat (`adb logcat -s totk`), settings to `totk.ini` in the data
folder. The Android device profile (`runtime/cfg.c`) runs the game in handheld mode at 30 fps. On the controller,
Minus and Plus together (Select and Start on most Android pads) open the settings menu, and L3 and R3 together cycle
the performance overlay: off, frame rate, full panel ([controls.md](controls.md)). A probe or another command line
goes through the launch intent; quote it for the device's shell:

```sh
adb shell "am start -n com.totkrecomp/.MainActivity --es args '--probe-kernel'"
```

A GPU driver of your own, such as a Mesa Turnip build for Adreno GPUs packaged for Android emulators, goes into
`gpu_driver/` of the data folder (`tools/install_android.py --gpu-driver FILE`; `--gpu-driver system` removes it). The
app copies it to its private storage and loads it through libadrenotools (`TOTK_VK_DRIVER`, `runtime/gfx_window_sdl.c`
driver_load), which `tools/build_android_deps.py` builds. On the AYN Thor (Adreno 740) Turnip v26.0.0 R8 renders the
game cleanly where Qualcomm's driver 0676.53 draws black tiles over moving characters.

Shaders are translated on the device the first time they are met (the translator runs from the APK's native library
folder), so the first minutes stutter until the cache in the data folder fills.

## Checks

Before a commit:

- `python tools/check_content.py --staged`: nothing game-derived is staged. CI runs the same check;
  `git config core.hooksPath .githooks` runs it before each local commit.
- `build\<ver>\totk.exe --selftest`: layout, dispatch table and host-function registry, without game files.

A change to the button prompts (`tools/make_romfs_patches.py --prompts`, `tools/button_icons.py`) passes
`.venv-py314\Scripts\python tests\romfs_prompts_test.py`, which runs the tool on a RomFS it makes itself (no game
files); a change to the settings passes `totk.exe --probe-cfg`.

The lifter (`recomp/`) and the CPU headers (`runtime/cpu.h`, `runtime/simd.h`) are checked against Unicorn. Every lifter
change passes these, all run with `.venv`:

| Test | Checks | Game files |
|---|---|---|
| `tests\diff\lift.py` | instructions sampled from the extracted modules; `--coverage` lists every word the lifter rejects or traps | yes |
| `tests\diff\simd.py` | the whole FP/SIMD encoding space | no |
| `tests\diff\atomic.py` | exclusive load/store sequences and ARMv8.1 atomics | no |
| `tests\diff\func.py` | whole functions as the generator emits them, loops, jump tables and calls included (calls go to a stub that checks the registers at the call) | yes |
| `tests\flags_check.py` | the function-local flag helpers of `runtime\cpu.h` against its packed ones | no |
| `tests\excl_mt.py` | the exclusive monitor helpers under contention and ABA, on several threads | no |
| `tests\jumptables_check.py` | `recomp/jumptables.py` against a Unicorn oracle; must report 0 disagreements, on 1.4.3 too | yes |

A `--cpu v2` build passes the same differential tests with `--cflags=-march=x86-64-v2`; they compile `runtime/fma.c`
too, so they check the software fma the v2 build links rather than the C runtime's.

`tests/mem_probe.py [--target pc|android] [--aarch64]` builds `runtime/mem_probe.c` with `memory.c` and `platform/linux.c`
on their own (no game files, no SDL) and checks the address-space reservations and write tracking: written pages, a file
read, a frozen region, four writers against a draining reader, and the VMA count. `TOTK_WW=mprotect` forces the
page-protection tracking that older kernels (Android before 6.7) use. The probe is also `totk --probe-mem`.

Native execution on Android ([native-execution.md](native-execution.md)): the game's own ARM64 code instead of the
lifted C. The rewritten modules, the library without generated code (`-DRT_NATIVE`), its APK, and the images in the
data folder's `native/` beside `exefs/`:

```sh
.venv/bin/python tools/native/rewrite.py --version v0 --verify
.venv/bin/python tools/build_game.py --version v0 --gen generated/v0-android --target android --native build/v0-native
.venv/bin/python tools/build_apk.py --version v0 --out build/v0-android-native
.venv/bin/python tools/install_android.py --version v0 --out build/v0-android-native --no-data
adb shell mkdir -p /storage/emulated/0/Android/data/com.totkrecomp/files/native
adb push build/v0-native/*.patched.bin build/v0-native/*.tsv /storage/emulated/0/Android/data/com.totkrecomp/files/native/
```

Diagnostics for GPU work (`[diagnostics]` in `totk.ini`): `trace = gpu` logs GPU time per pass every 120 frames
(`vk_gpuprof.c`: a timestamp at every full barrier, the table keyed by each pass's targets), one frame in 1200 segment
by segment with every draw timed on its own, and the game's barriers by their NVN bits. `trace = perf` adds, per draw,
the Vulkan commands the recorder carried. `driver_env = NAME=value;NAME=value` sets environment the GPU driver reads
when it loads, such as Turnip's `TU_DEBUG` or ir3's `IR3_SHADER_DEBUG` (shaders the driver cached keep the old
options: point `TOTK_PIPELINE_CACHE` at a new file in the same setting).

On an AArch64 host, `--probe-exec` checks what running guest code natively needs: executable memory at a fixed
address, x18 kept around a call, and the counter and thread registers at EL0 ([native-execution.md](native-execution.md)).

Runtime probes, each a mode of `totk.exe` (the list is at the top of `runtime/main.c`): `--probe-kernel`,
`--probe-vkexec`, `--probe-nvn-tex` and `--probe-shader` cover most runtime changes. `--probe-shader` and the draw
part of `--probe-vkexec` translate shaders of the census below: they look for `build/<ver>/shaders/jobs.json` of the
exe's own version first, then of the other one.

Shaders: `.venv-py314\Scripts\python tools\shader_census.py --version <ver> --translate` extracts every Maxwell shader
to `build\<ver>\shaders\` and runs it through the translator and `spirv-val`; failure groups go to `failures.txt`.
`tools\xlate_smoke.py` checks the translator alone, without game files.

## Reverse engineering (optional)

- Ghidra 12.1.2 with Adubbz's Ghidra-Switch-Loader in `toolchain\ghidra\` (JDK 21) opens the game modules (NSO).
- `tools\fetch_refs.sh` clones the upstream projects this port builds on into `refs\`, read-only.
