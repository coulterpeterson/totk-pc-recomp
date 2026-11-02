# Native-execution rewriter

Rewrites the game's modules so its own ARM64 code can run on an AArch64 Android host
([docs/native-execution.md](../../docs/native-execution.md)). `rewrite.py` is ours; the rest is MacNativeGames' copy of
d2r-mobile's rewriter and verifiers (MIT, `LICENSE`), at MacNativeGames `b75be8cfa704df3701e86130c8ced268e79d6b11`
(`games/totk/third_party/d2r/`), which carries its own TotK changes over d2r-mobile `9f0c257` (its `README.md`).

```sh
.venv/bin/python tools/native/rewrite.py --version v0 --verify
```

Output in `build/<version>-native/`: per module the patched image (`<m>.patched.bin`: image, bss, a 16 KiB key page,
then the patch area), `fixups.tsv`, `gates.tsv`, `sites.tsv`; `layout.json` / `layout.h` with each module given room
for its patch area; `patchsites.tsv`, the patch sets' one-word sites, which the loader writes into the code while their
set is on (`runtime/native.c` `native_patches_sync`).

Changes for Android, each marked "Android" in the code:
- Slot base. The Apple rewrite reaches its per-thread slots (guest x18, TPIDR_EL0, TPIDRRO_EL0, the gate address)
  through `mrs xT, tpidrro_el0`, Darwin's thread-specific data. Linux reads 0 there and bionic owns TPIDR_EL0, so the
  base is `mrs xT, tpidr_el0; ldr xT, [xT, #16]`: the slot block's address, which the runtime keeps in bionic's
  `TLS_SLOT_APP` (slot 2, available to apps since Android 10). One word longer; the key page is unchanged.
  `patch_nso.py` `emit_tp`, `call_window.py` `prepare_call`.
- Counters. `mrs xN, cntpct_el0` traps at EL0 on Linux; it becomes `mrs xN, cntvct_el0` in place, which reads at the
  Switch's 19.2 MHz (`--probe-exec`), and `cntvct_el0` reads stay as they are. No gate for either.
- Spin waits. The `spin` lines of `recomp/patches-<version>.txt` (loops only another thread ends), passed in
  `DM_SPINS` by `rewrite.py`: each back-edge goes to a block that keeps its condition and calls the SVC gate (kind
  `spin` in `gates.tsv`, the loop head as argument) on the way back to the head, where the runtime runs
  `rt_spin_gate` as the lifted code does. `patch_nso.py` `site_spin`.
- The verifiers check those forms: `structck.py` (slot sequences, the call window template, counter sites, spin
  blocks, no other system register or `tpidrro_el0` read in the patch area), `equiv.py` (the rewritten side runs with TPIDR_EL0 at a
  stand-in bionic TCB whose slot 2 points at the slots, TPIDRRO_EL0 0, and must leave both as they were).

D2R's build-keyed tables (CUTS, OBS, PARAMS, rule sets) stay in the copy and never match a TotK build; their
parameter-gate and host-call checks still expect the Apple slot form.
