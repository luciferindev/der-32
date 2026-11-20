#!/bin/sh
#
# DER/32 build driver.  Wraps NetBSD's build.sh (cross-builds from any
# POSIX host; developed on Arch Linux) and adds DER/32 steps.
#
#   scripts/build.sh check      verify the host environment
#   scripts/build.sh patches    fetch pinned source, apply patches/
#   scripts/build.sh tools      cross toolchain           -> out/tools
#   scripts/build.sh kernel     DER32 kernel               -> out/obj/.../DER32/netbsd
#   scripts/build.sh world      userland + upkg (DESTDIR)  -> out/destdir
#   scripts/build.sh sets       release sets + kern-DER32  -> out/release
#   scripts/build.sh image      bootable disk image        -> out/images/*.img
#   scripts/build.sh iso        bootable ISO               -> out/images/*.iso
#   scripts/build.sh repo       signed test repository     -> out/repos
#   scripts/build.sh release    all of the above, in order
#
# Environment:
#   DER32_OUT   output directory (default: <tree>/out)
#   JOBS        parallel jobs (default: number of CPUs + 1)
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src
OUT=${DER32_OUT:-$ROOT/out}
LOGDIR=$OUT/logs
MACHINE=amd64
REL="$SRC/sys/conf/der32.release"
# exists only once patches/ is applied (a fresh clone has pristine NetBSD)
[ -f "$REL" ] && . "$REL"
: "${DER32_RELEASE:=0.1}"

ncpu() {
	getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 2
}
JOBS=${JOBS:-$(( $(ncpu) + 1 ))}

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf 'build.sh: warning: %s\n' "$*" >&2; }
die()  { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ host

check_env() {
	missing=""
	for c in cc c++ make git awk sed tar gzip xz python3 pkg-config; do
		command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
	done
	host=$(uname -s)
	if [ -r /etc/os-release ]; then
		host="$host / $(. /etc/os-release; echo "${PRETTY_NAME:-$NAME}")"
	fi
	echo "host:      $host ($(uname -m)), $(ncpu) CPUs, JOBS=$JOBS"
	echo "tree:      $ROOT"
	echo "output:    $OUT"
	if [ -n "$missing" ]; then
		echo "missing:  $missing"
		echo
		echo "Install them, e.g. on Arch Linux:"
		echo "  sudo pacman -S --needed base-devel git xz python pkgconf"
		echo "On Debian/Ubuntu:"
		echo "  sudo apt install build-essential git xz-utils python3 pkg-config"
		return 1
	fi
	mkdir -p "$OUT"
	avail=$(df -Pk "$OUT" | awk 'NR == 2 { print int($4 / 1048576) }')
	echo "disk:      ${avail} GiB free in $(df -P "$OUT" | awk 'NR==2{print $6}')"
	[ "$avail" -ge 20 ] || warn "a full build needs about 20 GiB free"
	for c in qemu-system-x86_64 qemu-img; do
		command -v "$c" >/dev/null 2>&1 ||
		    warn "$c not found: 'make qemu' and 'make test' need it (Arch: pacman -S qemu-base; Debian: apt install qemu-system-x86 qemu-utils)"
	done
	for l in libarchive sqlite3 libcrypto libcurl; do
		pkg-config --exists "$l" 2>/dev/null ||
		    warn "host library $l not found: the host upkg build/tests need it (Arch: pacman -S libarchive sqlite openssl curl)"
	done
	[ -e /dev/kvm ] && [ -w /dev/kvm ] && echo "kvm:       available" ||
	    echo "kvm:       not available (QEMU will use slower TCG emulation)"
	return 0
}

# ------------------------------------------------------------------ helpers

# run_logged NAME command...: run with output to out/logs/NAME.log
run_logged() {
	name=$1; shift
	mkdir -p "$LOGDIR"
	log=$LOGDIR/$name.log
	say "$name (log: ${log#$ROOT/})"
	start=$(date +%s)
	if "$@" >"$log" 2>&1; then
		echo "    done in $(( $(date +%s) - start ))s"
	else
		rc=$?
		echo "---- last errors from $log ----" >&2
		grep -nE 'error:|Error code|\*\*\* Failed|ERROR:|missing|not found' "$log" |
		    head -20 >&2 || true
		echo "----" >&2
		tail -5 "$log" >&2
		die "$name failed (exit $rc); full log: $log"
	fi
}

# Modern host compilers reject parts of the toolchain sources NetBSD
# builds on the host (old GNU make, binutils, gcc): GCC >= 14 turns
# incompatible-pointer-types, implicit-function-declaration, implicit-int
# and int-conversion into errors, and GCC >= 15 defaults to C23.  Pin the
# host tools to gnu17 and demote exactly those errors back to warnings.
# This affects only programs built for the build host, never DER/32.
host_cflags() {
	hcc=${HOST_CC:-cc}
	flags="-O"
	for f in -std=gnu17 -Wno-error=incompatible-pointer-types \
	    -Wno-error=implicit-function-declaration -Wno-error=implicit-int \
	    -Wno-error=int-conversion -Wno-error=return-mismatch; do
		if echo 'int x;' | $hcc $f -Werror -x c -c -o /dev/null - \
		    >/dev/null 2>&1; then
			flags="$flags $f"
		fi
	done
	echo "$flags"
}

nbbuild() {
	: "${HOST_CFLAGS:=$(host_cflags)}"
	export HOST_CFLAGS
	cd "$SRC"
	MAKECONF=$ROOT/conf/mk.conf ./build.sh -U -u -N1 -j"$JOBS" -m "$MACHINE" \
	    -O "$OUT/obj" -T "$OUT/tools" -D "$OUT/destdir" -R "$OUT/release" "$@"
}

nbmake() {
	[ -x "$OUT/tools/bin/nbmake-$MACHINE" ] || die "toolchain missing: run 'make tools'"
	MAKECONF=$ROOT/conf/mk.conf "$OUT/tools/bin/nbmake-$MACHINE" "$@"
}

need_patches() {
	[ -e "$SRC/build.sh" ] && [ "$(git -C "$SRC" config --get der32.series 2>/dev/null || true)" = \
	    "$(cat "$ROOT"/patches/*.patch | git hash-object --stdin)" ] ||
	    sh "$ROOT/scripts/patches.sh" apply
}

# ------------------------------------------------------------------ steps

do_tools()  { need_patches; run_logged tools  nbbuild tools; }
do_kernel() { need_patches; run_logged kernel nbbuild kernel=DER32; }
do_world()  { need_patches; run_logged world  nbbuild distribution; }

do_sets() {
	need_patches
	run_logged sets nbbuild sets
	run_logged releasekernel nbbuild releasekernel=DER32
	run_logged kernset nbmake -C "$SRC/etc" KERNEL_SETS=DER32 \
	    BUILD_KERNELS= EXTRA_KERNELS= kernset-DER32
	[ -f "$OUT/release/$MACHINE/binary/sets/kern-DER32.tar.xz" ] ||
	    die "kern-DER32 set was not produced"
}

do_repo() {
	sh "$ROOT/scripts/mkrepo.sh"
}

do_image() {
	need_patches
	extra=$OUT/image-extra
	rm -rf "$extra"
	mkdir -p "$extra/etc/upkg/keys"
	cp "$ROOT/conf/upkg/upkg.conf" "$ROOT/conf/upkg/repos.conf" "$extra/etc/upkg/"
	if [ -d "$OUT/repos" ] && [ -f "$OUT/keys/der32-test.pub" ]; then
		mkdir -p "$extra/usr/share/upkg/repos"
		for r in core extra community; do
			cp -R "$OUT/repos/$r" "$extra/usr/share/upkg/repos/"
		done
		cp -R "$OUT/repos/distfiles" "$extra/usr/share/upkg/"
		cp "$OUT/keys/der32-test.pub" "$extra/etc/upkg/keys/"
	else
		warn "no test repositories (run 'make repo'); image will have none"
	fi
	mkdir -p "$extra/usr/ports"
	cp -R "$ROOT/ports/." "$extra/usr/ports/"
	imgdir=$SRC/distrib/$MACHINE/der32image
	run_logged image sh -c '
	    set -e
	    "$0" -C "$1" obj
	    "$0" -C "$1" cleandir
	    "$0" -C "$1" DER32_EXTRA_DIR="$2" RELEASEDIR="$3" live_image
	' "$OUT/tools/bin/nbmake-$MACHINE" "$imgdir" "$extra" "$OUT/release"
	img=$OUT/obj/distrib/$MACHINE/der32image/der32-$DER32_RELEASE-$MACHINE.img
	[ -f "$img" ] || die "image not produced: $img"
	mkdir -p "$OUT/images"
	cp "$img" "$OUT/images/"
	(cd "$OUT/images" && sha256sum "$(basename "$img")" > "$(basename "$img").sha256")
	say "image: out/images/$(basename "$img") ($(du -h "$img" | cut -f1))"
}

do_iso() {
	sh "$ROOT/scripts/mkiso.sh"
}

case "${1:-}" in
check)   check_env ;;
patches) sh "$ROOT/scripts/patches.sh" apply ;;
tools)   check_env >/dev/null; do_tools ;;
kernel)  do_kernel ;;
world)   do_world ;;
sets)    do_sets ;;
repo)    do_repo ;;
image)   do_image ;;
iso)     do_iso ;;
release)
	check_env
	do_tools; do_kernel; do_world; do_sets; do_repo; do_image; do_iso
	say "DER/32 $DER32_RELEASE release artifacts in out/images/"
	ls -l "$OUT/images"
	;;
*)
	sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
