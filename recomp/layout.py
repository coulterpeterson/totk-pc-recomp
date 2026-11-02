"""Guest address layout of the loaded modules, computed from their sizes.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the
generator and the runtime must agree on every module base. This is the single place that decides them: the generator
calls layout() and writes the result into generated/<version>/layout.h and layout.json, which the runtime and the
build read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules):
main, subsdk0..9, sdk. Each base is aligned to 2 MiB. Everything must stay below 2^39, the limit of the addresses
IPC buffers can encode.
"""
import json
from pathlib import Path

from nso import Nso

# PC: the top 64 GiB of a 39-bit space is free of system mappings. Android: ART, libraries, thread stacks and drivers
# are placed top-down from the top of the (commonly 39-bit) user space, so the modules sit at 128 GiB instead, in
# range nothing is placed in. runtime/host.h repeats the base for the kernel region (+0x10000000) and the host
# function window (+0x20000000); runtime/memory.c asserts that both agree.
FIRST_BASES = {'pc': 0x7000000000, 'android': 0x2000000000}
FIRST_BASE = FIRST_BASES['pc']
ALIGN = 0x200000
ADDRESS_LIMIT = 1 << 39
ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


def align_up(v, a=ALIGN):
    return (v + a - 1) & ~(a - 1)


def layout(exefs, target='pc', sizes=None):
    """[(name, base, size)] for the modules present in the ExeFS directory, in load order. sizes: module name -> size
    in place of the NSO's image and bss, for images that carry more behind them (the native rewrite's key page and
    patch area, tools/native/rewrite.py)."""
    exefs = Path(exefs)
    out, base = [], FIRST_BASES[target]
    for name in ORDER:
        path = exefs / name
        if not path.exists():
            continue
        size = Nso(str(path)).size
        if sizes and name in sizes:
            if sizes[name] < size:
                raise ValueError(f'{name}: size {sizes[name]:#x} below its image and bss ({size:#x})')
            size = sizes[name]
        out.append((name, base, size))
        base = align_up(base + size)
    if base > ADDRESS_LIMIT:
        raise ValueError(f'modules end at {base:#x}, above 2^39')
    return out


def write(mods, outdir):
    """layout.json for Python tools and layout.h for the runtime."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'layout.json').write_text(json.dumps(
        [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'} for n, b, s in mods], indent=1) + '\n')
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    (outdir / 'layout.h').write_text('\n'.join(lines))


def read(outdir):
    """[(name, base, size)] from a layout.json written by write()."""
    data = json.loads((Path(outdir) / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]
