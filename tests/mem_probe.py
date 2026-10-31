"""Build and run runtime/mem_probe.c on its own: the guest address space (runtime/memory.c) and write tracking
(runtime/platform/linux.c), with the module layout from recomp/layout.py and no game files.

Usage: mem_probe.py [--target pc|android] [--aarch64]   (TOTK_WW=mprotect forces page-protection tracking)
--target android compiles with __ANDROID__ (the Android address map, no re-exec) but links glibc.
--aarch64 cross-builds with clang (the aarch64-linux-gnu libc and gcc-cross files installed) and runs under
qemu-aarch64 with -L /usr/aarch64-linux-gnu (or QEMU_LD_PREFIX).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
import layout  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', choices=sorted(layout.FIRST_BASES), default='pc')
    ap.add_argument('--aarch64', action='store_true')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / f'mem_{args.target}' / ('aarch64' if args.aarch64 else 'host')
    layout.write([('main', layout.FIRST_BASES[args.target], 0x1000000)], work / 'inc')
    rt = ROOT / 'runtime'
    cmd = ['clang', '-O1', '-g', '-w', '-I', str(work / 'inc'), '-I', str(rt), '-I', str(ROOT / 'tests' / 'mem_probe_shim'),
           *(['-D__ANDROID__'] if args.target == 'android' else []),
           *(['--target=aarch64-linux-gnu', '-fuse-ld=lld'] if args.aarch64 else []),
           *(os.environ.get('TOTK_PROBE_CFLAGS', '').split()),
           str(rt / 'memory.c'), str(rt / 'platform' / 'linux.c'), str(rt / 'platform' / 'common.c'),
           str(rt / 'mem_probe.c'), str(ROOT / 'tests' / 'mem_probe_main.c'), '-ldl', '-lpthread',
           '-o', str(work / 'mem_probe')]
    subprocess.check_call(cmd)
    run = (['qemu-aarch64', '-L', os.environ.get('QEMU_LD_PREFIX', '/usr/aarch64-linux-gnu')] if args.aarch64 else []) + [str(work / 'mem_probe')]
    return subprocess.run(run).returncode


if __name__ == '__main__':
    sys.exit(main())
