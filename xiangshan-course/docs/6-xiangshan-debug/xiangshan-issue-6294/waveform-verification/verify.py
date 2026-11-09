#!/usr/bin/env python3
"""Re-check the Issue #6294 claims against replay.fst.

Usage:
  ~/wavekit/.venv/bin/python verify.py --wave replay.fst [--json out.json]

Sampling: TOP.clock, rising edge. The load runs in inner_LoadUnit_0.
"""

import argparse
import json
import sys

import numpy as np
from wavekit import FstReader

P = "TOP.SimTop.cpu.l_soc.core_with_l2.core.memBlock.inner_LoadUnit_0."
LO = 0x3ffffffe650ca
HI = 0x3ffffffe650d0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wave", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    checks = []
    ev = {}

    with FstReader(args.wave) as r:
        def load(s):
            return r.load_waveform(P + s, clock="TOP.clock", sample_on_posedge=True)

        def at(w, c):
            return int(w.value[int(np.argmin(np.abs(w.clock - c)))])

        def mask(prefix, c):
            m = 0
            for b in range(16):
                m |= at(load(f"{prefix}_s2Resp_bits_forwardMask_{b}"), c) << b
            return m

        def fdata(prefix, c):
            d = 0
            for b in range(16):
                d |= at(load(f"{prefix}_s2Resp_bits_forwardData_{b}"), c) << (8 * b)
            return d

        s0v = load("io_sqForward_s0Req_valid")
        s0a = load("io_sqForward_s0Req_bits_vaddr")
        lower = [int(s0a.clock[i]) for i in np.nonzero((s0a.value == LO) & (s0v.value == 1))[0]]
        upper = [int(s0a.clock[i]) for i in np.nonzero((s0a.value == HI) & (s0v.value == 1))[0]]
        if not lower or not upper:
            print("FAIL no split load request found")
            return 1
        c = lower[-1]
        ev["lower_s0_cycle"] = c
        ev["upper_s0_cycle"] = upper[-1]

        # S2 responses appear two cycles after the S0 query in this build.
        ev["lower_s2"] = {
            "cycle": c + 2,
            "sq_mask": mask("io_sqForward", c + 2),
            "sb_mask": mask("io_sbufferForward", c + 2),
        }
        ev["upper_s2"] = {
            "cycle": c + 3,
            "sq_mask": mask("io_sqForward", c + 3),
            "sq_data": fdata("io_sqForward", c + 3),
        }

        ldout_v = load("io_ldout_toRob_bits_debugInfo_vaddr")
        intrf_v = load("io_ldout_toIntRf_valid")
        idx = np.nonzero((ldout_v.value == LO) & (intrf_v.value == 1))[0]
        if not len(idx):
            print("FAIL no committed load writeback found")
            return 1
        wc = int(ldout_v.clock[idx[-1]])
        ev["commit"] = {
            "cycle": wc,
            "vaddr": LO,
            "data": at(load("io_ldout_toIntRf_bits_data"), wc),
            "pdest": at(load("io_ldout_toIntRf_bits_pdest"), wc),
        }

        checks.append(("lower half S2 SQ forward mask is 0", ev["lower_s2"]["sq_mask"] == 0))
        checks.append(("lower half S2 SBuffer forward mask is 0", ev["lower_s2"]["sb_mask"] == 0))
        checks.append(("upper half S2 SQ mask covers 0xfc03",
                       ev["upper_s2"]["sq_mask"] == 0xFC03))
        checks.append(("upper half SQ forward data contains 0x20008",
                       (ev["upper_s2"]["sq_data"] >> 80) & 0xFFFFFF == 0x20008))
        checks.append(("committed load data is 0", ev["commit"]["data"] == 0))
        checks.append(("committed load writes physical register 95",
                       ev["commit"]["pdest"] == 95))

    ok = True
    for name, passed in checks:
        print(("PASS " if passed else "FAIL ") + name)
        ok = ok and passed

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"checks": [{"name": n, "pass": p} for n, p in checks], "evidence": ev},
                      f, indent=1)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
