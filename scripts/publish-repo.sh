#!/bin/sh
#
# Publish DER/32's package repositories to the project website.
#
# Takes the repositories built by "make repo" (out/repos), re-signs each
# index with the DER/32 *release* key, and writes them to
#   site/repo/<release>/<arch>/{core,extra,community}/
# which GitHub Pages serves as https://luciferindev.github.io/der-32/repo/.
# Images trust the release key through conf/upkg/keys/der32-release.pub.
#
# The private key is never committed.  Point UPKG_RELEASE_KEY at it
# (default: out/keys/der32-release.key), or in CI keep it in the
# UPKG_RELEASE_KEY secret and write it to a file first.
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${DER32_OUT:-$ROOT/out}
. "$ROOT/src/sys/conf/der32.release" 2>/dev/null || DER32_RELEASE=0.1
KEY=${UPKG_RELEASE_KEY:-$OUT/keys/der32-release.key}
UPKG=$ROOT/upkg/upkg
ARCH=amd64
DEST=$ROOT/site/repo/$DER32_RELEASE/$ARCH

die() { echo "publish-repo.sh: $*" >&2; exit 1; }
[ -f "$KEY" ] || die "release key $KEY not found (set UPKG_RELEASE_KEY)"
[ -d "$OUT/repos/core" ] || die "no repositories: run 'make repo' first"
[ -x "$UPKG" ] || make -C "$ROOT/upkg" >/dev/null

# The key must match the public key the images trust.
pub=$ROOT/conf/upkg/keys/der32-release.pub
if command -v openssl >/dev/null 2>&1; then
	[ "$(openssl pkey -in "$KEY" -pubout 2>/dev/null)" = "$(cat "$pub")" ] ||
	    die "$KEY does not match $pub"
fi

for r in core extra community; do
	rm -rf "$DEST/$r"
	mkdir -p "$DEST/$r"
	cp "$OUT/repos/$r"/*.upkg "$DEST/$r/" 2>/dev/null || true
	"$UPKG" index -n "der32-$r" -k "$KEY" "$DEST/$r" >/dev/null
	echo "==> der32-$r: $(ls "$DEST/$r"/*.upkg 2>/dev/null | wc -l | tr -d ' ') package(s) -> ${DEST#$ROOT/}/$r"
done
# Source archives, so "upkg build" works on DER/32 from the recipes.
rm -rf "$ROOT/site/repo/distfiles"
mkdir -p "$ROOT/site/repo/distfiles"
cp "$OUT/repos/distfiles"/* "$ROOT/site/repo/distfiles/"

cat > "$ROOT/site/repo/index.html" <<HTML
<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>DER/32 package repositories</title>
<link rel="stylesheet" href="../style.css"></head>
<body><div id="wrap" style="padding:12px">
<h2>DER/32 package repositories</h2>
<p>These are upkg repositories, not web pages. On DER/32: <tt>upkg sync</tt>, then <tt>upkg search</tt>.</p>
<ul>
$(for r in core extra community; do echo "<li><a href=\"$DER32_RELEASE/$ARCH/$r/INDEX\">der32-$r</a> ($DER32_RELEASE, $ARCH)</li>"; done)
</ul>
<p>Indexes are signed with the DER/32 release key,
<a href="https://github.com/luciferindev/der-32/blob/main/conf/upkg/keys/der32-release.pub">der32-release.pub</a>.</p>
<p><a href="../index.html">&laquo; DER/32 home</a></p>
</div></body></html>
HTML
echo "==> published; commit site/repo and push to update GitHub Pages"
