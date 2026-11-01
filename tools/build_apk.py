"""Package build/<version>-android/libtotk.so as an APK: build/<version>-android/totk.apk.

The APK is SDL's Java activity (SDL 3.4, from tools/build_android_deps.py's source tree), tools/android/'s manifest and
MainActivity, libSDL3.so and libtotk.so. No Gradle: javac, the SDK's d8, aapt2, zipalign and apksigner do the steps.
The libraries are stored uncompressed and page-aligned so Android maps them from the APK (extractNativeLibs="false").
It is signed with the local debug key (~/.android/debug.keystore, made with keytool when missing).

The APK holds code compiled from your game: install it on your own devices only, never share it. The game data stays
out of it; tools/install_android.py pushes that.

  python tools/build_apk.py [--version v0] [--out DIR] [--keep-debug]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANDROID = ROOT / 'tools' / 'android'
DEPS = ROOT / 'toolchain' / 'android'
COMPILE_SDK = 35
MIN_SDK = 33   # tools/build_game.py ANDROID_API


def sdk_dir():
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or
               Path.home() / ('Library/Android/sdk' if sys.platform == 'darwin' else 'Android/Sdk'))
    if not (sdk / 'platforms').is_dir():
        sys.exit(f'Android SDK not found at {sdk}: set ANDROID_HOME')
    return sdk


def newest(paths):
    paths = sorted(paths, key=lambda p: [int(x) for x in p.name.replace('-', '.').split('.') if x.isdigit()])
    return paths[-1] if paths else None


def run(cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def debug_keystore():
    ks = Path.home() / '.android' / 'debug.keystore'
    if not ks.is_file():
        ks.parent.mkdir(parents=True, exist_ok=True)
        run(['keytool', '-genkeypair', '-keystore', ks, '-storepass', 'android', '-alias', 'androiddebugkey',
             '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
             '-dname', 'CN=Android Debug,O=Android,C=US'])
    return ks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default='v0', help='game version (default v0)')
    ap.add_argument('--out', type=Path, help='build directory (default build/<version>-android)')
    ap.add_argument('--keep-debug', action='store_true', help='keep libtotk.so\'s debug sections (larger APK)')
    args = ap.parse_args()
    out = (args.out or ROOT / 'build' / f'{args.version}-android').resolve()
    lib = out / 'libtotk.so'
    if not lib.is_file():
        sys.exit(f'{lib} not found: run tools/build_game.py --target android first')
    sdl_java = DEPS / 'src' / 'SDL' / 'android-project' / 'app' / 'src' / 'main' / 'java'
    sdl_so = DEPS / 'arm64-v8a' / 'lib' / 'libSDL3.so'
    if not sdl_java.is_dir() or not sdl_so.is_file():
        sys.exit('SDL for Android not found: run tools/build_android_deps.py')

    sdk = sdk_dir()
    jar = sdk / 'platforms' / f'android-{COMPILE_SDK}' / 'android.jar'
    if not jar.is_file():
        sys.exit(f'{jar} not found: install the Android {COMPILE_SDK} platform with sdkmanager')
    tools = newest((sdk / 'build-tools').glob('*'))
    from build_android_deps import default_ndk
    strip = newest((default_ndk() / 'toolchains' / 'llvm' / 'prebuilt').glob('*')) / 'bin' / 'llvm-strip'

    with tempfile.TemporaryDirectory(prefix='apk-') as tmp:
        tmp = Path(tmp)
        sources = sorted(sdl_java.rglob('*.java')) + sorted((ANDROID / 'java').rglob('*.java'))
        run(['javac', '-nowarn', '-Xlint:none', '--release', '11', '-classpath', jar, '-d', tmp / 'classes', *sources])
        run([tools / 'd8', '--release', '--min-api', MIN_SDK, '--lib', jar, '--output', tmp,
             *sorted((tmp / 'classes').rglob('*.class'))])
        run([tools / 'aapt2', 'link', '-o', tmp / 'base.apk', '--manifest', ANDROID / 'AndroidManifest.xml', '-I', jar,
             '--min-sdk-version', MIN_SDK, '--target-sdk-version', COMPILE_SDK])
        so = tmp / 'libtotk.so'
        if args.keep_debug:
            shutil.copy2(lib, so)
        else:
            run([strip, '--strip-debug', '-o', so, lib])
        unaligned = tmp / 'unaligned.apk'
        with zipfile.ZipFile(tmp / 'base.apk') as base, zipfile.ZipFile(unaligned, 'w', allowZip64=True) as z:
            for item in base.infolist():
                z.writestr(item, base.read(item.filename))
            z.write(tmp / 'classes.dex', 'classes.dex', zipfile.ZIP_DEFLATED)
            for path in (sdl_so, so):
                z.write(path, f'lib/arm64-v8a/{path.name}', zipfile.ZIP_STORED)
        aligned = tmp / 'aligned.apk'
        # -P 16: libraries on 16 KiB boundaries, which also covers 4 KiB-page kernels
        run([tools / 'zipalign', '-f', '-P', '16', '4', unaligned, aligned])
        apk = out / 'totk.apk'
        run([tools / 'apksigner', 'sign', '--ks', debug_keystore(), '--ks-pass', 'pass:android',
             '--key-pass', 'pass:android', '--out', apk, aligned])
    print(f'{apk}: {apk.stat().st_size / (1 << 20):,.1f} MiB')


if __name__ == '__main__':
    main()
