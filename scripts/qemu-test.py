#!/usr/bin/env python3
"""
DER/32 automated boot and system test.

Boots a disposable copy of the release image in QEMU (all disk writes are
discarded: -snapshot), logs in on the serial console exactly as a user
would, runs checks, and powers the machine off.  The complete serial
transcript is written to out/logs/qemu-test.log as evidence.

Every check compares output produced by the running DER/32 system; nothing
is inferred from the build tree.

usage: qemu-test.py IMAGE [LOGFILE]
"""
import os
import re
import subprocess
import sys
import threading
import time

IMG = sys.argv[1]
LOG = sys.argv[2] if len(sys.argv) > 2 else "qemu-test.log"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))
CMD_TIMEOUT = int(os.environ.get("CMD_TIMEOUT", "300"))
PASSWORD = os.environ.get("DER32_ROOT_PASSWORD", "der32")

ISO = IMG.endswith(".iso")
accel = ["-accel", "kvm", "-cpu", "host"] if os.access("/dev/kvm", os.W_OK) \
    else ["-accel", "tcg"]
media = ["-cdrom", IMG, "-boot", "d"] if ISO else \
    ["-drive", f"file={IMG},format=raw,if=ide,snapshot=on"]
qemu = ["qemu-system-x86_64", *accel, "-m", "1024", "-smp", "2", *media,
        "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
        "-display", "none", "-serial", "stdio", "-monitor", "none",
        "-no-reboot"]

logf = open(LOG, "wb")
buf = bytearray()
lock = threading.Lock()
proc = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)


def reader():
    while True:
        b = proc.stdout.read1(4096)
        if not b:
            break
        with lock:
            buf.extend(b)
        logf.write(b)
        logf.flush()


threading.Thread(target=reader, daemon=True).start()
pos = 0


def text():
    with lock:
        return buf.decode("utf-8", "replace")


def expect(pattern, timeout):
    """Wait for regex after the current position; advance past it."""
    global pos
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        t = text()
        m = rx.search(t, pos)
        if m:
            pos = m.end()
            return m
        if proc.poll() is not None:
            break
        time.sleep(0.2)
    raise TimeoutError(f"timed out waiting for {pattern!r}")


def send(s):
    for ch in s:          # the serial line is 9600 baud: pace input
        proc.stdin.write(ch.encode())
        proc.stdin.flush()
        time.sleep(0.003)


seq = 0


def run(cmd, timeout=CMD_TIMEOUT):
    """Run a shell command on DER/32; return (status, output)."""
    global seq
    seq += 1
    # the markers are split by quotes so the echoed command line never
    # matches them -- only the shell's output does
    send(f"echo @@B''EGIN{seq}@@; {cmd}; echo @@E''ND{seq}@@ $?\n")
    expect(rf"@@BEGIN{seq}@@\r*\n", timeout)
    start = pos
    m = expect(rf"@@END{seq}@@ (\d+)", timeout)
    out = text()[start:m.start()].replace("\r", "").strip("\n")
    return int(m.group(1)), out


results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"  [{detail}]" if detail and not ok else ""))
    sys.stdout.flush()


def check_cmd(name, cmd, want=None, rx=None, status=0, timeout=CMD_TIMEOUT):
    st, out = run(cmd, timeout)
    ok = st == status
    if ok and want is not None:
        ok = out.strip() == want
    if ok and rx is not None:
        ok = re.search(rx, out, re.M) is not None
    check(name, ok, f"status {st}, output {out[-300:]!r}")
    return out


try:
    print(f"booting {IMG} ({'KVM' if 'kvm' in accel else 'TCG'}), transcript: {LOG}")
    t0 = time.time()
    expect(r"DER/32 0\.1 GENESIS \(DER32\)", 300)
    check("kernel identifies as DER/32 0.1 GENESIS at boot", True)
    m = expect(r"DER/32 UNIX Operating System\r*\nRelease 0\.1 \S+ Genesis\r*\n\r*\n"
               r"(\S+)/(\S+) \((\S+)\)\r*\n\r*\nlogin: ", BOOT_TIMEOUT)
    check(f"multi-user boot to getty login in {int(time.time() - t0)}s "
          f"({m.group(1)}/{m.group(2)} ({m.group(3)}))", True)
    check("getty banner: der32/amd64 (tty00)",
          (m.group(1), m.group(2), m.group(3)) == ("der32", "amd64", "tty00"))
    send("root\n")
    expect(r"Password:", 60)
    send(PASSWORD + "\n")
    expect(r"DER/32 0\.1 GENESIS", 60)
    check("login as root; motd shows DER/32 0.1 GENESIS", True)
    time.sleep(2)
    send("PS1='# '; export PS1\n")
    time.sleep(1)

    print("== identity")
    check_cmd("uname -s = DER32", "uname -s", want="DER32")
    check_cmd("uname -m = amd64", "uname -m", want="amd64")
    check_cmd("uname -v starts with DER/32 0.1 GENESIS", "uname -v",
              rx=r"^DER/32 0\.1 GENESIS \(DER32\)")
    check_cmd("sysctl kern.ostype", "sysctl -n kern.ostype", want="DER32")
    check_cmd("sysctl kern.der32.release", "sysctl -n kern.der32.release", want="0.1")
    check_cmd("sysctl kern.der32.codename", "sysctl -n kern.der32.codename",
              want="Genesis")
    check_cmd("kern.osrelease keeps the NetBSD ABI release",
              "sysctl -n kern.osrelease", rx=r"^10\.\d+")
    check_cmd("/etc/release names DER/32", "head -2 /etc/release",
              rx=r"^DER/32 0\.1 Genesis/amd64\nBased on NetBSD 10")
    check_cmd("hostname is der32", "hostname", want="der32")

    print("== base system")
    check_cmd("POSIX sh: arithmetic, parameter expansion",
              'x=der32; echo $((6*7)) ${x#der} $(echo ok)', want="42 32 ok")
    check_cmd("init is the traditional BSD init (pid 1)",
              "ps -o comm= -p 1", want="init")
    if ISO:
        check_cmd("root filesystem is the read-only CD (cd9660)",
                  "mount | head -1", rx=r" on / type cd9660 \(.*read-only")
        check_cmd("writable tmpfs layers", "mount | grep -c 'tmpfs on /'",
                  rx=r"^[5-9]$")
    else:
        check_cmd("root filesystem is FFS", "mount | head -1", rx=r" on / type ffs")
    check_cmd("filesystem operations (mkdir/write/mv/ln/rm)",
              "(set -e; d=/tmp/t.$$; mkdir -p $d/a/b; echo hi > $d/a/f; "
              "mv $d/a/f $d/a/b/g; ln -s b/g $d/a/l; cat $d/a/l; "
              "test -L $d/a/l; rm -r $d; test ! -e $d && echo gone)",
              want="hi\ngone")
    check_cmd("standard hierarchy present",
              "for d in /bin /sbin /etc /usr/bin /usr/sbin /usr/lib /var/log "
              "/usr/local /var/lib/upkg; do test -d $d || echo missing $d; done; echo done",
              want="done")
    check_cmd("C compiler present (self-hosting)", "cc --version | head -1",
              rx=r"^cc \(nb\d+ \d+\) \d+\.\d+")
    check_cmd("network interface configured by dhcpcd",
              "sleep 5; ifconfig wm0 | grep 'inet '", rx=r"inet 10\.0\.2\.\d+")
    check_cmd("network: reach the QEMU gateway",
              "ping -c 2 -w 10 10.0.2.2 >/dev/null && echo up", want="up",
              timeout=60)

    print("== derfetch")
    out = check_cmd("derfetch shows the DER/32 release from kern.der32",
                    "derfetch -n", rx=r"OS: DER/32 0\.1 Genesis amd64")
    check("derfetch shows the DER32 kernel and NetBSD base",
          re.search(r"Kernel: DER32 #\d+", out) is not None and
          re.search(r"Base: NetBSD 10\.", out) is not None, out[-200:])
    check("derfetch draws Genesis", "'-.__.-'" in out, out[:200])

    print("== easter eggs")
    check_cmd("motd carries a tortoise quip, refreshed at boot",
              "grep -c '^Tortoise says: ' /etc/motd", want="1")
    check_cmd("upkg tortoise", "upkg tortoise | grep -c '_|___|_'", want="1")

    print("== NetBSD binary packages (pkg_add)")
    check_cmd("pkg_add is pointed at cdn.NetBSD.org x86_64/10.2",
              "pkg_admin config-var PKG_PATH",
              want="https://cdn.NetBSD.org/pub/pkgsrc/packages/NetBSD/x86_64/10.2/All")
    check_cmd("CA certificates installed for HTTPS (certctl_init)",
              "ls /etc/openssl/certs | wc -l", rx=r"^\s*[1-9]\d+$")
    # Build a pkgsrc-format package in the guest and install it with the
    # base pkg_add: proves the OPSYS check accepts NetBSD packages although
    # uname -s says DER32, and rejects other platforms.
    mkpkg = ("rm -rf /tmp/pk && mkdir -p /tmp/pk/bin && cd /tmp/pk && "
             "printf '#!/bin/sh\\necho pkgsrc ok\\n' > bin/der32-pkgtest && "
             "chmod 755 bin/der32-pkgtest && echo bin/der32-pkgtest > plist && "
             "echo test > c && echo test > d && "
             "printf 'OPSYS=%s\\nOS_VERSION=10.2\\nMACHINE_ARCH=x86_64\\n"
             "PKGTOOLS_VERSION=20091115\\n' > bi && "
             "pkg_create -B bi -c c -d d -f plist -I /usr/pkg -p /tmp/pk "
             "der32-pkgtest-1.0.tgz && ")
    check_cmd("pkg_add refuses a package built for another OS",
              mkpkg % "Linux" + "pkg_add ./der32-pkgtest-1.0.tgz 2>&1",
              status=1, rx=r"built for a platform")
    check_cmd("pkg_add installs a NetBSD package on DER/32 into /usr/pkg",
              mkpkg % "NetBSD" + "pkg_add ./der32-pkgtest-1.0.tgz && "
              "/usr/pkg/bin/der32-pkgtest", want="pkgsrc ok")
    check_cmd("derfetch counts pkgsrc packages",
              "derfetch -n -l | grep Packages", rx=r"\(upkg\), 1 \(pkgsrc\)")
    check_cmd("pkg_delete removes it", "pkg_delete der32-pkgtest && "
              "test ! -e /usr/pkg/bin/der32-pkgtest && echo gone", want="gone")

    print("== upkg")
    check_cmd("upkg --version", "upkg --version", want="upkg 0.1")
    if ISO:
        check_cmd("live: upkg sync + install der32-hello",
                  "upkg sync >/dev/null && upkg install der32-hello >/dev/null && der32-hello",
                  rx=r"kern\.der32: release 0\.1")
        raise StopIteration
    check_cmd("upkg repo list (3 test repositories)", "upkg repo list | wc -l",
              want="3")
    check_cmd("upkg sync verifies signed indexes", "upkg sync",
              rx=r"core .*packages")
    check_cmd("upkg search", "upkg search lua", rx=r"core/lua 5\.4\.7-1")
    check_cmd("upkg install lua-hello (pulls lua as dependency)",
              "upkg install lua-hello", rx=r"installing lua 5\.4\.7-1")
    check_cmd("native packaged Lua 5.4.7 runs on DER/32",
              "/usr/local/bin/lua -v", rx=r"^Lua 5\.4\.7")
    check_cmd("base system's own /usr/bin/lua is untouched by the package",
              "/usr/bin/lua -v", rx=r"^Lua 5\.4\.6")
    check_cmd("package from 'extra' runs", "lua-hello",
              want="Hello from Lua 5.4 on DER32")
    check_cmd("repository priority: der32-hello comes from core (1.0)",
              "upkg install der32-hello >/dev/null && der32-hello",
              rx=r"^der32-hello 1\.0: DER32 10\.\d\S* amd64\nkern\.der32: release 0\.1 \"Genesis\"")
    check_cmd("refuses to remove a required package", "upkg remove lua",
              status=1)
    check_cmd("explicit selection community/der32-hello upgrades to 1.1",
              "upkg install community/der32-hello >/dev/null && der32-hello",
              rx=r"^der32-hello 1\.1:")
    check_cmd("derfetch counts upkg packages", "derfetch -n -l | grep Packages",
              rx=r"Packages: [1-9]\d* \(upkg\)")
    check_cmd("upkg check verifies installed files", "upkg check",
              rx=r"all installed files verified")
    check_cmd("upkg list", "upkg list", rx=r"lua\s+5\.4\.7-1\s+\(dependency\)")
    check_cmd("native source build as unprivileged user: upkg build der32-hello",
              "cd /tmp && upkg build der32-hello 2>&1 | tail -1",
              rx=r"built der32-hello-1\.0-1-amd64\.upkg", timeout=600)
    check_cmd("upkg remove + autoremove", "upkg remove lua-hello der32-hello >/dev/null "
              "&& upkg autoremove >/dev/null && upkg list | wc -l", want="0")
    check_cmd("removed files are gone", "test ! -e /usr/local/bin/lua && echo clean",
              want="clean")

    send("shutdown -p now\n")
    try:
        proc.wait(timeout=180)
        check("clean shutdown (QEMU exited)", True)
    except subprocess.TimeoutExpired:
        check("clean shutdown (QEMU exited)", False)
except StopIteration:
    send("shutdown -p now\n")
    try:
        proc.wait(timeout=180)
        check("clean shutdown (QEMU exited)", True)
    except subprocess.TimeoutExpired:
        check("clean shutdown (QEMU exited)", False)
except Exception as e:  # noqa: BLE001
    check(f"unexpected: {e}", False)
finally:
    if proc.poll() is None:
        proc.kill()
    logf.close()

failed = [n for n, ok in results if not ok]
print(f"\nQEMU test: {len(results) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
