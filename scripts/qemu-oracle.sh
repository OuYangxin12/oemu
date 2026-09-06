#!/usr/bin/env bash
#
# qemu-oracle.sh -- run a guest under qemu-system-aarch64 and assert markers.
#
# usage: scripts/qemu-oracle.sh <image.bin> <MARKER>... [--expect-exit N]
#        [--timeout SEC] [--stdin STR] [--stdin-delay SEC]
#
# The QEMU half of the L4 differential discipline (docs/verification-strategy.md):
# QEMU is the oracle, so a guest's expected serial output is captured HERE
# first, and the same assertion set replays against `oemu boot` when the
# milestone's implementation lands. Exit codes:
#   0  every marker seen and the exit contract held
#   1  a marker is missing or the exit code mismatched (log kept, path printed)
#   2  usage error
#   3  no qemu-system-aarch64 available (clean skip, never a false failure)
set -u

usage() { sed -n '3,6p' "$0"; exit 2; }

[ $# -ge 2 ] || usage
image=$1
shift
[ -f "$image" ] || { echo "qemu-oracle: no such image: $image" >&2; exit 2; }

qemu=${OEMU_QEMU:-}
if [ -z "$qemu" ]; then
  for cand in "$(dirname "$0")/../guest/qemu-root/usr/bin/qemu-system-aarch64" \
              "$(command -v qemu-system-aarch64 || true)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then qemu=$cand; break; fi
  done
fi
if [ -z "$qemu" ]; then
  echo "qemu-oracle: skipped -- no qemu-system-aarch64 (set OEMU_QEMU)" >&2
  exit 3
fi

expect_exit=""
timeout_sec=20
stdin_text=""
stdin_delay=0
markers=()
while [ $# -gt 0 ]; do
  case "$1" in
    --expect-exit) expect_exit=$2; shift 2 ;;
    --timeout) timeout_sec=$2; shift 2 ;;
    --stdin) stdin_text=$2; shift 2 ;;
    --stdin-delay) stdin_delay=$2; shift 2 ;;
    -*) usage ;;
    *) markers+=("$1"); shift ;;
  esac
done
[ ${#markers[@]} -gt 0 ] || usage

# -virtualization=off removes EL2 so -kernel boots the guest at EL1, matching
# `oemu boot`. EL3 stays present (secure=on default); the PSCI conduit comes
# from the DTB QEMU generates, and the guests probe it exactly like the
# baseline kernel did (docs/linux-minimal-qemu.md).
workdir=$(mktemp -d)
log="$workdir/serial.log"
feed="$workdir/stdin.fifo"
mkfifo "$feed"

if [ -n "$stdin_text" ]; then
  (
    sleep "$stdin_delay"
    printf '%s\n' "$stdin_text"
  ) > "$feed" &
else
  : > "$feed" &
fi

"$qemu" -machine virt,virtualization=off -cpu cortex-a53 -m 256M \
  -nographic \
  -kernel "$image" < "$feed" > "$log" 2>&1 &
qemu_pid=$!
qemu_rc=timeout
( while kill -0 "$qemu_pid" 2>/dev/null; do sleep 0.2; done ) & watcher=$!
for _ in $(seq 1 $((timeout_sec * 5))); do
  if ! kill -0 "$qemu_pid" 2>/dev/null; then wait "$qemu_pid"; qemu_rc=$?; break; fi
  sleep 0.2
done
if [ "$qemu_rc" = "timeout" ] && kill -0 "$qemu_pid" 2>/dev/null; then
  # TERM first: QEMU flushes its serial output on graceful shutdown, so a
  # WFI-parked guest still leaves its markers in the log. KILL is the last
  # resort only.
  kill "$qemu_pid" 2>/dev/null
  for _ in $(seq 1 10); do
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "$qemu_pid" 2>/dev/null
  wait "$qemu_pid" 2>/dev/null
fi
kill "$watcher" 2>/dev/null

failed=0
for m in "${markers[@]}"; do
  if ! grep -qF -- "$m" "$log"; then
    echo "qemu-oracle: MARKER MISSING: $m" >&2
    failed=1
  fi
done

if [ $failed -eq 0 ] && [ -n "$expect_exit" ]; then
  if [ "$qemu_rc" = "timeout" ]; then
    echo "qemu-oracle: guest parked in WFI but --expect-exit $expect_exit was set" >&2
    failed=1
  elif [ "$qemu_rc" -ne "$expect_exit" ]; then
    echo "qemu-oracle: exit code $qemu_rc != expected $expect_exit" >&2
    failed=1
  fi
fi

if [ $failed -ne 0 ]; then
  echo "qemu-oracle: FAILED (serial log kept at $log)" >&2
  rm -f "$feed"
  exit 1
fi
rm -rf "$workdir"
echo "qemu-oracle: PASS (markers: ${markers[*]}; exit: $qemu_rc)"
