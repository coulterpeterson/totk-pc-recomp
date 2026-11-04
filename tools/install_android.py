"""Install the Android build on a device over adb and give it the game data.

Installs build/<version>-android/totk.apk, then fills the app's data folder
(/storage/emulated/0/Android/data/com.totkrecomp/files, android.c's TOTK_HOME) the way a portable PC folder is laid out
(runtime/main.c portable_paths): exefs/ from the extracted ExeFS, romfs/romfs.bin, and beside them romfs-index.bin,
the RomFS overlays of the build folder and the translator's identity (runtime/android.c). A file whose size on the
device already matches is skipped, so a second run only pushes what changed (the 17 GB romfs.bin once). The data is
your own game's: keep it on your devices.

--gpu-driver FILE puts a Vulkan driver library (a Mesa Turnip build for Adreno GPUs) into gpu_driver/ of the data
folder, which the app then loads instead of the system's driver (runtime/android.c); --gpu-driver system removes it.

--version-folder puts the game data into <data>/<version>/ (v143/ for 1.4.3) instead of the data folder itself, so two
game versions sit on the device side by side and each version's APK finds its own; saves, settings and caches stay
shared (a save a newer version wrote may not load in an older one).

--native installs the native-execution build (docs/native-execution.md): build/<version>-android-native/totk.apk and the
rewritten images of build/<version>-native in native/ beside the version's exefs/. An image is compared by its hash,
since a rewrite can change its bytes and keep its size.

  python tools/install_android.py [--version v0] [--serial SERIAL] [--no-apk] [--no-data] [--gpu-driver FILE|system]
                                  [--version-folder] [--native]
"""

import argparse
import hashlib
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from build_game import work_dir

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = 'com.totkrecomp'
HOME = f'/storage/emulated/0/Android/data/{PACKAGE}/files'


def adb(serial, *args, capture=False):
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(a) for a in args]
    if capture:
        return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    subprocess.run(cmd, check=True)


def remote_size(serial, path):
    out = adb(serial, 'shell', f'stat -c %s {shlex.quote(path)} 2>/dev/null || echo -1', capture=True).strip()
    return int(out) if out.lstrip('-').isdigit() else -1


made = set()


def make_dir(serial, path):
    """path and the folders between it and the data folder, open to the app: a folder adb's shell makes is the
    shell's, with group access only, and the app is not in that group (run-as cannot enter the data folder to make it
    as the app)."""
    if path in made:
        return
    parts = path[len(HOME) + 1:].split('/') if path.startswith(HOME + '/') else []
    dirs = [HOME + '/' + '/'.join(parts[:i + 1]) for i in range(len(parts))]
    adb(serial, 'shell', ' && '.join(f'mkdir -p {shlex.quote(d)} && chmod 2777 {shlex.quote(d)}' for d in dirs) or 'true')
    made.update(dirs)


PIECE = 256 << 20


def push(serial, src, dst):
    if remote_size(serial, dst) == src.stat().st_size:
        print(f'  up to date: {dst}')
        return
    make_dir(serial, dst.rsplit('/', 1)[0])
    print(f'  {src.name} ({src.stat().st_size / (1 << 20):,.1f} MiB) -> {dst}', flush=True)
    if src.stat().st_size > PIECE:
        push_resumable(serial, src, dst)
    else:
        adb(serial, 'push', src, dst)


def push_changed(serial, src, dst):
    """push for a file that can change and keep its size: compared by hash."""
    out = adb(serial, 'shell', f'sha256sum {shlex.quote(dst)} 2>/dev/null || true', capture=True).split()
    if out and out[0] == hashlib.sha256(src.read_bytes()).hexdigest():
        print(f'  up to date: {dst}')
        return
    make_dir(serial, dst.rsplit('/', 1)[0])
    print(f'  {src.name} ({src.stat().st_size / (1 << 20):,.1f} MiB) -> {dst}', flush=True)
    adb(serial, 'push', src, dst)


def push_resumable(serial, src, dst):
    """A large file in PIECE-sized appends, each a stream into `cat >>` on the device. adb push deletes what it wrote
    when the connection drops, which over wireless debugging lost gigabytes of romfs.bin at a time; this keeps what
    arrived and goes on from the last whole MiB after reconnecting."""
    size, failures = src.stat().st_size, 0
    q = shlex.quote(dst)
    while True:
        try:
            have = remote_size(serial, dst)
            if have == size:
                return
            start = max(have, 0) // (1 << 20) << 20 if 0 <= have < size else 0
            adb(serial, 'shell', f'truncate -s {start} {q}')
            n = min(PIECE, size - start)
            print(f'    {start / (1 << 20):,.0f} of {size / (1 << 20):,.0f} MiB', flush=True)
            cmd = ['adb'] + (['-s', serial] if serial else []) + ['exec-in', f'cat >> {q}']
            with open(src, 'rb') as f, subprocess.Popen(cmd, stdin=subprocess.PIPE) as p:
                f.seek(start)
                left = n
                while left:
                    block = f.read(min(left, 1 << 20))
                    p.stdin.write(block)
                    left -= len(block)
                p.stdin.close()
                if p.wait():
                    raise subprocess.CalledProcessError(p.returncode, cmd)
            if remote_size(serial, dst) <= start:
                raise RuntimeError('the piece did not arrive')
            failures = 0
        except (OSError, subprocess.CalledProcessError, RuntimeError) as e:
            failures += 1
            if failures > 20:
                sys.exit(f'{dst}: giving up after {failures} failures ({e})')
            print(f'    interrupted ({e}); retrying', flush=True)
            if serial and ':' in serial:   # a wireless debugging address: reconnect to it
                subprocess.run(['adb', 'connect', serial], capture_output=True)
            time.sleep(10)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='v0')
    ap.add_argument('--out', type=Path, help='build directory (default build/<version>-android)')
    ap.add_argument('--serial', help='adb device serial (default: the only device)')
    ap.add_argument('--no-apk', action='store_true', help='push the data only')
    ap.add_argument('--no-data', action='store_true', help='install the APK only')
    ap.add_argument('--gpu-driver', help='a Vulkan driver .so for gpu_driver/, or "system" to remove the one there')
    ap.add_argument('--version-folder', action='store_true',
                    help='push the game data into <data>/<version>/, beside another version\'s; the app built for that '
                         'version looks there first (runtime/main.c portable_paths)')
    ap.add_argument('--native', action='store_true',
                    help='the native-execution build: its APK and the rewritten images of build/<version>-native')
    args = ap.parse_args()
    if not shutil.which('adb'):
        sys.exit('adb not found: install the Android platform tools')
    out = (args.out or ROOT / 'build' / (f'{args.version}-android-native' if args.native else f'{args.version}-android')).resolve()
    extracted = work_dir() / 'extracted' / args.version
    if not args.no_apk:
        apk = out / 'totk.apk'
        if not apk.is_file():
            sys.exit(f'{apk} not found: run tools/build_apk.py')
        print(f'installing {apk}', flush=True)
        adb(args.serial, 'install', '-r', apk)
    if args.gpu_driver:
        adb(args.serial, 'shell', f'rm -rf {shlex.quote(HOME + "/gpu_driver")}')
        if args.gpu_driver != 'system':
            drv = Path(args.gpu_driver)
            if drv.suffix != '.so' or not drv.is_file():
                sys.exit(f'{drv}: not a .so file')
            push(args.serial, drv, f'{HOME}/gpu_driver/{drv.name}')
    data = f'{HOME}/{args.version}' if args.version_folder else HOME
    if args.native:
        images = ROOT / 'build' / f'{args.version}-native'
        files = sorted(images.glob('*.patched.bin')) + sorted(images.glob('*.tsv'))
        if not files:
            sys.exit(f'{images} holds no rewritten images: run tools/native/rewrite.py --version {args.version}')
        print(f'native images -> {data}/native', flush=True)
        for f in files:
            push_changed(args.serial, f, f'{data}/native/{f.name}')
    if args.no_data:
        return
    print(f'data -> {data}', flush=True)
    for name in ('main', 'main.npdm', 'sdk', 'subsdk0'):
        push(args.serial, extracted / 'exefs' / name, f'{data}/exefs/{name}')
    push(args.serial, extracted / 'romfs' / 'romfs.bin', f'{data}/romfs/romfs.bin')
    # the native build's folder has no index, seed or overlays of its own: they are the lifted build's
    lifted = ROOT / 'build' / f'{args.version}-android' if args.native else out
    push(args.serial, lifted / 'romfs-index.bin', f'{data}/romfs-index.bin')
    push(args.serial, ROOT / 'build' / 'xlate-android' / 'totk_xlate.id', f'{HOME}/totk_xlate.id')
    for seed in ('seed-answers.bin', 'seed-programs.bin'):   # tools/prepare_shaders.py --no-run: the shaders the game
        if (lifted / seed).is_file():                         # prepares in the background (TOTK_SHADER_PREPARE)
            push(args.serial, lifted / seed, f'{data}/{seed}')
    for overlay in sorted(lifted.glob('romfs-patches*')):   # beside romfs-index.bin, where romfs.c looks for them
        for f in sorted(p for p in overlay.rglob('*') if p.is_file()):
            push(args.serial, f, f'{data}/{f.relative_to(lifted).as_posix()}')
    print('done: start "TotK Recomp" on the device; logcat -s totk shows its log')


if __name__ == '__main__':
    main()
