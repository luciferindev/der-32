#!/bin/sh
#
# DER/32 patch series management.
#
# src/ is a git submodule pinned to an upstream NetBSD commit (see
# UPSTREAM).  Every DER/32 change to the NetBSD tree lives in patches/ as a
# git-format-patch series and is applied on a local branch "der32" inside
# src/.  The outer repository therefore records exactly two things about the
# OS tree: the upstream commit and the DER/32 delta.
#
#   scripts/patches.sh status   show pinned commit and patch state
#   scripts/patches.sh apply    check out the pinned commit and apply patches/
#   scripts/patches.sh export   regenerate patches/ from commits on der32
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src
PATCHES=$ROOT/patches
. "$ROOT/UPSTREAM"

die() { echo "patches.sh: $*" >&2; exit 1; }

g() { git -C "$SRC" "$@"; }

ensure_src() {
	if [ ! -e "$SRC/.git" ]; then
		echo "==> fetching pinned NetBSD source ($UPSTREAM_COMMIT)"
		git -C "$ROOT" submodule update --init --depth 1 src ||
		    die "cannot initialise src/ (network access to $UPSTREAM_URL needed)"
	fi
	if ! g cat-file -e "$UPSTREAM_COMMIT^{commit}" 2>/dev/null; then
		echo "==> fetching upstream commit $UPSTREAM_COMMIT"
		g fetch --depth 1 origin "$UPSTREAM_COMMIT" ||
		    die "cannot fetch $UPSTREAM_COMMIT"
	fi
}

series_id() {
	cat "$PATCHES"/*.patch 2>/dev/null | git hash-object --stdin
}

cmd_status() {
	ensure_src
	echo "upstream:  $UPSTREAM_URL"
	echo "branch:    $UPSTREAM_BRANCH"
	echo "commit:    $UPSTREAM_COMMIT"
	echo "patches:   $(ls "$PATCHES"/*.patch 2>/dev/null | wc -l) in patches/"
	echo "src HEAD:  $(g rev-parse HEAD) ($(g rev-parse --abbrev-ref HEAD))"
	if [ "$(g config --get der32.series 2>/dev/null || true)" = "$(series_id)" ]; then
		echo "state:     patch series applied"
	else
		echo "state:     patch series NOT applied (run: make patches)"
	fi
	[ -z "$(g status --porcelain --untracked-files=no)" ] ||
	    echo "warning:   uncommitted changes in src/"
}

cmd_apply() {
	ensure_src
	want=$(series_id)
	if [ "$(g config --get der32.series 2>/dev/null || true)" = "$want" ] &&
	    [ "$(g rev-parse --abbrev-ref HEAD)" = der32 ]; then
		echo "==> patch series already applied"
		return 0
	fi
	if [ -n "$(g status --porcelain --untracked-files=no)" ] &&
	    [ -z "${DER32_FORCE:-}" ]; then
		die "src/ has uncommitted changes; commit them on der32 and run 'export', or set DER32_FORCE=1 to discard"
	fi
	echo "==> resetting src/ to $UPSTREAM_COMMIT and applying patches/"
	g checkout -q -f -B der32 "$UPSTREAM_COMMIT"
	g clean -q -fd -e /obj
	for p in "$PATCHES"/*.patch; do
		[ -e "$p" ] || continue
		echo "    $(basename "$p")"
		g -c user.name="DER/32 Build" -c user.email=build@der32.invalid \
		    am -q --keep-cr --committer-date-is-author-date "$p" ||
		    { g am --abort 2>/dev/null; die "patch $(basename "$p") does not apply"; }
	done
	g config der32.series "$want"
}

cmd_export() {
	ensure_src
	[ -z "$(g status --porcelain --untracked-files=no)" ] ||
	    die "commit your src/ changes on the der32 branch first"
	n=$(g rev-list --count "$UPSTREAM_COMMIT..HEAD")
	[ "$n" -gt 0 ] || die "no commits on top of $UPSTREAM_COMMIT"
	rm -f "$PATCHES"/*.patch
	mkdir -p "$PATCHES"
	g format-patch -q --no-signature --zero-commit --full-index \
	    -o "$PATCHES" "$UPSTREAM_COMMIT..HEAD"
	g config der32.series "$(series_id)"
	echo "==> exported $n patch(es) to patches/"
	ls "$PATCHES"
}

case "${1:-status}" in
status) cmd_status ;;
apply)  cmd_apply ;;
export) cmd_export ;;
*) die "usage: patches.sh status|apply|export" ;;
esac
