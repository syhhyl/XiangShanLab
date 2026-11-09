# AtomicsUnit stale hardwareError RTL reproduction

## Result

PASS on both required traces. The unchanged generated RTL reproduces the bug: op B writes back `exceptionVec[6]=1` and stale `exceptionVec[19]=1`. The test-only fixed copy writes back bit 6 = 1 and bit 19 = 0 under the identical harness.

## Environment and DUT

- WSL distro: Ubuntu-22.04
- Verilator: 5.048 (2026-04-26 rev v5.048)
- Compiler observed: g++ 11.4.0; Verilator selected clang++ for generated makefiles
- Source DUT: `../build/rtl/AtomicsUnit.sv`, current SHA-256 `9e54c69c4c777affdbb0e3099f0f38b545cb5ec67658e2025495a7fc81c8c07f`
- `LSUOpType.amoswap_d` is `b001011` (`0x0b`), confirmed in `src/main/scala/xiangshan/package.scala`; generated ports and widths were confirmed from the SV/Verilator header.

The current standalone generated DUT has `io.out.ready` constant-propagated to 1, so each observed `io_out_valid` is a real output transfer. The harness drives all remaining external module inputs. `difftest_stub.sv` only supplies the missing non-functional `DummyDPICWrapper_LrScEvent` event sink needed to elaborate this standalone module; it does not replace functional logic.

## Build and run

From WSL:

```bash
cd /home/lhf/project/xiangshan/xs-env/XiangShan/atomics_stale_hwerr_repro
./build_and_run.sh
# or: make
```

The script regenerates `AtomicsUnit_fixed.sv`, rebuilds both variants, runs both, and preserves `build.log` and `run.log`.

## Driven trajectory

Defaults are zero, except dcache request ready = 1, flush_sbuffer.empty = 1, cache_error_enable = 1, and implicit output ready = 1. DTLB/PMP responses are clean hits. For each op, the harness deliberately responds on the first DTLB request cycle and verifies it is ignored, then holds the response for the next request cycle.

1. Reset.
2. Op A: aligned `amoswap.d`, STA address `0x1000`, then required STD data. A clean TLB translation and PMP response advance to dcache. The harness sees a real dcache request and returns non-miss/non-replay with `tl_corrupt=1`, `tl_denied=0`. Op A WB must have bit 19 = 1 and bit 6 = 0, and really fires.
3. Wait for idle (`io_in_ready`).
4. Op B: misaligned `amoswap.d`, STA address `0x2003`. No STD is needed for this early-exit path. A clean TLB response detects D-width misalignment. Op B must write back bit 6 = 1 without any new dcache request.
5. Original PASS requires op B bit 19 = 1. Fixed PASS requires op B bit 19 = 0.

Every wait has a 200-cycle timeout and reports ready/request/output state on failure.

## Actual run log

See `run.log`. Key lines:

```text
cycle 13 opA WB fire bit6=0 bit19=1
cycle 17 opB WB fire bit6=1 bit19=1
PASS[original]: opA bit19=1; opB bit6=1 bit19=1; opB dcache requests=0
cycle 13 opA WB fire bit6=0 bit19=1
cycle 17 opB WB fire bit6=1 bit19=0
PASS[fixed]: opA bit19=1; opB bit6=1 bit19=0; opB dcache requests=0
```

## Fixed control semantics

`make_fixed.py` copies the source DUT and inserts only this test-only state clear:

```systemverilog
if (_GEN_16 | _GEN_14 & ~_io_in_ready_T_1)
  exceptionVec_19 <= 1'h0;
```

In this generated RTL, `_GEN_14` is the first writeback fire in `s_finish`, `_io_in_ready_T_1` identifies AMOCAS.Q (which continues to `s_finish2`), and `_GEN_16` is the `s_finish2` writeback fire. Thus the condition is precisely the generated equivalent of clearing `hardwareError` whenever `resetFSM()` completes, while preserving it during the second AMOCAS.Q writeback. No cache response, exception detection, or other operation behavior changes.

## Limits

This is module-level testing of the complete generated AtomicsUnit, not a whole-core simulation. Real external AtomicsUnit interfaces are directly driven. The generated non-functional difftest sink is stubbed because its wrapper is not included in this standalone RTL file. Results apply to the recorded generated DUT checksum; if it is regenerated, the script intentionally fails if its exact fixed-copy insertion point no longer matches.
