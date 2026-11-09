#!/usr/bin/env bash
# Rebuild the Issue #6343 module-level reproduction from a generated AtomicsUnit.sv.
#
# Usage:
#   ./run.sh /path/to/XiangShan/build/rtl/AtomicsUnit.sv
#
# The RTL must be generated at XiangShan commit c4cfd2b78 (or the same RTL).
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RTL=${1:-}
if [[ -z "${RTL}" || ! -f "${RTL}" ]]; then
  printf 'Usage: %s /path/to/AtomicsUnit.sv\n' "$0" >&2
  exit 2
fi

command -v verilator >/dev/null || { printf 'Missing verilator\n' >&2; exit 127; }
command -v python3 >/dev/null || { printf 'Missing python3\n' >&2; exit 127; }

WORK="${HERE}/.work"
rm -rf "${WORK}"
mkdir -p "${WORK}/build/rtl" "${WORK}/repro"
cp "${RTL}" "${WORK}/build/rtl/AtomicsUnit.sv"
cp "${HERE}/tb_atomics.cpp" "${HERE}/make_fixed.py" "${HERE}/difftest_stub.sv" "${WORK}/repro/"
cd "${WORK}/repro"
python3 make_fixed.py

build_one() {
  local rtl="$1" mdir="$2" prefix="$3" cflags="$4"
  echo "=== BUILD ${prefix} ==="
  verilator --cc --exe --build -j 4 --top-module AtomicsUnit \
    --prefix "${prefix}" --Mdir "${mdir}" -DSYNTHESIS \
    -Wno-fatal -Wno-WIDTH -Wno-UNOPTFLAT -Wno-CASEINCOMPLETE -Wno-LATCH \
    -CFLAGS "${cflags}" "${rtl}" difftest_stub.sv tb_atomics.cpp
}

build_one "${WORK}/build/rtl/AtomicsUnit.sv" obj_orig VAtomicsUnitOrig "-std=c++17"
build_one "${WORK}/repro/AtomicsUnit_fixed.sv" obj_fixed VAtomicsUnitFixed "-std=c++17 -DFIXED"

{
  echo "=== RUN original ==="
  ./obj_orig/VAtomicsUnitOrig
  echo "=== RUN fixed ==="
  ./obj_fixed/VAtomicsUnitFixed
} 2>&1 | tee run.log
