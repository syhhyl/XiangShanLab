#!/usr/bin/env python3
"""Re-check the Issue #6289 claims against replay.fst.

Usage:
  ~/wavekit/.venv/bin/python verify.py --wave replay.fst [--json out.json]

Sampling: TOP.clock, rising edge.
"""

import argparse
import json
import sys

import numpy as np
from wavekit import FstReader

A = "TOP.SimTop.cpu.l_soc.core_with_l2.core.memBlock.inner_atomicsUnit."
S = "TOP.SimTop.cpu.l_soc.core_with_l2.core.memBlock.inner_sbuffer."
Q = "TOP.SimTop.cpu.l_soc.core_with_l2.core.memBlock.inner_lsq.storeQueue.physicalQueue."
V = "TOP.SimTop.cpu.l_soc.core_with_l2.core.memBlock.inner_lsq.storeQueue.virtualStoreQueue."


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wave", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    checks = []
    ev = {}

    with FstReader(args.wave) as r:
        def load(path):
            return r.load_waveform(path, clock="TOP.clock", sample_on_posedge=True)

        def at(w, c):
            return int(w.value[int(np.argmin(np.abs(w.clock - c)))])

        clk = load("TOP.clock")
        last = int(clk.clock[-1])
        ev["wave_cycles"] = [int(clk.clock[0]), last]

        state = load(A + "state")
        flush_empty = load(S + "io_flush_empty")
        sqempty = load(S + "io_sqempty")
        ev["final_atom_state"] = at(state, last)
        ev["final_sqempty"] = at(sqempty, last)
        ev["final_flush_empty"] = at(flush_empty, last)

        checks.append(("final AMO state is s_wait_flush_sbuffer_resp (3)",
                       ev["final_atom_state"] == 3))
        checks.append(("final sqempty is 0", ev["final_sqempty"] == 0))
        checks.append(("final flush_empty is 0", ev["final_flush_empty"] == 0))

        slot = {}
        for f in ["ctrlEntries_3_addrValid", "ctrlEntries_3_dataValid",
                  "ctrlEntries_3_hasException", "ctrlEntries_3_committed"]:
            slot[f.split("_")[-1]] = at(load(Q + f), last)
        slot["vaddr"] = at(load(Q + "dataEntries_3_vaddr"), last)
        slot["robIdx"] = at(load(Q + "dataEntries_3_debugUop_robIdx_value"), last)
        slot["numLsElem"] = at(load(Q + "dataEntries_3_debugUop_numLsElem"), last)
        ev["slot3"] = slot
        checks.append(("slot3 hasException=1", slot["hasException"] == 1))
        checks.append(("slot3 committed=0", slot["committed"] == 0))
        checks.append(("slot3 addrValid=1 and dataValid=1",
                       slot["addrValid"] == 1 and slot["dataValid"] == 1))
        checks.append(("slot3 vaddr=0x5c0", slot["vaddr"] == 0x5C0))
        checks.append(("slot3 robIdx=0x12e", slot["robIdx"] == 0x12E))

        ptr = {}
        for f in ["deqPtrExt_0_value", "rdataPtrExt_0_value", "commitPtrExt_0_value",
                  "io_fromVirtualStoreQueue_physicalQueueEnqPtr_value"]:
            ptr[f] = at(load(Q + f), last)
        ev["pointers"] = ptr
        checks.append(("deqPtr=3 at end", ptr["deqPtrExt_0_value"] == 3))
        checks.append(("commitPtr=3 at end", ptr["commitPtrExt_0_value"] == 3))

        # Transition anchors.
        for prefix, f in [(Q, "ctrlEntries_3_hasException"),
                          (V, "ctrlEntries_3_vecMbCommit"),
                          (V, "retireValidReg"),
                          (V, "retireReqNumReg"),
                          (V, "toPhysicalQueueRetirePtr_value"),
                          (V, "io_toPhysicalQueue_redirectPtr_bits_value")]:
            w = load(prefix + f)
            v = w.value
            ch = np.nonzero(np.diff(v))[0] + 1
            ev[f] = [(int(w.clock[i]), int(v[i])) for i in ch][:20]

        checks.append(("vecMbCommit pulses for slot3",
                       any(c == 1 for _, c in ev["ctrlEntries_3_vecMbCommit"])))
        checks.append(("retire pointer reaches 5",
                       any(c == 5 for _, c in ev["toPhysicalQueueRetirePtr_value"])))
        checks.append(("redirect recovery pointer is 5",
                       any(c == 5 for _, c in ev["io_toPhysicalQueue_redirectPtr_bits_value"])))

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
