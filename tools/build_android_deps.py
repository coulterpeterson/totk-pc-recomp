"""Build the runtime's libraries for Android arm64 with the NDK: toolchain/android/arm64-v8a/{include,lib}.

SDL3 (shared, libSDL3.so goes into the APK), Opus, libvpx and zstd (static, linked into libtotk.so), plus the
Vulkan-Headers and the Vulkan Memory Allocator header. Each source is fetched at a pinned tag into
toolchain/android/src/<name> and built out of tree. The versions are the ones the Windows build ships
(THIRD_PARTY_NOTICES.md). Run with .venv; needs git, cmake, ninja (the Android SDK's cmake directory has one) and
make.

  python tools/build_android_deps.py [--ndk DIR] [--api N] [--only NAME,...]
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ABI = 'arm64-v8a'
TRIPLE = 'aarch64-linux-android'
DEFAULT_API = 29   # Android 10, the oldest the SDLActivity of SDL 3.4 supports with Vulkan

# name: (git URL, tag)
SOURCES = {
    'SDL': ('https://github.com/libsdl-org/SDL.git', 'release-3.4.18'),
    'opus': ('https://github.com/xiph/opus.git', 'v1.5.2'),
    'libvpx': ('https://github.com/webmproject/libvpx.git', 'v1.17.0'),
    'zstd': ('https://github.com/facebook/zstd.git', 'v1.5.7'),
    'Vulkan-Headers': ('https://github.com/KhronosGroup/Vulkan-Headers.git', 'v1.4.363'),
}
# The header tools/build_game.py pins for the Windows release (VMA_FETCH there)
VMA = ('https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h',
       '8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a')


def default_ndk():
    for var in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'ANDROID_NDK'):
        if os.environ.get(var):
            return Path(os.environ[var])
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or
               Path.home() / ('Library/Android/sdk' if sys.platform == 'darwin' else 'Android/Sdk'))
    ndks = sorted((sdk / 'ndk').glob('*'), key=lambda p: [int(x) for x in p.name.split('.') if x.isdigit()])
    return ndks[-1] if ndks else None


def find_ninja():
    found = shutil.which('ninja')
    if found:
        return found
    sdk = Path(os.environ.get('ANDROID_HOME') or Path.home() / 'Library/Android/sdk')
    for p in sorted((sdk / 'cmake').glob('*/bin/ninja')):
        return str(p)
    sys.exit('ninja not found: install it or the Android SDK cmake package')


def run(cmd, **kw):
    print('  $', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def fetch(name, src):
    url, tag = SOURCES[name]
    if (src / '.git').is_dir():
        have = subprocess.run(['git', '-C', str(src), 'describe', '--tags', '--exact-match'], capture_output=True,
                              text=True).stdout.strip()
        if have == tag:
            return
        shutil.rmtree(src)
    src.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'clone', '-q', '--depth', '1', '--branch', tag, url, src])


def cmake_build(src, build, prefix, ndk, api, ninja, extra):
    run(['cmake', '-S', src, '-B', build, '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}',
         f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', f'-DANDROID_ABI={ABI}',
         f'-DANDROID_PLATFORM=android-{api}', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={prefix}',
         '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', *extra])
    run(['cmake', '--build', build])
    run(['cmake', '--install', build])


def build_libvpx(src, build, prefix, ndk, api):
    """libvpx has its own configure script; the NDK's clang stands in for the arm64-android-gcc target."""
    bin_ = next((ndk / 'toolchains' / 'llvm' / 'prebuilt').glob('*')) / 'bin'
    env = dict(os.environ, CC=f'{bin_}/{TRIPLE}{api}-clang', CXX=f'{bin_}/{TRIPLE}{api}-clang++',
               AR=f'{bin_}/llvm-ar', AS=f'{bin_}/{TRIPLE}{api}-clang', LD=f'{bin_}/{TRIPLE}{api}-clang',
               STRIP=f'{bin_}/llvm-strip', NM=f'{bin_}/llvm-nm', RANLIB=f'{bin_}/llvm-ranlib')
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)
    run([src / 'configure', '--target=arm64-android-gcc', f'--prefix={prefix}', '--enable-pic', '--disable-examples',
         '--disable-tools', '--disable-docs', '--disable-unit-tests', '--disable-vp8', '--disable-vp9-encoder',
         '--enable-vp9-decoder', '--disable-webm-io', '--disable-libyuv'], cwd=build, env=env)
    run(['make', f'-j{os.cpu_count() or 4}'], cwd=build, env=env)
    run(['make', 'install'], cwd=build, env=env)


def fetch_vma(prefix):
    dst = prefix / 'include' / 'vk_mem_alloc.h'
    if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == VMA[1]:
        return
    data = urllib.request.urlopen(VMA[0], timeout=60).read()
    if hashlib.sha256(data).hexdigest() != VMA[1]:
        sys.exit('vk_mem_alloc.h: hash mismatch')
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk', type=Path, default=default_ndk())
    ap.add_argument('--api', type=int, default=DEFAULT_API)
    ap.add_argument('--only', help='comma-separated subset of ' + ','.join(SOURCES))
    args = ap.parse_args()
    if not args.ndk or not (args.ndk / 'build' / 'cmake' / 'android.toolchain.cmake').is_file():
        sys.exit('NDK not found: pass --ndk or set ANDROID_NDK_HOME')
    if not shutil.which('cmake'):
        sys.exit('cmake not found')
    ninja = find_ninja()
    base = ROOT / 'toolchain' / 'android'
    prefix = base / ABI
    wanted = args.only.split(',') if args.only else list(SOURCES)
    for name in wanted:
        print(f'{name} {SOURCES[name][1]}', flush=True)
        src, build = base / 'src' / name, base / 'build' / name
        fetch(name, src)
        if name == 'SDL':
            cmake_build(src, build, prefix, args.ndk, args.api, ninja,
                        ['-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF',
                         '-DSDL_EXAMPLES=OFF'])
        elif name == 'opus':
            cmake_build(src, build, prefix, args.ndk, args.api, ninja,
                        ['-DOPUS_BUILD_SHARED_LIBRARY=OFF', '-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF'])
        elif name == 'zstd':
            cmake_build(src / 'build' / 'cmake', build, prefix, args.ndk, args.api, ninja,
                        ['-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_STATIC=ON', '-DZSTD_BUILD_PROGRAMS=OFF',
                         '-DZSTD_BUILD_TESTS=OFF'])
        elif name == 'libvpx':
            build_libvpx(src, build, prefix, args.ndk, args.api)
        elif name == 'Vulkan-Headers':
            cmake_build(src, build, prefix, args.ndk, args.api, ninja, ['-DVULKAN_HEADERS_ENABLE_MODULE=OFF'])
    fetch_vma(prefix)
    print(f'done: {prefix}')


if __name__ == '__main__':
    main()
