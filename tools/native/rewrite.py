"""Rewrite a game version's modules for native execution on an AArch64 Android host (docs/native-execution.md).

Runs tools/native/patch_nso.py on main, subsdk0 and sdk, which writes each module's patched image (image, bss, key page
and patch area) and its site tables into the output folder, then lays the modules out with room for their patch areas
(recomp/layout.py, sizes from the patched images) and writes layout.json and layout.h there. --verify runs the
structure checker (structck.py) and the per-site equivalence checker against Unicorn (equiv.py) on every module.

Usage: .venv/bin/python tools/native/rewrite.py --version v0 [--verify] [--jobs N]
Output: build/<version>-native/ (game-derived, ignored by Git).
"""
import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'recomp'))
from layout import FIRST_BASES, layout, write  # noqa: E402

MODULES = ('main', 'subsdk0', 'sdk')
KERNEL_OFFSET = 0x10000000   # runtime/host.h RT_KERNEL_BASE: the guest kernel region starts this far above the modules


def llvm_bin():
    """The NDK's LLVM tools (structck.py disassembles with llvm-objdump): LLVM_BIN, else the newest NDK."""
    if os.environ.get('LLVM_BIN'):
        return os.environ['LLVM_BIN']
    roots = [os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')]
    sdk = os.environ.get('ANDROID_HOME') or os.path.expanduser('~/Library/Android/sdk')
    ndks = sorted((Path(sdk) / 'ndk').glob('*')) if (Path(sdk) / 'ndk').is_dir() else []
    for root in [r for r in roots if r] + [str(p) for p in reversed(ndks)]:
        found = sorted(Path(root).glob('toolchains/llvm/prebuilt/*/bin/llvm-objdump'))
        if found:
            return str(found[0].parent)
    raise SystemExit('no llvm-objdump: set LLVM_BIN or install the Android NDK')


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f'failed: {" ".join(map(str, cmd))}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v0')
    ap.add_argument('--exefs', help='default build/extracted/<version>/exefs')
    ap.add_argument('--out', help='default build/<version>-native')
    ap.add_argument('--verify', action='store_true', help='run structck.py and equiv.py on every module')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    exefs = Path(a.exefs or ROOT / 'build' / 'extracted' / a.version / 'exefs')
    out = Path(a.out or ROOT / 'build' / f'{a.version}-native')
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    py = sys.executable

    for m in MODULES:
        line = [l for l in run([py, HERE / 'patch_nso.py', exefs / m, out, m], env).splitlines() if l.startswith(m + ' ')]
        print(line[-1] if line else f'{m}: rewritten')
    (out / 'build_src.sha256').write_text(
        ''.join(f'{hashlib.sha256((HERE / f).read_bytes()).hexdigest()}  {f}\n'
                for f in ('patch_nso.py', 'call_window.py', 'relr.py')))

    sizes = {m: (out / f'{m}.patched.bin').stat().st_size for m in MODULES}
    mods = layout(exefs, 'android', sizes)
    end = max(b + s for _, b, s in mods)
    if end > FIRST_BASES['android'] + KERNEL_OFFSET:
        raise SystemExit(f'modules end at {end:#x}, inside the kernel region (runtime/host.h RT_KERNEL_BASE)')
    write(mods, out)
    for n, b, s in mods:
        print(f'layout: {n} at {b:#x}, {s / 2**20:.1f} MiB')

    if a.verify:
        env = dict(env, LLVM_BIN=llvm_bin(), DM_VCACHE=str(out / 'vcache'))
        for m in MODULES:
            print(run([py, HERE / 'structck.py', out, m, exefs / m], env).strip().splitlines()[-1])
        for m in MODULES:
            print(run([py, HERE / 'equiv.py', out, m, exefs / m, '--jobs', str(a.jobs)], env).strip().splitlines()[-1])


if __name__ == '__main__':
    main()
