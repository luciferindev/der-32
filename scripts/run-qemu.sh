#!/bin/sh
#
# Boot DER/32 in QEMU with the serial console on this terminal.
#
#   scripts/run-qemu.sh [image]       boot a persistent working copy
#   SNAPSHOT=1 scripts/run-qemu.sh    discard all disk writes on exit
#
# The release image itself is never modified: the default run uses
# out/images/der32-run.img, created from the release image on first use.
# Exit QEMU with Ctrl-A X.  Log in as root (password: der32).
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
REL="$ROOT/src/sys/conf/der32.release"
# exists only once patches/ is applied (a fresh clone has pristine NetBSD)
[ -f "$REL" ] && . "$REL"
: "${DER32_RELEASE:=0.1}"
case ${QEMU_CONSOLE:-serial} in
serial) suffix= ;;
pc) suffix=-pc ;;
*) echo "QEMU_CONSOLE must be serial or pc" >&2; exit 2 ;;
esac
IMG=${1:-$OUT/images/der32-$DER32_RELEASE-amd64$suffix.img}

command -v qemu-system-x86_64 >/dev/null ||
    { echo "qemu-system-x86_64 not found (Arch: pacman -S qemu-base)" >&2; exit 1; }
[ -f "$IMG" ] || { echo "no image at $IMG: run 'make release'" >&2; exit 1; }

if [ -n "${SNAPSHOT:-}" ]; then
	disk="file=$IMG,format=raw,if=ide,snapshot=on"
else
	run=$OUT/images/der32-run$suffix.img
	[ -f "$run" ] || cp "$IMG" "$run"
	disk="file=$run,format=raw,if=ide"
fi

if [ -w /dev/kvm ]; then
	accel="-accel kvm -cpu host"
else
	accel="-accel tcg"
fi

if [ "${QEMU_CONSOLE:-serial}" = pc ]; then
    # Graphical display + emulated PS/2 keyboard for ttyE0/wscons.
    exec qemu-system-x86_64 $accel -m "${MEM:-1024}" -smp "${SMP:-2}" \
        -drive "$disk" -netdev user,id=net0 -device e1000,netdev=net0 \
        -vga std -display default -serial none -monitor none
fi
exec qemu-system-x86_64 $accel -m "${MEM:-1024}" -smp "${SMP:-2}" \
    -drive "$disk" \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -nographic
