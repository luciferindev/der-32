#!/bin/sh
#
# upkg integration test suite.
#
# Everything happens below a fresh temporary directory: an isolated install
# root (upkg -r), three local repositories, a throwaway signing key and, for
# the transport test, a local HTTPS server.  Nothing outside $T is touched.
#
# usage: sh tests/run.sh /path/to/upkg
#
set -u

UPKG_BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/upkg-test.XXXXXX")
chmod 755 "$T"
ROOT=$T/root
REPOS=$T/repos
LOG=$T/log
PASS=0
FAIL=0
FAILED=""
HTTPD_PID=""

cleanup() {
	[ -n "$HTTPD_PID" ] && kill "$HTTPD_PID" 2>/dev/null
	if [ "$FAIL" -eq 0 ] && [ -z "${KEEP:-}" ]; then
		rm -rf "$T"
	else
		echo "test artifacts kept in $T"
	fi
}
trap cleanup EXIT

u() { "$UPKG_BIN" -r "$ROOT" "$@"; }

ok() {
	PASS=$((PASS + 1))
	printf '  ok   %s\n' "$1"
}

nok() {
	FAIL=$((FAIL + 1))
	FAILED="$FAILED
  - $1"
	printf '  FAIL %s\n' "$1"
	sed 's/^/       | /' "$LOG" | tail -15
}

# expect_ok "description" command...
expect_ok() {
	d=$1; shift
	if "$@" >"$LOG" 2>&1; then ok "$d"; else nok "$d"; fi
}

# expect_fail "description" "pattern" command...
expect_fail() {
	d=$1; pat=$2; shift 2
	if "$@" >"$LOG" 2>&1; then
		nok "$d (unexpectedly succeeded)"
	elif [ -n "$pat" ] && ! grep -q -- "$pat" "$LOG"; then
		nok "$d (missing message: $pat)"
	else
		ok "$d"
	fi
}

check() {
	d=$1; shift
	if "$@" >"$LOG" 2>&1; then ok "$d"; else nok "$d"; fi
}

installed_version() {
	u list 2>/dev/null | awk -v p="$1" '$1 == p { print $2 }'
}

# mkpkg repo name version release [key value ...]
#   creates usr/local/bin/<name> and usr/local/share/<name>/README;
#   extra "file" args add usr/local/share/<name>/<file>
mkpkg() {
	repo=$1 name=$2 ver=$3 rel=$4; shift 4
	st=$T/stage/$name-$ver-$rel
	rm -rf "$st"
	mkdir -p "$st/usr/local/bin" "$st/usr/local/share/$name"
	printf '#!/bin/sh\necho "%s %s-%s"\n' "$name" "$ver" "$rel" \
	    > "$st/usr/local/bin/$name"
	chmod 755 "$st/usr/local/bin/$name"
	echo "$name $ver" > "$st/usr/local/share/$name/README"
	{
		echo "format 1"
		echo "name $name"
		echo "version $ver"
		echo "release $rel"
		echo "arch ${ARCH:-amd64}"
		echo "description test package $name"
		while [ $# -ge 2 ]; do
			if [ "$1" = file ]; then
				echo "$2" > "$st/usr/local/share/$name/$2"
			else
				echo "$1 $2"
			fi
			shift 2
		done
	} > "$T/stage/$name.info"
	mkdir -p "$REPOS/$repo"
	"$UPKG_BIN" create -i "$T/stage/$name.info" -d "$st" \
	    -o "$REPOS/$repo/$name-$ver-$rel-${ARCH:-amd64}.upkg" >/dev/null
}

index() {
	"$UPKG_BIN" index -n "$1" -k "$T/keys/test.key" "$REPOS/$1" >/dev/null
}

echo "upkg test suite: $($UPKG_BIN --version)"
echo "workdir: $T"

# ------------------------------------------------------------------ setup
echo "== setup"
mkdir -p "$T/keys" "$ROOT/etc/upkg/keys" "$T/stage"
expect_ok "keygen creates an Ed25519 key pair" \
    "$UPKG_BIN" keygen "$T/keys/test"
expect_ok "keygen creates a second (untrusted) key" \
    "$UPKG_BIN" keygen "$T/keys/evil"
cp "$T/keys/test.pub" "$ROOT/etc/upkg/keys/"
check "private key is mode 0600" \
    sh -c "ls -l '$T/keys/test.key' | grep -q '^-rw-------'"
cat > "$ROOT/etc/upkg/upkg.conf" <<EOF
[options]
arch = amd64
prefixes = usr/local
builddir = $T/build
EOF

mkpkg core libfoo 1.0 1 file old.txt
mkpkg core curl 8.0 1 depend "libfoo>=1.0"
mkpkg core zlib 1.3 1
mkpkg extra vim 9.0 1 depend libfoo
mkpkg extra libfoo 2.0 1
mkpkg extra needy 1.0 1 depend "libfoo>=5"
mkpkg community neovim 0.9 1 depend "libfoo>=1.0" conflict vim
mkpkg community vim 9.1 1 depend libfoo
mkpkg community clash 1.0 1
ARCH=sparc64 mkpkg community sparconly 1.0 1
# 'clash' ships a file that zlib also ships
cp "$T/stage/zlib-1.3-1/usr/local/bin/zlib" "$T/stage/clash-1.0-1/usr/local/bin/zlib"
"$UPKG_BIN" create -i "$T/stage/clash.info" -d "$T/stage/clash-1.0-1" \
    -o "$REPOS/community/clash-1.0-1-amd64.upkg" >/dev/null
for r in core extra community; do index $r; done

expect_ok "repo add core (priority 100)" u repo add core "file://$REPOS/core" -p 100
expect_ok "repo add extra (priority 80)" u repo add extra "file://$REPOS/extra" -p 80
expect_ok "repo add community (priority 50)" u repo add community "$REPOS/community" -p 50
expect_fail "repo add rejects duplicates" "already exists" \
    u repo add core "file://$REPOS/core"
expect_fail "repo add rejects unsupported URL schemes" "unsupported" \
    u repo add bad "ftp://example.org/x"
check "repo list shows priority order" sh -c \
    "$UPKG_BIN -r '$ROOT' repo list | awk '{print \$1}' | tr '\n' ' ' | grep -q '^core extra community '"

# ------------------------------------------------------------------ sync
echo "== synchronisation and signatures"
expect_fail "install before sync finds nothing" "no package satisfies" u install zlib
expect_ok "sync verifies and caches all indexes" u sync
check "search finds packages across repositories" \
    sh -c "$UPKG_BIN -r '$ROOT' search libfoo | grep -q 'core/libfoo 1.0-1' && $UPKG_BIN -r '$ROOT' search libfoo | grep -q 'extra/libfoo 2.0-1'"
check "info shows repository metadata" \
    sh -c "$UPKG_BIN -r '$ROOT' info curl | grep -q 'Depends on   : libfoo>=1.0'"

cp "$REPOS/community/INDEX" "$T/INDEX.good"
cp "$REPOS/community/INDEX.sig" "$T/INDEX.sig.good"
sed 's/test package neovim/TAMPERED/' "$T/INDEX.good" > "$REPOS/community/INDEX"
expect_fail "tampered index is rejected" "signature verification FAILED" u sync
check "tampered index did not replace the cached copy" \
    sh -c "! grep -q TAMPERED '$ROOT/var/lib/upkg/sync/community.INDEX'"
"$UPKG_BIN" index -n community -k "$T/keys/evil.key" "$REPOS/community" >/dev/null
expect_fail "index signed by an untrusted key is rejected" \
    "signature verification FAILED" u sync
cp "$REPOS/core/INDEX" "$REPOS/community/INDEX"
cp "$REPOS/core/INDEX.sig" "$REPOS/community/INDEX.sig"
expect_fail "validly signed index of another repository is rejected" \
    "belongs to repository 'core'" u sync
sleep 1
index community
expect_ok "re-signed index is accepted" u sync
cp "$T/INDEX.good" "$REPOS/community/INDEX"
cp "$T/INDEX.sig.good" "$REPOS/community/INDEX.sig"
expect_fail "older (replayed) index is rejected" "replay" u sync
index community
expect_ok "fresh index accepted again" u sync
rm -f "$REPOS/community/INDEX.sig"
expect_fail "missing signature is an error" "" u sync
index community
u sync >/dev/null 2>&1

# ------------------------------------------------------------------ install
echo "== installation and dependency resolution"
expect_ok "install curl" u install curl
check "dependency libfoo was installed with curl" test -x "$ROOT/usr/local/bin/libfoo"
check "curl payload installed and runnable" \
    sh -c "'$ROOT/usr/local/bin/curl' | grep -q 'curl 8.0-1'"
check "libfoo recorded as a dependency, curl as explicit" \
    sh -c "$UPKG_BIN -r '$ROOT' list -d | grep -q '^libfoo' && $UPKG_BIN -r '$ROOT' list -e | grep -q '^curl'"
check "priority: libfoo 1.0 came from core (prio 100), not extra's 2.0" \
    test "$(installed_version libfoo)" = "1.0-1"
check "provenance recorded" \
    sh -c "$UPKG_BIN -r '$ROOT' info libfoo | grep -q 'Repository   : core'"
check "list -f shows owned files" \
    sh -c "$UPKG_BIN -r '$ROOT' list -f curl | grep -q '^/usr/local/bin/curl$'"
expect_ok "check verifies installed files" u check
echo garbage >> "$ROOT/usr/local/share/curl/README"
expect_fail "check detects a modified file" "modified" u check
echo "curl 8.0" > "$ROOT/usr/local/share/curl/README"
expect_ok "explicit repository selection: extra/vim" u install extra/vim
check "vim 9.0 came from extra" test "$(installed_version vim)" = "9.0-1"
expect_fail "unsatisfiable version constraint is reported" \
    "'libfoo>=5'" u install needy
expect_fail "unknown package" "no package satisfies 'nosuchpkg'" u install nosuchpkg
expect_fail "unknown repository" "unknown repository 'nope'" u install nope/zlib
expect_fail "architecture mismatch excludes a package" \
    "no package satisfies 'sparconly'" u install sparconly
expect_fail "declared conflict (neovim conflicts vim)" "conflicts with installed" \
    u install neovim
expect_ok "install zlib" u install zlib
expect_fail "file ownership conflict is detected" "already owned by zlib" \
    u install clash
check "failed install left nothing behind" \
    sh -c "! ls -R '$ROOT/usr/local' | grep -q upkg-new && test -z \"\$($UPKG_BIN -r '$ROOT' list | grep clash)\""
touch "$ROOT/usr/local/bin/neovim"
u remove vim >/dev/null 2>&1
expect_fail "unowned file in the way is not overwritten" "not owned by any package" \
    u install neovim
rm -f "$ROOT/usr/local/bin/neovim"
expect_ok "neovim installs once vim is gone and the path is free" u install neovim
expect_fail "vim now conflicts with installed neovim" "conflicts with vim" \
    u install community/vim
u remove neovim >/dev/null 2>&1
expect_ok "dry run changes nothing" u install -n community/vim
check "dry run did not install vim" test -z "$(installed_version vim)"

# ------------------------------------------------------------------ removal
echo "== removal"
expect_ok "install community/vim (newer)" u install community/vim
check "vim 9.1 from community" test "$(installed_version vim)" = "9.1-1"
expect_fail "removing a required package is refused" "required by" u remove libfoo
check "libfoo still installed after refusal" test -n "$(installed_version libfoo)"
expect_ok "remove curl" u remove curl
check "curl files removed" test ! -e "$ROOT/usr/local/bin/curl"
check "curl's empty share directory removed" test ! -e "$ROOT/usr/local/share/curl"
expect_ok "autoremove keeps libfoo (still needed by vim)" u autoremove
check "libfoo still present" test -n "$(installed_version libfoo)"
expect_ok "remove vim" u remove vim
expect_ok "autoremove removes the orphaned libfoo" u autoremove
check "libfoo gone" test -z "$(installed_version libfoo)"
expect_ok "install curl again" u install curl
expect_ok "remove -c removes dependents too" u remove -c libfoo
check "cascade removed curl" test -z "$(installed_version curl)"
expect_fail "removing a package that is not installed" "not installed" u remove curl

# ------------------------------------------------------------------ upgrade
echo "== upgrades"
u install curl >/dev/null
sleep 1
mkpkg core libfoo 1.1 1 file new.txt
rm -f "$REPOS/core/libfoo-1.0-1-amd64.upkg"
index core
expect_ok "sync picks up the new index" u sync
expect_ok "upgrade" u upgrade
check "libfoo upgraded to 1.1" test "$(installed_version libfoo)" = "1.1-1"
check "upgrade replaced file contents" \
    sh -c "'$ROOT/usr/local/bin/libfoo' | grep -q 'libfoo 1.1-1'"
check "file dropped by the new version was removed" \
    test ! -e "$ROOT/usr/local/share/libfoo/old.txt"
check "file added by the new version exists" \
    test -e "$ROOT/usr/local/share/libfoo/new.txt"
check "upgrade kept the install reason" \
    sh -c "$UPKG_BIN -r '$ROOT' list -d | grep -q '^libfoo'"
expect_ok "nothing left to upgrade" sh -c "$UPKG_BIN -r '$ROOT' upgrade | grep -q 'nothing to do'"
expect_ok "check after upgrade" u check

# ------------------------------------------------------------------ hostile input
echo "== invalid and malicious packages"
for k in traversal absolute outside-prefix symlink-escape symlink-absolute \
    bad-checksum setuid unlisted-member wrong-arch bad-metadata not-a-package; do
	python3 "$HERE/mkbadpkg.py" "$k" "$T/$k.upkg"
	expect_fail "rejects package: $k" "" u install --allow-unsigned "$T/$k.upkg"
done
check "no hostile file escaped the root" \
    sh -c "test ! -e /tmp/upkg-evil && test ! -e /tmp/upkg-evil-abs && test ! -e '$ROOT/etc/evil.conf' && test ! -e '$ROOT/usr/local/bin/passwd' && test ! -e '$ROOT/usr/local/bin/evil'"
check "no staged leftovers after rejections" \
    sh -c "! find '$ROOT' -name '*.upkg-new' | grep -q ."
check "database unchanged by rejected packages" test -z "$(installed_version evil)"
expect_fail "local package without --allow-unsigned is refused" "allow-unsigned" \
    u install "$REPOS/core/zlib-1.3-1-amd64.upkg"
cp "$REPOS/core/zlib-1.3-1-amd64.upkg" "$T/zlib.bak"
cp "$REPOS/community/clash-1.0-1-amd64.upkg" "$REPOS/core/zlib-1.3-1-amd64.upkg"
u remove zlib >/dev/null 2>&1
rm -rf "$ROOT/var/cache/upkg"
expect_fail "package not matching the signed index checksum is refused" \
    "checksum mismatch" u install zlib
cp "$T/zlib.bak" "$REPOS/core/zlib-1.3-1-amd64.upkg"
mkdir -p "$ROOT/usr/local/share"
ln -s /tmp "$ROOT/usr/local/share/zlib"
expect_fail "planted symlink in place of a package directory is refused" \
    "is not a directory" u install zlib
rm -f "$ROOT/usr/local/share/zlib"

expect_ok "zlib installs normally afterwards" u install zlib

# ------------------------------------------------------------------ recovery
echo "== interrupted transactions"
UPKG_TEST_CRASH=after-stage u install neovim >"$T/crash.log" 2>&1
check "install killed after staging (SIGKILL)" grep -q "test crash at 'after-stage'" "$T/crash.log"
check "journal and staged files left behind" \
    sh -c "test -f '$ROOT/var/lib/upkg/journal' && find '$ROOT' -name '*.upkg-new' | grep -q ."
expect_ok "recover rolls the uncommitted install back" u recover
check "rollback: neovim not installed, no files, no journal" \
    sh -c "test -z \"\$($UPKG_BIN -r '$ROOT' list | grep neovim)\" && test ! -e '$ROOT/usr/local/bin/neovim' && test ! -e '$ROOT/var/lib/upkg/journal' && ! find '$ROOT' -name '*.upkg-new' | grep -q ."
u remove -c libfoo >/dev/null 2>&1
UPKG_TEST_CRASH=after-commit u install curl >"$T/crash.log" 2>&1
check "install killed after the database commit" grep -q "after-commit" "$T/crash.log"
check "files not yet in place" test ! -e "$ROOT/usr/local/bin/curl"
expect_ok "next command recovers automatically (roll forward)" u install zlib
check "roll forward: curl files in place" test -x "$ROOT/usr/local/bin/curl"
expect_ok "roll forward produced a consistent system" u check
UPKG_TEST_CRASH=after-commit u remove curl >"$LOG" 2>&1
expect_ok "recover completes an interrupted removal" u recover
check "removal completed" sh -c "test ! -e '$ROOT/usr/local/bin/curl' && test -z \"\$($UPKG_BIN -r '$ROOT' list | grep '^curl')\""
UPKG_TEST_CRASH=before-commit u install curl >/dev/null 2>&1
expect_ok "recover after a crash just before commit" u recover
check "nothing half-installed" sh -c "test ! -e '$ROOT/usr/local/bin/curl'"
expect_ok "consistent after all recovery scenarios" u check

# SIGINT during staging: hold the transaction open, interrupt it
UPKG_TEST_HOLD_LOCK=2 "$UPKG_BIN" -r "$ROOT" install curl >"$T/sigint.log" 2>&1 &
pid=$!
sleep 1
expect_fail "concurrent upkg is refused while the lock is held" "locked" \
    u remove zlib
kill -INT $pid
wait $pid
rc=$?
check "SIGINT before staging aborts cleanly" \
    sh -c "[ $rc -ne 0 ] && test ! -e '$ROOT/var/lib/upkg/journal' && test ! -e '$ROOT/usr/local/bin/curl'"
expect_ok "consistent after SIGINT" u check

# ------------------------------------------------------------------ repos
echo "== repository management"
u remove -c libfoo >/dev/null 2>&1
expect_ok "disable core" u repo disable core
expect_ok "install libfoo from the next repository" u install libfoo
check "libfoo 2.0 from extra while core is disabled" \
    test "$(installed_version libfoo)" = "2.0-1"
expect_fail "disabled repository cannot be selected explicitly" "disabled" \
    u install core/zlib
expect_ok "enable core" u repo enable core
expect_ok "repo priority change" u repo priority extra 200
check "extra now listed first" \
    sh -c "$UPKG_BIN -r '$ROOT' repo list | head -1 | grep -q '^extra'"
expect_ok "repo remove community" u repo remove community
check "community gone from repos.conf" sh -c "! grep -q community '$ROOT/etc/upkg/repos.conf'"

# ------------------------------------------------------------------ https
echo "== HTTPS transport"
if command -v openssl >/dev/null && command -v python3 >/dev/null; then
	openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
	    -addext "subjectAltName=DNS:localhost" \
	    -keyout "$T/tls.key" -out "$T/tls.crt" >/dev/null 2>&1
	PORT=$((20000 + $$ % 20000))
	PYTHONDONTWRITEBYTECODE=1 python3 - "$REPOS" "$T/tls.crt" "$T/tls.key" "$PORT" <<'EOF' &
import http.server, ssl, sys, functools
d, crt, key, port = sys.argv[1:5]
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=d)
class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
h = functools.partial(Q, directory=d)
s = http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), h)
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
c.load_cert_chain(crt, key)
s.socket = c.wrap_socket(s.socket, server_side=True)
s.serve_forever()
EOF
	HTTPD_PID=$!
	sleep 1
	u repo add community "https://localhost:$PORT/community" -p 10 >/dev/null
	expect_fail "untrusted TLS certificate is rejected" "" u sync
	printf 'ca_file = %s\n' "$T/tls.crt" >> "$ROOT/etc/upkg/upkg.conf"
	expect_ok "sync over HTTPS with the configured CA" u sync
	expect_ok "install over HTTPS (community/neovim)" u install community/neovim
	u remove neovim >/dev/null 2>&1
	u repo add plain "http://localhost:$PORT/core" >"$LOG" 2>&1
	expect_fail "plain HTTP repositories are refused by default" "" \
	    sh -c "grep -q unsupported '$LOG'"
	kill "$HTTPD_PID"; HTTPD_PID=""
else
	echo "  skip HTTPS tests (openssl/python3 missing)"
fi

# ------------------------------------------------------------------ build
echo "== source builds"
if command -v cc >/dev/null; then
	PORTS=$T/ports/misc/hello
	mkdir -p "$PORTS" "$T/dist/hello-1.0"
	cat > "$T/dist/hello-1.0/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("hello from a upkg source build"); return 0; }
EOF
	(cd "$T/dist" && tar czf hello-1.0.tar.gz hello-1.0)
	SUM=$(sha256sum "$T/dist/hello-1.0.tar.gz" | cut -d' ' -f1)
	cat > "$PORTS/RECIPE" <<EOF
name hello
version 1.0
release 1
description greeting program built from source
license BSD-2-Clause
source file://$T/dist/hello-1.0.tar.gz
source_sha256 $SUM
EOF
	cat > "$PORTS/build" <<'EOF'
cc -O2 -o hello hello.c
mkdir -p "$DESTDIR$PREFIX/bin"
install -m 755 hello "$DESTDIR$PREFIX/bin/hello"
echo "built as $(id -un)" > "$DESTDIR$PREFIX/bin/.buildinfo" && rm "$DESTDIR$PREFIX/bin/.buildinfo"
id -u > "$SRCDIR/../../uid"
EOF
	chmod -R a+rX "$T/ports" "$T/dist"
	printf 'portsdir = %s\n' "$T/ports" >> "$ROOT/etc/upkg/upkg.conf"
	mkdir -p "$T/out" && chmod 777 "$T/out"
	expect_ok "upkg build hello (from the ports tree)" \
	    sh -c "cd '$T/out' && '$UPKG_BIN' -r '$ROOT' build hello"
	check "built package exists" test -f "$T/out/hello-1.0-1-amd64.upkg"
	if [ "$(id -u)" = 0 ]; then
		check "build script ran unprivileged" \
		    sh -c "test \"\$(cat '$T/build'/hello-1.0.*/uid 2>/dev/null || echo gone)\" != 0"
	fi
	expect_ok "install the built package" \
	    u install --allow-unsigned "$T/out/hello-1.0-1-amd64.upkg"
	check "built program runs" sh -c "'$ROOT/usr/local/bin/hello' | grep -q 'source build'"
	sed -i.bak "s/^source_sha256 .*/source_sha256 $(printf '%064d' 0)/" "$PORTS/RECIPE"
	expect_fail "source checksum mismatch aborts the build" "checksum mismatch" \
	    sh -c "cd '$T/out' && '$UPKG_BIN' -r '$ROOT' build hello"
	mv "$PORTS/RECIPE.bak" "$PORTS/RECIPE"
	echo 'exit 3' > "$PORTS/build"
	expect_fail "failing build script is reported" "failed" \
	    sh -c "cd '$T/out' && '$UPKG_BIN' -r '$ROOT' build hello"
else
	echo "  skip build tests (no cc)"
fi

# ------------------------------------------------------------------ misc
echo "== versions and CLI"
for c in "1.0 1.0 0" "1.0 1.1 -1" "1.10 1.9 1" "1.0~rc1 1.0 -1" "2.0 1.99.9 1" "1.0b 1.0a 1"; do
	set -- $c
	check "vercmp $1 $2 = $3" test "$($UPKG_BIN vercmp "$1" "$2")" = "$3"
done
check "--version" sh -c "$UPKG_BIN --version | grep -qx 'upkg 0.1'"
expect_fail "unknown command" "unknown command" u frobnicate

echo
echo "passed: $PASS  failed: $FAIL"
if [ "$FAIL" -ne 0 ]; then
	echo "failures:$FAILED"
	exit 1
fi
exit 0
