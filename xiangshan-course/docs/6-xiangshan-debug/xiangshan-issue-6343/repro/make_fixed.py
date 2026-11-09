#!/usr/bin/env python3
from pathlib import Path

here = Path(__file__).resolve().parent
src = here.parent / "build" / "rtl" / "AtomicsUnit.sv"
dst = here / "AtomicsUnit_fixed.sv"
text = src.read_text()
needle = """      pdest2Valid <= ~_GEN_16 & _GEN_15 & (_GEN & ~_GEN_1 & _GEN_3 | pdest2Valid);
      if (_GEN_16 | ~_GEN_15)
"""
replacement = """      pdest2Valid <= ~_GEN_16 & _GEN_15 & (_GEN & ~_GEN_1 & _GEN_3 | pdest2Valid);
      // Test-only fix: clear hardwareError exactly when resetFSM completes.
      // _GEN_14 is first WB; CAS.Q continues to finish2 when _io_in_ready_T_1.
      if (_GEN_16 | _GEN_14 & ~_io_in_ready_T_1)
        exceptionVec_19 <= 1'h0;
      if (_GEN_16 | ~_GEN_15)
"""
if text.count(needle) != 1:
    raise SystemExit(f"expected one patch point, found {text.count(needle)}")
dst.write_text(text.replace(needle, replacement))
print(f"generated {dst} from {src}")
