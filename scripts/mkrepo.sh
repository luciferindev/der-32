#!/bin/sh
#
# Build DER/32's official *test* package repositories (core, extra,
# community) from the recipes in ports/, cross-compiled with the DER/32
# toolchain against the DER/32 userland (out/destdir), then sign them.
#
# Output:
#   out/repos/{core,extra,community}/   INDEX, INDEX.sig, *.upkg
#   out/repos/distfiles/                source archives used by the ports
#   out/keys/der32-test.{key,pub}       test signing key (generated once;
#                                       the private key is never committed)
#
# The same recipes build natively inside DER/32 with "upkg build".
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
TOOLS=$OUT/tools
SYSROOT=$OUT/destdir
WORK=$OUT/repo-work
REPOS=$OUT/repos
UPKG=$ROOT/upkg/upkg
ARCH=amd64

die() { echo "mkrepo.sh: $*" >&2; exit 1; }

CROSSCC=$TOOLS/bin/x86_64--netbsd-gcc
[ -x "$CROSSCC" ] || die "cross compiler missing ($CROSSCC): run 'make tools'"
[ -f "$SYSROOT/usr/lib/libc.so" ] || die "DER/32 userland missing: run 'make world'"
[ -x "$UPKG" ] || make -C "$ROOT/upkg" >/dev/null || die "cannot build host upkg"

mkdir -p "$OUT/keys"
[ -f "$OUT/keys/der32-test.key" ] ||
    "$UPKG" keygen "$OUT/keys/der32-test" >/dev/null

rm -rf "$WORK" "$REPOS"
mkdir -p "$WORK" "$REPOS/core" "$REPOS/extra" "$REPOS/community" "$REPOS/distfiles"
cp "$OUT/distfiles/lua-5.4.7.tar" "$REPOS/distfiles/" 2>/dev/null ||
    die "missing out/distfiles/lua-5.4.7.tar (see docs/building.md)"

# recipe_value FILE KEY
recipe_value() { awk -v k="$2" '$1 == k { sub(/^[^ \t]+[ \t]+/, ""); print; exit }' "$1"; }

# build_port REPO PORTDIR [VERSION-OVERRIDE]
build_port() {
	repo=$1 port=$2 over=${3:-}
	recipe=$port/RECIPE
	name=$(recipe_value "$recipe" name)
	ver=${over:-$(recipe_value "$recipe" version)}
	rel=$(recipe_value "$recipe" release)
	arch=$(recipe_value "$recipe" arch); arch=${arch:-$ARCH}
	src=$(recipe_value "$recipe" source)
	sum=$(recipe_value "$recipe" source_sha256)
	w=$WORK/$name-$ver
	mkdir -p "$w/src" "$w/dest"
	if [ -n "$src" ]; then
		f=$OUT/distfiles/${src##*/}
		[ "$(sha256sum "$f" | cut -d' ' -f1)" = "$sum" ] ||
		    die "$name: distfile checksum mismatch"
		tar -xf "$f" -C "$w/src"
	fi
	sd=$w/src
	[ "$(ls "$w/src" | wc -l)" -eq 1 ] && [ -d "$w/src/$(ls "$w/src")" ] &&
	    sd=$w/src/$(ls "$w/src")
	echo "==> $repo/$name $ver-$rel ($arch)"
	(cd "$sd" && env -i PATH="$PATH" HOME="$w" \
	    CC="$CROSSCC --sysroot=$SYSROOT" CFLAGS="-O2 -DPKGVERSION=\"$ver\"" \
	    SRCDIR="$sd" DESTDIR="$w/dest" PREFIX=/usr/local PORTDIR="$port" \
	    PKGNAME="$name" PKGVERSION="$ver" UPKG_ARCH="$ARCH" LC_ALL=C \
	    sh -e "$port/build") > "$w/build.log" 2>&1 ||
	    { cat "$w/build.log"; die "$name: build failed"; }
	{
		grep -vE '^[[:space:]]*(#|$)|^(source|source_sha256|builddep|version|arch)[[:space:]]' "$recipe"
		echo "version $ver"
		echo "arch $arch"
	} > "$w/PKGINFO"
	"$UPKG" create -i "$w/PKGINFO" -d "$w/dest" \
	    -o "$REPOS/$repo/$name-$ver-$rel-$arch.upkg" >/dev/null
	# every ELF object must be a NetBSD (DER/32 ABI) amd64 executable
	find "$w/dest" -type f | while read -r f; do
		if head -c 4 "$f" | grep -q 'ELF'; then
			file "$f" | grep -q 'NetBSD' || die "$f is not a NetBSD ELF binary"
			file "$f" | sed "s|$w/dest||; s/^/    /"
		fi
	done
}

build_port core      "$ROOT/ports/core/der32-hello"
build_port core      "$ROOT/ports/core/lua"
build_port extra     "$ROOT/ports/extra/lua-hello"
# a newer der32-hello in the lowest-priority repository: exercises
# repository priority and explicit repository selection on DER/32
build_port community "$ROOT/ports/core/der32-hello" 1.1

for r in core extra community; do
	"$UPKG" index -n "$r" -k "$OUT/keys/der32-test.key" "$REPOS/$r"
done
echo "==> test repositories in ${REPOS#$ROOT/}; signing key ${OUT#$ROOT/}/keys/der32-test.pub"
