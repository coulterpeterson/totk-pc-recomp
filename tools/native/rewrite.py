"""Rewrite a game version's modules for native execution on an AArch64 Android host (docs/native-execution.md).

Runs tools/native/patch_nso.py on main, subsdk0 and sdk, which writes each module's patched image (image, bss, key page
and patch area) and its site tables into the output folder, then lays the modules out with room for their patch areas
(recomp/layout.py, sizes from the patched images) and writes layout.json and layout.h there. The version's patch file
(recomp/patches-<version>.txt) gives the spin waits, which the rewrite sends through the SVC gate (rt_spin_gate), and
the patch sets' sites, written to patchsites.tsv for the loader, which sets their words while the sets are on
(runtime/native.c). --verify runs the structure checker (structck.py) and the per-site equivalence checker against
Unicorn (equiv.py) on every module.

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
import patches  # noqa: E402

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
    table = patches.load(ROOT / 'recomp' / f'patches-{a.version}.txt', layout(exefs, 'android'))
    spins = out / 'spins.tsv'
    spins.write_text(''.join(f'{m} {b:x} {h:x}\n' for m, edges in table.spins.items() for b, h in sorted(edges.items())))
    # The loader sets one word per site; a set with a longer replacement anywhere (an inlined veneer: the hair and
    # cloth clock) stays out whole, so no set half-applies. Its switch then does nothing in the native build.
    lifted_only = {k for sites in table.sites.values() for alts in sites.values() for k, repl, _ in alts if len(repl) != 1}
    for k in sorted(lifted_only):
        print(f'set {table.sets[k][0]}: sites of several words, lifted code only')
    rows = []
    for m, sites in table.sites.items():
        for off, alts in sorted(sites.items()):
            for k, repl, orig in alts:
                if k in lifted_only:
                    continue
                w = repl[0]
                rows.append(f'{k}\t{m}\t{off:x}\t{orig:08x}\t' + (f'page:x{w[1]}' if isinstance(w, tuple) else f'{w:08x}'))
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0', DM_SPINS=str(spins))
    py = sys.executable

    for m in MODULES:
        line = [l for l in run([py, HERE / 'patch_nso.py', exefs / m, out, m], env).splitlines() if l.startswith(m + ' ')]
        print(line[-1] if line else f'{m}: rewritten')
    (out / 'build_src.sha256').write_text(
        ''.join(f'{hashlib.sha256((HERE / f).read_bytes()).hexdigest()}  {f}\n'
                for f in ('patch_nso.py', 'call_window.py', 'relr.py')))

    for m in MODULES:   # a patch site must still hold its original word: the rewrite may not have moved it
        rewritten = {int(l.split()[0], 16) for l in (out / f'{m}.sites.tsv').read_text().splitlines() if l.split()[1] != 'J'}
        clash = sorted(off for off, alts in table.sites.get(m, {}).items()
                       if off in rewritten and any(k not in lifted_only for k, _, _ in alts))
        if clash:
            raise SystemExit(f'{m}: patch sites {[hex(x) for x in clash]} are rewrite sites')
    (out / 'patchsites.tsv').write_text(
        f'# set\tmodule\toffset\toriginal\treplacement (page:x<n> = adrp x<n>, the patch page)\n' +
        ''.join(r + '\n' for r in rows))
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
