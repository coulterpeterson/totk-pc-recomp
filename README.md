# TotK Recomp for Android

This fork builds [TotK Recomp](https://github.com/valentderah/totk-pc-recomp), the static recompilation of *The Legend
of Zelda: Tears of the Kingdom*, into an APK for arm64 Android handhelds. Its target is the AYN Thor Max (Snapdragon 8
Gen 2, Adreno 740, Android 13), where it plays at 40-48 fps in performance mode. Upstream closed Android as out of
scope, so the work lives here. It builds on the aarch64 groundwork of
[Astrr00/totk-pc-recomp-android](https://github.com/Astrr00/totk-pc-recomp-android).

> [!IMPORTANT]
> **No copyrighted code or data is redistributed here.** This repository holds no Nintendo code, assets, keys or game
> files, and no prebuilt APK. Everything derived from the game is generated on your own computer, from a dump of a copy
> you own, into folders git ignores; a check (`tools/check_content.py`, also run in CI) keeps game-derived files out of
> commits.
>
> The project is for research, interoperability and preservation. Use it only with legal dumps of games you own, made
> from your own console. Instructions for obtaining game files or keys will not be provided.
>
> The APK you build contains code compiled from your game. Install it on your own devices and do not share it.

## Status

- **Game version:** 1.0.0 (the base game) and the 1.4.3 update both play on the device, and both can be installed side by
  side (`tools/install_android.py --version-folder`). 1.2.1 builds too but has not been run on Android. Other updates,
  1.4.2 among them, are not supported: each version needs its own table of code patches.
- **Speed on the AYN Thor Max:** 44-48 fps outdoors in performance mode at the 60 fps cap (about 40 when the device runs
  hot) and 30 fps in standard mode, at correct game speed, with the Mesa Turnip driver. In alternating four-minute runs
  1.4.3 averaged 46.3 fps and 1.0.0 44.0; 1.0.0 with native execution 46.3, and 1.4.3 native matches 1.4.3 lifted. Qualcomm's own driver draws black tiles over moving characters outdoors.
- **Stability:** a 30-minute session outdoors on a real save ran without a stop. Expect visual bugs; the first minutes
  stutter while shaders are translated on the device and cached.

## Features

- The game's own ARM64 code lifted to C and compiled for Android by the NDK; nothing is compiled on the device
- Handheld mode at the game's own 1280x720, scaled to the screen by the present (bilinear or AMD FSR 1)
- Frame rate limit of 30, 60 (the default) or uncapped, with game time following the clock: a frame that misses its
  slot shows late but does not slow the game down
- Ports of the real-time fixes to 1.0.0: the equipment-switch cooldown and the hair and cloth clock
- A Vulkan driver of your choice (a Turnip build in the data folder), loaded through libadrenotools
- The in-game settings menu and performance overlay from the controller: Minus and Plus together open the menu, L3 and
  R3 together cycle the overlay. The menu shows the Android defaults as such and leaves out rows that cannot apply
  (window size, graphics card, DLSS, HDR on a screen without it) or that the game version has no patch for
- The game pauses while the app is in the background, as on the Switch's Home screen
- Save import, export and snapshots from the menu's Game tab: drop an Eden, Ryujinx or JKSV/Checkpoint save into
  `files/save-import`, export to `files/save-export`, and keep up to 50 automatic snapshots to go back to
- Motion controls from the handheld's own gyro and accelerometer when its built-in controller reports none
- Shaders prepared in the background on first play, with a progress bar, so new places do not stutter later
- Mods and Atmosphère cheats as upstream, from the app's data folder (`files/mods`)
- The game's own launcher icon, taken from your dump
- Native execution, which runs the game's own ARM64 code instead of the lifted C, for 1.0.0 and 1.4.3
  ([docs/native-execution.md](docs/native-execution.md)): the same frame rate on the Thor for about 10% less power

## How it got to 40 fps

The first outdoor frames on the Thor ran at 20 fps. Each step below was measured on the device, mostly in four-minute
runs of the same save; the full record is [docs/android-plan.md](docs/android-plan.md).

| Step | Outdoors on the Thor |
| --- | --- |
| Turnip instead of Qualcomm's driver, which drew black tiles; the game in whole-vsync steps | 20 fps |
| GPU timestamps stamped when the replay records them, so the game stops asking for a third vsync | 23-28 fps |
| Placed memory pools (`VK_EXT_map_memory_placed`) with write tracking in 64 KiB units, and only on the pools | 30 fps |
| Performance mode (the X3 core at 3.19 GHz, the GPU up to 680 MHz) with the real-time step | 35 fps |
| A shorter replay walk: the recorder spins before sleeping, RCpc acquire loads (LDAPR), cached snapshot lookups | 36 fps |
| The fence write-back recorded as copies at the end of each submission instead of a GPU round trip per target | 40 fps |

What else had to change for the game to run on Android at all:

- Guest atomics made sequentially consistent, and the lifter's output checked against Unicorn on AArch64.
- An address layout that fits Android's 39-bit address space, with the modules at 0x2000000000.
- Write tracking by page protection, since Android 13's 5.15 kernel lacks the newer write-protect interface.
- The swapchain kept in the panel's own portrait transform and rebuilt after the switch to immersive mode.
- No dependency on `shaderInt64`.
- Dynamic vertex input turned off on Qualcomm's driver.
- The shader translator built for bionic and shipped inside the APK.

What limits it now: a frame needs about 19 ms of GPU time. Half of that is the G-buffer pass's 2,100 draws, and each
draw costs the Adreno about 1.6 µs plus 1.3 µs of state changes, so the GPU is bound by draw count, not pixels.
Several game threads also run one after another on the critical path. 60 fps needs both under 16.7 ms. Native
execution takes 15-25% off the game threads' CPU, but the frame stays bound by the graphics pipeline.

## Building the APK

The build runs on a Mac (tested on an Apple M4) or a Linux PC. The full reference is
[docs/building.md](docs/building.md), "Android".

You need:

- **Android tools:** the Android SDK with platform 35 and build-tools, NDK r30 (`sdkmanager "ndk;30.0.16248370"`), a JDK
  and `adb`.
- **Docker** with arm64 Linux containers, for the shader translator.
- **[uv](https://docs.astral.sh/uv/)** for the two Python environments.
- **About 40 GB free:** the RomFS alone is 17 GB.
- **Your own game files,** dumped from your own console: the base game NSP and the keys file of the console.

1. Python environments:

   ```sh
   uv venv --python 3.12 .venv && uv pip install --python .venv/bin/python -r tools/requirements.txt
   uv venv --python 3.14 .venv-py314 && uv pip install --python .venv-py314/bin/python -r tools/requirements-py314.txt
   ```

2. Copy `config.example.toml` to `config.local.toml` and set the paths to your base game, any supported update and
   your keys file. The file is ignored by git, and the tools read the keys file themselves.

3. Extract and verify your dump. This writes `build/extracted/v0` and checks every file against known hashes;
   `tools/build_hactool.py` first if you list an update:

   ```sh
   .venv-py314/bin/python tools/extract.py
   ```

4. Libraries for Android, then lift the game to C and compile `libtotk.so`. The generated C is about 1 GB, and a
   clean compile takes a while. The last line writes the shader seed the game prepares in the background:

   ```sh
   .venv/bin/python tools/build_android_deps.py
   .venv/bin/python recomp/gen.py --exefs build/extracted/v0/exefs --out generated/v0-android --target android --patches recomp/patches-v0.txt
   .venv/bin/python tools/build_game.py --version v0 --gen generated/v0-android --target android
   .venv-py314/bin/python tools/prepare_shaders.py build/v0-android --version v0 --no-run --romfs build/extracted/v0/romfs --exefs build/extracted/v0/exefs
   ```

5. The shader translator for Android, in an arm64 container, with the NDK's prebuilt toolchain folder mounted. Use
   `linux-x86_64` in place of `darwin-x86_64` on Linux:

   ```sh
   docker build --platform linux/arm64 -t totk-xlate-android -f tools/android/xlate.Dockerfile tools/android
   docker run --rm --platform linux/arm64 -v "$PWD:$PWD" -w "$PWD" \
       -v "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64:/ndk:ro" totk-xlate-android \
       sh -c 'git config --global --add safe.directory "*" && python3 tools/build_xlate.py --android /ndk'
   ```

6. Pack the APK, then install it and push the game data over adb into the app's folder. The 17 GB RomFS goes once, and
   later runs push only what changed. `--gpu-driver` puts a Turnip build for your Adreno into the data folder:

   ```sh
   .venv/bin/python tools/build_apk.py --version v0
   .venv/bin/python tools/install_android.py --version v0 --gpu-driver path/to/turnip.so
   ```

7. Start "TotK Recomp" on the device. Settings live in `totk.ini` in
   `/storage/emulated/0/Android/data/com.totkrecomp/files` and in the in-game menu. The log goes to
   `adb logcat -s totk`.

A save from a later game version loaded after its header's version field was set to 1.0.0's
([docs/android-plan.md](docs/android-plan.md)). There is no tool for that yet.

## Credits

Upstream [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) and everyone it credits below, the aarch64 work
of [Astrr00](https://github.com/Astrr00/totk-pc-recomp-android), [Mesa](https://mesa3d.org) Turnip and
[libadrenotools](https://github.com/bylaws/libadrenotools). Third-party code and its licenses:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

*Below: the upstream project's README, unchanged.*

# TotK Recomp

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native x86-64 application for Windows and Linux ([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.1.0, 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development. The game is playable on Windows and Linux; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.1.0, 1.2.1 and 1.4.3; 1.2.1 and 1.4.3 saves carry over between them
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- HDR10 output with peak, game and HUD brightness settings
- 16:10 (Steam Deck, handhelds), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- Anisotropic texture filtering up to 16x, for sharp ground into the distance
- In-game settings menu (F1) and performance overlay (F10)
- Setup and in-game menu in English, Russian, Brazilian Portuguese and Simplified Chinese
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Xbox button prompts: LB, RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button to press, with either button layout
- Shaders prepared at install instead of during play
- Mods made for emulators: unpack a mod into the `mods` folder next to `totk.exe`; switch and order them in the menu. RomFS
  mods, TKMM's merged output and ExeFS code patches (`.ips`, `.pchtxt`) work; the game rebuilds itself for code patches
  ([docs/tester-guide.md](docs/tester-guide.md), "Mods")
- Atmosphère cheats: put a cheat file into a mod and switch its cheats in the menu ([docs/cheats.md](docs/cheats.md))

## Planned

- VRR

## Installing

### Windows

Requirements: Windows 10 or later, an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build), a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after).

1. Download `TotK-Setup-<version>-windows.zip` from [Releases](../../releases/latest) and unpack it.
2. Start `TotK-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once.
4. Play `totk.exe` from the install folder.

### Linux

Requirements: as on Windows, plus Linux 6.7 or later and glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35, SteamOS 3).

1. Download `TotK-Setup-x86_64.AppImage` from [Releases](../../releases/latest), or build it ([docs/installer.md](docs/installer.md), "Linux release").
2. Make it executable (`chmod +x TotK-Setup-x86_64.AppImage`) and start it. On a Steam Deck, do this in Desktop Mode.
3. Pick your game files, the language and an install folder, and press *Start*.
4. Play `totk` from the install folder.

To update, run a newer setup on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

## Credits

TotK Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

TotK Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
