"""Patch sites of several words for native execution (tools/native/rewrite.py, docs/native-execution.md).

The loader switches one word per patch site (runtime/native.c native_patches_sync). A replacement of several words
(recomp/patches.py: an inlined veneer, such as the real-time step's or the hair and cloth clock's) becomes a block of
code appended to the module's rewritten image, behind its patch area, and the site's one switchable word becomes a
branch to that block. The block holds the replacement's words; the PC-relative ones are encoded for the block: an
`@page:x<n>` becomes an adrp to the patch page from the block's address, and a last bl calls the target the bl had at
the site. The block ends with a branch back to the instruction after the site. A bl's return then lands in the block
(the branch back) rather than at the site's next word: the callee sees another x30, the same return path.

check() runs every block in Unicorn against a reference that loads the page address with movz/movk and stops at the
bl, whose target capstone reads from the original word at the site, and has capstone confirm every block's branch back
(which a run stopping at the bl does not reach).
"""
import random
import struct

PAGE_WORD = 'page'   # recomp/patches.py: ('page', reg) for @page:x<reg>


def B(frm, to, link=False):
    d = to - frm
    assert d % 4 == 0 and -(1 << 27) <= d < (1 << 27), f'branch {frm:#x} -> {to:#x} out of range'
    return (0x94000000 if link else 0x14000000) | ((d >> 2) & 0x3FFFFFF)


def ADRP(rd, pc, target):
    d = (target >> 12) - (pc >> 12)
    assert -(1 << 20) <= d < (1 << 20), f'adrp {pc:#x} -> {target:#x} out of range'
    imm = d & 0x1FFFFF
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


def is_bl(w): return w & 0xFC000000 == 0x94000000


def branch_target(w, pc):
    imm = w & 0x3FFFFFF
    return pc + ((imm - (1 << 26) if imm & (1 << 25) else imm) << 2)


def pc_relative(w):
    """A word whose meaning depends on its address, other than b and bl: adr/adrp, ldr (literal), b.cond, cbz/cbnz,
    tbz/tbnz. Patch replacements have none of these besides @page; a new one needs encoding here first."""
    return (w & 0x1F000000 == 0x10000000 or w & 0x3B000000 == 0x18000000 or w & 0xFF000010 == 0x54000000 or
            w & 0x7E000000 == 0x34000000 or w & 0x7E000000 == 0x36000000)


def build(sites, base, size, page):
    """sites: [(set, offset, original, replacement words)] of one module whose image starts at base and is size bytes.
    Returns (code to append at offset size, {(set, offset): the site's branch word}, [block descriptions for check])."""
    code, branches, blocks = bytearray(), {}, []
    for k, off, orig, repl in sites:
        site, start = base + off, base + size + len(code)
        words, refs = [], []   # refs: (index, kind, what the reference does) for check
        for j, w in enumerate(repl):
            pc = start + 4 * len(words)
            if isinstance(w, tuple) and w[0] == PAGE_WORD:
                words.append(ADRP(w[1], pc, page))
                refs.append(('page', w[1]))
            elif w & 0x7C000000 == 0x14000000:   # b or bl
                assert j == len(repl) - 1, f'{off:#x}: a branch before the last word'
                target = branch_target(w, site)   # the replacement is lifted at the site's address
                assert is_bl(w), f'{off:#x}: a last b needs no branch back; not seen yet, so not handled'
                words.append(B(pc, target, link=True))
                refs.append(('bl', w))
            else:
                assert not pc_relative(w), f'{off:#x}: replacement word {w:08x} depends on its address'
                words.append(w)
                refs.append(('word', w))
        words.append(B(start + 4 * len(words), site + 4))
        code += b''.join(struct.pack('<I', w) for w in words)
        branches[(k, off)] = B(site, start)
        blocks.append((k, off, orig, site, start, words, refs))
    return code, branches, blocks


# ---- check: Unicorn runs the block from the site and a reference of the same words, from the same random state


def check(blocks, page, trials=24, seed=1):
    from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
    from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_X30, UC_ARM64_REG_SP, \
        UC_ARM64_REG_NZCV, UC_ARM64_REG_Q0, UC_ARM64_REG_FPCR, UC_ARM64_REG_CPACR_EL1
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    rng = random.Random(seed)
    STACK, SCRATCH = 0x7F0000000000, 0x7E0000000000
    GPR = [UC_ARM64_REG_X0 + i for i in range(29)]   # x0..x28 are consecutive in Unicorn's numbering, x29 and x30 not
    X29, X30 = UC_ARM64_REG_X29, UC_ARM64_REG_X30
    QS = [UC_ARM64_REG_Q0 + i for i in range(32)]
    bad = 0

    def page_of(a): return a & ~0xFFF

    def run(words_at, entry, stops, state):
        """Runs from entry until a stop address; memory the words reach through a register (a game object) is mapped
        when first touched, with contents fixed by the trial, so both runs see the same bytes."""
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)   # FP and SIMD on
        mapped = set()
        for a in [page, STACK] + [a for a, _ in words_at] + list(stops):
            p = page_of(a)
            if p not in mapped:
                uc.mem_map(p, 0x1000)
                mapped.add(p)
        uc.mem_write(page, state['page'])
        uc.mem_write(STACK, state['stack'])
        for a, w in words_at:
            uc.mem_write(a, struct.pack('<I', w))
        for r, v in zip(GPR, state['x']):
            uc.reg_write(r, v)
        uc.reg_write(X29, STACK + 0x800)
        uc.reg_write(X30, 0)
        uc.reg_write(UC_ARM64_REG_SP, STACK + 0x800)
        for r, v in zip(QS, state['q']):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM64_REG_NZCV, state['nzcv'])
        uc.reg_write(UC_ARM64_REG_FPCR, 0)
        stop, lazy = {}, []

        def hook(u, addr, size, _):
            if addr in stops:
                stop['pc'] = addr
                u.emu_stop()

        def unmapped(u, access, addr, size, value, _):
            p = page_of(addr)
            if p in mapped or len(lazy) > 8:
                return False
            u.mem_map(p, 0x1000)
            u.mem_write(p, random.Random(state['seed'] ^ p).randbytes(0x1000))
            mapped.add(p)
            lazy.append(p)
            return True
        uc.hook_add(UC_HOOK_CODE, hook)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
        try:
            uc.emu_start(entry, 0xFFFFFFFFFFFFFFFF, count=200)
            fault = None
        except UcError as e:
            fault = str(e)
        return {'pc': stop.get('pc'), 'fault': fault,
                'lazy': {p: bytes(uc.mem_read(p, 0x1000)) for p in lazy}, 'x': [uc.reg_read(r) for r in GPR], 'x29': uc.reg_read(X29),
                'sp': uc.reg_read(UC_ARM64_REG_SP), 'q': [uc.reg_read(r) for r in QS],
                'nzcv': uc.reg_read(UC_ARM64_REG_NZCV), 'page': bytes(uc.mem_read(page, 0x1000)),
                'stack': bytes(uc.mem_read(STACK, 0x1000)), 'x30': uc.reg_read(X30)}

    for k, off, orig, site, start, words, refs in blocks:
        bl = refs[-1][0] == 'bl'
        back = next(md.disasm(struct.pack('<I', words[-1]), start + 4 * (len(words) - 1)))
        if back.mnemonic != 'b' or int(back.op_str.lstrip('#'), 16) != site + 4:   # also after a bl's return
            bad += 1
            print(f'veneer {off:#x}: the block ends in {back.mnemonic} {back.op_str}, not b {site + 4:#x}')
        bl_target = None
        if bl:
            i = next(md.disasm(struct.pack('<I', refs[-1][1]), site))   # capstone's reading of the word at the site
            bl_target = int(i.op_str.lstrip('#'), 16)
        # the reference: the words at a scratch address in order, @page as movz/movk of the page address, stopping at
        # the bl (whose target capstone gave) or after the last word
        ref_words = []
        for kind, v in refs:
            if kind == 'page':
                ref_words += [0xD2800000 | (((page >> s) & 0xFFFF) << 5) | ((s // 16) << 21) | v if s == 0 else
                              0xF2800000 | (((page >> s) & 0xFFFF) << 5) | ((s // 16) << 21) | v for s in (0, 16, 32, 48)]
            elif kind == 'word':
                ref_words.append(v)
        ref_end = SCRATCH + 4 * len(ref_words)
        for t in range(trials):
            # registers: addresses a page can be mapped at (40 bits) or small values; the patch page's first words are
            # the frame step's ratio and seconds (runtime/frame_rate.c step_store)
            state = {'seed': rng.getrandbits(64),
                     'x': [(rng.getrandbits(40) & ~7) if rng.random() < 0.7 else rng.getrandbits(16) for _ in GPR],
                     'q': [rng.getrandbits(128) if rng.random() < 0.5 else
                           int.from_bytes(struct.pack('<4f', *[rng.uniform(-4, 4) for _ in range(4)]), 'little')
                           for _ in QS],
                     'nzcv': rng.getrandbits(4) << 28,
                     'page': struct.pack('<4f', rng.uniform(0.2, 2), rng.uniform(0.005, 0.07), 0, 0) +
                             bytes(rng.getrandbits(8) for _ in range(0x1000 - 16)),
                     'stack': bytes(rng.getrandbits(8) for _ in range(0x1000))}
            ref = run([(SCRATCH + 4 * i, w) for i, w in enumerate(ref_words)] + [(ref_end, 0xD503201F)], SCRATCH,
                      {ref_end}, state)
            nat = run([(site, B(site, start))] + [(start + 4 * i, w) for i, w in enumerate(words)] +
                      [(site + 4, 0xD503201F)] + ([(bl_target, 0xD503201F)] if bl else []), site,
                      {bl_target} if bl else {site + 4}, state)
            want_pc = bl_target if bl else site + 4
            if ref['fault'] and nat['fault']:
                continue   # both stopped at the same kind of fault: nothing to compare
            diffs = [n for n in ('x', 'x29', 'sp', 'q', 'nzcv', 'page', 'stack', 'lazy', 'fault') if ref[n] != nat[n]]
            if nat['pc'] != want_pc or ref['pc'] != ref_end or diffs:
                bad += 1
                if bad <= 10:
                    print(f'veneer {off:#x} (set {k}) trial {t}: stopped at {nat["pc"]} (want {want_pc:#x}), '
                          f'differs in {diffs}')
            if bl and nat['x30'] != start + 4 * (len(words) - 1):
                bad += 1
                print(f'veneer {off:#x}: the bl returns to {nat["x30"]:#x}, not the branch back')
    return bad
