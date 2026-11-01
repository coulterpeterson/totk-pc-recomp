"""Install the Android build on a device over adb and give it the game data.

Installs build/<version>-android/totk.apk, then fills the app's data folder
(/sdcard/Android/data/com.totkrecomp/files, android.c's TOTK_HOME) the way a portable PC folder is laid out
(runtime/main.c portable_paths): exefs/ from the extracted ExeFS, romfs/romfs.bin, and beside them romfs-index.bin,
the RomFS overlays of the build folder and the translator's identity (runtime/android.c). A file whose size on the
device already matches is skipped, so a second run only pushes what changed (the 17 GB romfs.bin once). The data is
your own game's: keep it on your devices.

  python tools/install_android.py [--version v0] [--serial SERIAL] [--no-apk] [--no-data]
"""

import argparse
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from build_game import work_dir

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = 'com.totkrecomp'
HOME = f'/sdcard/Android/data/{PACKAGE}/files'


def adb(serial, *args, capture=False):
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(a) for a in args]
    if capture:
        return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    subprocess.run(cmd, check=True)


def remote_size(serial, path):
    out = adb(serial, 'shell', f'stat -c %s {shlex.quote(path)} 2>/dev/null || echo -1', capture=True).strip()
    return int(out) if out.lstrip('-').isdigit() else -1


def push(serial, src, dst):
    if remote_size(serial, dst) == src.stat().st_size:
        print(f'  up to date: {dst}')
        return
    adb(serial, 'shell', 'mkdir', '-p', shlex.quote(dst.rsplit('/', 1)[0]))
    print(f'  {src.name} ({src.stat().st_size / (1 << 20):,.1f} MiB) -> {dst}', flush=True)
    adb(serial, 'push', src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='v0')
    ap.add_argument('--out', type=Path, help='build directory (default build/<version>-android)')
    ap.add_argument('--serial', help='adb device serial (default: the only device)')
    ap.add_argument('--no-apk', action='store_true', help='push the data only')
    ap.add_argument('--no-data', action='store_true', help='install the APK only')
    args = ap.parse_args()
    if not shutil.which('adb'):
        sys.exit('adb not found: install the Android platform tools')
    out = (args.out or ROOT / 'build' / f'{args.version}-android').resolve()
    extracted = work_dir() / 'extracted' / args.version
    if not args.no_apk:
        apk = out / 'totk.apk'
        if not apk.is_file():
            sys.exit(f'{apk} not found: run tools/build_apk.py')
        print(f'installing {apk}', flush=True)
        adb(args.serial, 'install', '-r', apk)
    if args.no_data:
        return
    print(f'data -> {HOME}', flush=True)
    for name in ('main', 'main.npdm', 'sdk', 'subsdk0'):
        push(args.serial, extracted / 'exefs' / name, f'{HOME}/exefs/{name}')
    push(args.serial, extracted / 'romfs' / 'romfs.bin', f'{HOME}/romfs/romfs.bin')
    push(args.serial, out / 'romfs-index.bin', f'{HOME}/romfs-index.bin')
    push(args.serial, ROOT / 'build' / 'xlate-android' / 'totk_xlate.id', f'{HOME}/totk_xlate.id')
    for overlay in sorted(out.glob('romfs-patches*')):
        for f in sorted(p for p in overlay.rglob('*') if p.is_file()):
            push(args.serial, f, f'{HOME}/{f.relative_to(out).as_posix()}')
    print('done: start "TotK Recomp" on the device; logcat -s totk shows its log')


if __name__ == '__main__':
    main()
