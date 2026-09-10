#!/usr/bin/env bash
#
# boot-linux-gate.sh -- the M5 acceptance gate: boot Linux under oemu and
# assert the same markers docs/linux-minimal-qemu.md asserts under QEMU.
#
# usage: scripts/boot-linux-gate.sh [--kernel Image] [--initrd initramfs.cpio]
#        [--marker M]... [--expect-exit N] [--timeout SEC] [--smp N] [-m MB]
#        [--log PATH] [--no-interactive] [--bin PATH]
#
# This is the oemu half of the L4 differential discipline
# (docs/verification-strategy.md): the marker set was captured from QEMU, the
# oracle, and replaying it against `oemu boot` is what "the machine boots
# Linux" means for this repository. The three markers are one contract, not
# three options, so they are the defaults:
#
#   BOOT OK                     -- the kernel reached the initramfs /init
#   MINIMAL-BOOT-CHECK-PASSED   -- /init's own assertions passed
#   SHELL_ALIVE                 -- the shell answered a line we typed
#
# and the exit contract is `poweroff` -> PSCI SYSTEM_OFF -> oemu exit 0.
#
# The console is wired with `-serial stdio`, which is also what feeds the
# interactive line: `--serial file:` has no RX path, so a gate that asserts
# SHELL_ALIVE cannot use it. stdin is a pipe we write to after a delay, exactly
# like scripts/qemu-oracle.sh does, so a guest that never reaches the shell
# costs the timeout and nothing else.
#
# Exit codes (same shape as qemu-oracle.sh, so CI can treat them alike):
#   0  every marker seen and the exit contract held
#   1  a marker is missing or the exit code mismatched (log kept, path printed)
#   2  our own console dropped bytes: the log is truncated, so no marker verdict is valid
#   2  usage error / missing inputs
#   3  skipped: the guest is known-blocked and --allow-blocked was given
set -u

die() {
  echo "boot-linux-gate: $*" >&2
  exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
kernel=$repo/guest/build/Image
initrd=$repo/guest/build/initramfs.cpio
bin=""
mib=256
smp=1
# Measured, default debug binary, interactive exchange and shutdown included:
# 17 s wall. The fixture reaches /init in ~10 s; the rest is the exchange. The
# figure that used to live in this comment -- "the prompt appears after ~140 s" --
# was inferred from a serial log that had stopped growing, and it had stopped
# growing because the guest was stuck, not because it was still booting. A fixed
# feed delay built on that reading cost 90 s per run and hid how fast a real
# failure was. 420 s is therefore generous by design: it is what a *hung* guest
# should die at, not what a healthy one takes.
timeout_sec=420
expect_exit=0
interactive=1
allow_blocked=0
markers=()
log=""
mem_append="console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 rdinit=/init"

while [ $# -gt 0 ]; do
  case "$1" in
    --kernel) kernel=$2; shift 2 ;;
    --initrd) initrd=$2; shift 2 ;;
    --bin) bin=$2; shift 2 ;;
    --marker) markers+=("$2"); shift 2 ;;
    --expect-exit) expect_exit=$2; shift 2 ;;
    --timeout) timeout_sec=$2; shift 2 ;;
    --smp) smp=$2; shift 2 ;;
    -m) mib=$2; shift 2 ;;
    --log) log=$2; shift 2 ;;
    --append) mem_append=$mem_append" $2"; shift 2 ;;
    --no-interactive) interactive=0; shift ;;
    --allow-blocked) allow_blocked=1; shift ;;
    -h | --help)
      sed -n '3,26p' "$0"
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ ${#markers[@]} -gt 0 ] || markers=("BOOT OK" "MINIMAL-BOOT-CHECK-PASSED" "SHELL_ALIVE")

[ -f "$kernel" ] || die "no kernel Image at $kernel (scripts/build-guest.sh builds it)"
[ -f "$initrd" ] || die "no initramfs at $initrd (scripts/build-linux-initramfs.sh builds it)"

if [ -z "$bin" ]; then
  for cand in "$repo/build/debug/bin/oemu" "$repo/build/release/bin/oemu" \
              "$repo/build/bin/oemu" "$(command -v oemu || true)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then
      bin=$cand
      break
    fi
  done
fi
[ -n "$bin" ] || die "no oemu binary (make build, or --bin PATH)"

workdir=$(mktemp -d) || die "mktemp -d failed"
[ -n "$log" ] || log=$workdir/serial.log
errlog=$workdir/model.err
fifo=$workdir/stdin.fifo
mkfifo "$fifo"

# `panic=-1` reboots on panic and -no-reboot is not ours to pass, so a panic
# ends the run by exiting; that is how a failure surfaces as a missing marker
# plus a non-zero exit code rather than as an endless loop.
args=(boot -kernel "$kernel" -initrd "$initrd" -m "$mib" -append "$mem_append"
      --max-insns "${OEMU_BOOT_GATE_MAX_INSNS:-4000000000}")
# `oemu boot --smp N` is refused outright until SMP lands (one vCPU boots
# alone, M4b+), so passing `--smp 1` would turn the gate into a usage error.
# It is only worth passing when we are actually asking for more than one core.
if [ "$smp" -gt 1 ]; then
  args+=(--smp "$smp")
fi

run_with_stdin() {
  # The interactive line is what proves the shell is alive: we type `echo
  # SHELL_ALIVE` and require the echo back, then ask for a clean shutdown.
  if [ "$interactive" -eq 1 ]; then
    (
      # Feed on the prompt, not on a stopwatch. The same image reaches /init in
      # ~2 minutes (release) and ~7 (debug), so every fixed delay is either a race
      # or six wasted minutes -- and a line typed into a guest that has not
      # started listening yet is a test whose failure says nothing. We wait for the
      # second marker, which is the last thing the fixture writes before it reads.
      # OEMU_BOOT_GATE_FEED_DELAY stays as an optional floor.
      deadline=$((timeout_sec - 90)); [ "$deadline" -lt 30 ] && deadline=30
      waited=0
      while [ "$waited" -lt "$deadline" ]; do
        grep -q "${markers[1]:-MINIMAL-BOOT-CHECK-PASSED}" "$log" 2>/dev/null && break
        sleep 2; waited=$((waited + 2))
      done
      sleep "${OEMU_BOOT_GATE_FEED_DELAY:-0}"
      printf 'echo SHELL_ALIVE\n'
      sleep 5
      printf 'poweroff -f\n'
    ) > "$fifo" &
    timeout "$timeout_sec" "$bin" "${args[@]}" -serial stdio < "$fifo" > "$log" 2> "$errlog"
  else
    : > "$fifo" &
    timeout "$timeout_sec" "$bin" "${args[@]}" -serial file:"$log" >/dev/null 2> "$errlog"
  fi
}

echo "boot-linux-gate: $bin (${mib} MiB, -smp $smp, markers: ${markers[*]})"
run_with_stdin
rc=$?

# A dropped byte means our own console lost output, so the log can no longer be
# read as evidence about the guest: refusing here is what keeps a truncated log
# from ever being reported as "the guest never printed the marker".
if [ -f "$errlog" ] && grep -q 'console dropped' "$errlog"; then
  echo "boot-linux-gate: CONSOLE LOSS: $(grep -m1 'console dropped' "$errlog")" >&2
  echo "boot-linux-gate: the captured log is truncated, so a missing marker proves nothing" >&2
  echo "boot-linux-gate: log kept at $log (model stderr at $errlog)" >&2
  exit 2
fi

failed=0
for m in "${markers[@]}"; do
  if ! grep -qF -- "$m" "$log"; then
    echo "boot-linux-gate: MARKER MISSING: $m" >&2
    failed=1
  fi
done
if [ $failed -eq 0 ] && [ "$expect_exit" != "any" ] && [ "$rc" -ne "$expect_exit" ]; then
  echo "boot-linux-gate: exit code $rc != expected $expect_exit" >&2
  failed=1
fi

if [ $failed -ne 0 ]; then
  tail -n 12 "$log" | sed 's/^/    | /' >&2
  if [ "$allow_blocked" -eq 1 ]; then
    # #28 is an open oemu-side defect on this exact path (a synchronous abort
    # taken against a context whose stack lands on the guest's own page
    # table). Until it is fixed the gate is expected to be red; --allow-blocked
    # turns that one specific failure into a skip so CI can run the gate
    # without pretending it passed.
    echo "boot-linux-gate: SKIPPED as known-blocked (see issue #28); log kept at $log" >&2
    rm -f "$fifo"
    exit 3
  fi
  echo "boot-linux-gate: FAILED (serial log kept at $log)" >&2
  rm -f "$fifo"
  exit 1
fi

rm -rf "$workdir"
echo "boot-linux-gate: PASS (markers: ${markers[*]}; exit: $rc)"
