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
# Distfiles: lua-5.4.7.tar is "git archive" of the upstream v5.4.7 tag,
# which is byte-for-byte reproducible; fetch it once if it is missing.
# (The recipe pins its SHA-256, checked below.)
if [ ! -f "$OUT/distfiles/lua-5.4.7.tar" ]; then
	echo "==> fetching lua 5.4.7 source (git archive of github.com/lua/lua v5.4.7)"
	mkdir -p "$OUT/distfiles"
	t=$(mktemp -d)
	git clone -q --depth 1 -b v5.4.7 https://github.com/lua/lua "$t/lua" ||
	    die "cannot fetch lua (network access to github.com needed)"
	git -C "$t/lua" archive --format=tar --prefix=lua-5.4.7/ v5.4.7 \
	    > "$OUT/distfiles/lua-5.4.7.tar.tmp"
	rm -rf "$t"
	mv "$OUT/distfiles/lua-5.4.7.tar.tmp" "$OUT/distfiles/lua-5.4.7.tar"
fi
cp "$OUT/distfiles/lua-5.4.7.tar" "$REPOS/distfiles/"

# recipe_value FILE KEY
recipe_value() { awk -v k="$2" '$1 == k { sub(/^[^ \t]+[ \t]+/, ""); print; exit }' "$1"; }

# fetch_git_distfile RECIPE FILE: make a distfile from the recipe's
# "#@git URL REF PREFIX" line as "git archive" of that ref, which is
# reproducible byte for byte (the recipe's source_sha256 is checked after).
fetch_git_distfile() {
	set -- "$1" "$2" $(awk '$1 == "#@git" { print $2, $3, $4; exit }' "$1")
	[ $# -eq 5 ] || die "${2##*/} missing and $1 has no #@git line"
	echo "==> fetching ${2##*/} (git archive of $3 $4)"
	t=$(mktemp -d)
	git -c advice.detachedHead=false clone -q --depth 1 -b "$4" "$3" "$t/src" ||
	    die "cannot fetch $3 (network access to github.com needed)"
	mkdir -p "${2%/*}"
	git -C "$t/src" archive --format=tar --prefix="$5/" "$4" > "$2.tmp"
	rm -rf "$t"
	mv "$2.tmp" "$2"
}

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
		[ -f "$f" ] || fetch_git_distfile "$recipe" "$f"
		cp "$f" "$REPOS/distfiles/"
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
build_port extra     "$ROOT/ports/extra/tree"
build_port extra     "$ROOT/ports/extra/nnn"
# a newer der32-hello in the lowest-priority repository: exercises
# repository priority and explicit repository selection on DER/32
build_port community "$ROOT/ports/core/der32-hello" 1.1

for r in core extra community; do
	"$UPKG" index -n "$r" -k "$OUT/keys/der32-test.key" "$REPOS/$r"
done
echo "==> test repositories in ${REPOS#$ROOT/}; signing key ${OUT#$ROOT/}/keys/der32-test.pub"
