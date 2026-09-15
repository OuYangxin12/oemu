#!/bin/sh
#
# scripts/bench-report.sh -- the in-guest benchmark comparison table.
#
# Parses two `PERF <name> ns=... ms=... ops=... r=...` transcripts (see
# bench/perf/oemu-perf.c) -- one from the QEMU oracle, one from oemu -- and
# prints a per-test table: oracle time, oemu time, the ratio, and whether the
# result checksums agree. A MISMATCH line means the two machines did not
# compute the same thing: that is an emulator defect surfaced as a number,
# which is the whole point of the oracle discipline, so the script exits
# non-zero when any checksum diverges even if every timing looks fine.
#
# usage: scripts/bench-report.sh <oracle.log> <oemu.log>
set -eu

[ $# -eq 2 ] || { sed -n '3,13p' "$0"; exit 2; }
[ -r "$1" ] && [ -r "$2" ] || { echo "bench-report: unreadable log" >&2; exit 2; }

awk '
  # First pass: oracle timings and checksums; second: the measured side.
  /^PERF [a-z0-9-]+ ns=/ {
    name = $2; ns = 0; ms = 0; r = "";
    for (i = 3; i <= NF; i++) {
      if ($i ~ /^ns=/)  { split($i, a, "="); ns = a[2] }
      if ($i ~ /^ms=/)  { split($i, a, "="); ms = a[2] }
      if ($i ~ /^r=/)   { split($i, a, "="); r  = a[2] }
    }
    if (FNR == NR) { ons[name] = ns; oms[name] = ms; ors[name] = r; order[++n] = name }
    else           { tns[name] = ns; tms[name] = ms; trs[name] = r }
    next
  }
  /^PERF TOTAL ms=/ {
    split($3, a, "=");
    if (FNR == NR) { otot = a[2] } else { ttot = a[2] }
    next
  }
  END {
    printf "%-12s %14s %14s %8s  %s\n", "test", "QEMU ms", "oemu ms", "ratio", "checksum"
    bad = 0
    for (i = 1; i <= n; i++) {
      k = order[i]
      if (trs[k] == "") { printf "%-12s missing on the measured side\n", k; bad = 1; continue }
      ratio = (tms[k] + 0) / (oms[k] + 0.0000001)
      ok = (ors[k] == trs[k]) ? "ok" : "MISMATCH"
      if (ok == "MISMATCH") bad = 1
      printf "%-12s %14s %14s %7.1fx  %s\n", k, oms[k], tms[k], ratio, ok
    }
    if (ttot != "") printf "%-12s %14s %14s %7.1fx\n", "TOTAL", otot, ttot,
           (ttot + 0) / (otot + 0.0000001)
    exit bad
  }
' "$1" "$2"
