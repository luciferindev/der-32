#!/bin/sh
#
# DER/32 automated tests.
#
#   scripts/test.sh            everything below that the current build supports
#   scripts/test.sh upkg       upkg integration suite (host, isolated temp root)
#   scripts/test.sh build      verify build artifacts and DER/32 identity in them
#   scripts/test.sh qemu       boot the release image in QEMU and test the system
#
# Evidence is written to out/logs/test-*.log.
#
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
LOGDIR=$OUT/logs
. "$ROOT/src/sys/conf/der32.release" 2>/dev/null || DER32_RELEASE=0.1
IMG=$OUT/images/der32-$DER32_RELEASE-amd64.img
KERNEL=$OUT/obj/sys/arch/amd64/compile/DER32/netbsd
mkdir -p "$LOGDIR"
FAIL=0

t_upkg() {
	echo "== upkg integration tests"
	make -C "$ROOT/upkg" >/dev/null || { FAIL=1; return; }
	sh "$ROOT/upkg/tests/run.sh" "$ROOT/upkg/upkg" 2>&1 |
	    tee "$LOGDIR/test-upkg.log" | tail -3
	grep -q 'failed: 0$' "$LOGDIR/test-upkg.log" || FAIL=1
}

ck() {	# ck "description" command...
	d=$1; shift
	if "$@" >/dev/null 2>&1; then echo "  ok   $d"; else echo "  FAIL $d"; FAIL=1; fi
}

t_build() {
	echo "== build artifacts"
	{
	ck "toolchain built"              test -x "$OUT/tools/bin/nbmake-amd64"
	ck "DER32 kernel compiled"        test -f "$KERNEL"
	ck "kernel ostype is DER32" \
	    grep -q 'const char ostype\[\] = "DER32";' "$(dirname "$KERNEL")/vers.c"
	ck "kernel version string is DER/32" \
	    sh -c "strings '$KERNEL' | grep -q '^DER/32 $DER32_RELEASE GENESIS (DER32)'"
	ck "kernel has kern.der32 sysctl code" \
	    sh -c "strings '$KERNEL' | grep -q 'DER/32 release identification'"
	ck "userland: /bin/sh"            test -f "$OUT/destdir/bin/sh"
	ck "userland: libc"               test -f "$OUT/destdir/usr/lib/libc.so"
	ck "userland: upkg in base"       test -f "$OUT/destdir/usr/sbin/upkg"
	ck "userland: /etc/release is DER/32" \
	    sh -c "head -1 '$OUT/destdir/etc/release' | grep -q '^DER/32 '"
	ck "release sets: base"           test -f "$OUT/release/amd64/binary/sets/base.tar.xz"
	ck "release sets: kern-DER32"     test -f "$OUT/release/amd64/binary/sets/kern-DER32.tar.xz"
	ck "bootable image exists"        test -f "$IMG"
	ck "image has an MBR boot signature" \
	    sh -c "[ \"\$(od -An -tx1 -j510 -N2 '$IMG' | tr -d ' ')\" = 55aa ]"
	} 2>&1 | tee "$LOGDIR/test-build.log"
	grep -q FAIL "$LOGDIR/test-build.log" && FAIL=1
}

t_qemu() {
	echo "== QEMU boot and system test"
	[ -f "$IMG" ] || { echo "  FAIL no image ($IMG)"; FAIL=1; return; }
	command -v qemu-system-x86_64 >/dev/null ||
	    { echo "  FAIL qemu-system-x86_64 not installed"; FAIL=1; return; }
	python3 "$ROOT/scripts/qemu-test.py" "$IMG" "$LOGDIR/qemu-test.log" |
	    tee "$LOGDIR/test-qemu.log"
	grep -q ', 0 failed$' "$LOGDIR/test-qemu.log" || FAIL=1
	iso=${IMG%.img}.iso
	if [ -f "$iso" ]; then
		echo "== QEMU live ISO boot test"
		python3 "$ROOT/scripts/qemu-test.py" "$iso" "$LOGDIR/qemu-test-iso.log" |
		    tee "$LOGDIR/test-qemu-iso.log"
		grep -q ', 0 failed$' "$LOGDIR/test-qemu-iso.log" || FAIL=1
	fi
}

case "${1:-all}" in
upkg)  t_upkg ;;
build) t_build ;;
qemu)  t_qemu ;;
all)   t_upkg; t_build; t_qemu ;;
*) echo "usage: test.sh [upkg|build|qemu]" >&2; exit 2 ;;
esac
[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" || echo "TESTS FAILED"
exit "$FAIL"
