#!/usr/bin/env python3
"""
DER/32 installer test.

1. Boots the live + install ISO in QEMU with an empty 8 GiB virtio disk,
   logs in, and runs der32-install unattended (answer file written on the
   guest) with a user, a time zone, swap and, by default, the XFCE option.
2. Boots the installed disk with legacy BIOS and checks the result.
3. Boots it again with UEFI firmware (OVMF), if available.

Every check reads output from the running DER/32 guest.  Transcripts go to
out/logs/install-test-*.log.

usage: install-test.py ISO [WORKDIR]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemulib import Session, OVMF_CODE  # noqa: E402

ISO = sys.argv[1]
WORK = sys.argv[2] if len(sys.argv) > 2 else "out/install-test"
LOGDIR = os.environ.get("LOGDIR", "out/logs")
XFCE = os.environ.get("DER32_TEST_XFCE", "yes")
# Development shortcut: run this installer script instead of the one on
# the medium (typed into the guest over the serial line).
OVERRIDE = os.environ.get("DER32_INSTALLER")
ROOTPW = "tortoise-root"
USERPW = "tortoise-user"
os.makedirs(WORK, exist_ok=True)
os.makedirs(LOGDIR, exist_ok=True)
DISK = os.path.join(WORK, "target.img")

results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  [{detail}]"))
    sys.stdout.flush()


def check_cmd(s, name, cmd, want=None, rx=None, status=0, timeout=300):
    import re
    st, out = s.run(cmd, timeout)
    ok = st == status
    if ok and want is not None:
        ok = out.strip() == want
    if ok and rx is not None:
        ok = re.search(rx, out, re.M) is not None
    check(name, ok, f"status {st}, output {out[-400:]!r}")
    return out


subprocess.run(["rm", "-f", DISK], check=True)
subprocess.run(["truncate", "-s", "8G", DISK], check=True)
# IDE rather than virtio: NetBSD 10's ld(4) on virtio panics in its cache
# flush at shutdown under some QEMU versions (ld_virtio_vq_done1).
target = ["-drive", f"file={DISK},format=raw,if=ide"]

# ------------------------------------------------------------- 1. install
print(f"== install from {ISO}")
s = Session([["-cdrom", ISO, "-boot", "d"], target],
            os.path.join(LOGDIR, "install-test-1-install.log"), workdir=WORK)
try:
    s.login("root", "der32")
    check("live ISO boots to a root shell", True)
    installer = "der32-install"
    if OVERRIDE:
        import base64
        raw = open(OVERRIDE, "rb").read()
        body = raw.decode()
        # base64 in uuencode -m framing: no tabs for the shell's line editor
        b64 = base64.encodebytes(raw).decode()
        s.send("cat > /tmp/inst.b64 <<'DER32EOF'\nbegin-base64 755 /tmp/der32-install\n"
               + b64 + "====\nDER32EOF\n")
        s.run("uudecode -m /tmp/inst.b64 2>&1 || uudecode /tmp/inst.b64")
        installer = "sh /tmp/der32-install"
        check_cmd(s, "override installer uploaded intact",
                  "wc -c < /tmp/der32-install", want=str(len(body.encode())))
    st, disks = s.run("sysctl -n hw.disknames")
    tdisk = next((d for d in disks.split() if d[:2] in ("wd", "ld", "sd")), None)
    check(f"target disk visible ({disks.strip()})", tdisk is not None)
    s.run(f"rh=$(pwhash '{ROOTPW}'); uh=$(pwhash '{USERPW}')")
    answers = [
        f"DISK={tdisk}", f"CONFIRM='erase {tdisk}'", "HOSTNAME=tortoise",
        "TIMEZONE=America/Boise", "CONSOLE=serial", "SWAP_MB=512",
        "ROOT_PWHASH='$rh'", "USERNAME=alice", "USER_PWHASH='$uh'",
        "DHCP=yes", f"XFCE={XFCE}",
    ]
    s.run("rm -f /tmp/ans")
    for a in answers:
        # ROOT_PWHASH='$rh' must be written with the hash value, quoted
        if "'$rh'" in a or "'$uh'" in a:
            var, ref = a.split("=", 1)
            ref = ref.strip("'")
            s.run(f"printf \"%s='%s'\\n\" {var} \"{ref}\" >> /tmp/ans")
        else:
            s.run(f"echo \"{a}\" >> /tmp/ans")
    check_cmd(s, "answer file has a root password hash", "grep -c '^ROOT_PWHASH=.\\$' /tmp/ans",
              want="1")
    check_cmd(s, "der32-install runs unattended to completion",
              f"{installer} -f /tmp/ans 2>&1 | tail -15",
              rx=r"DER/32 is installed on " + (tdisk or "wd"), timeout=3600)
    check_cmd(s, "partitions are labelled der32efi/der32swap/der32root",
              f"gpt show -l {tdisk} | grep -o 'der32[a-z]*' | tr '\\n' ' '",
              rx=r"der32efi der32swap der32root")
    check_cmd(s, "refuses to run without an answer for CONFIRM",
              f"grep -v CONFIRM /tmp/ans > /tmp/ans2; {installer} -f /tmp/ans2 2>&1 | tail -1",
              rx=r"CONFIRM")
    check("installer medium powers off cleanly", s.poweroff())
finally:
    s.close()


def boot_installed(tag, uefi):
    print(f"== boot installed disk ({tag})")
    s = Session([["-drive", f"file={DISK},format=raw,if=ide"]],
                os.path.join(LOGDIR, f"install-test-2-{tag}.log"),
                uefi=uefi, workdir=WORK)
    try:
        m = s.login("root", ROOTPW)
        check(f"[{tag}] boots to getty: {m.group(1)} ({m.group(2)})",
              m.group(1) == "tortoise")
        s.expect(r"DER/32 0\.1 GENESIS", 60)
        check(f"[{tag}] root logs in with the password set by the installer", True)
        check_cmd(s, f"[{tag}] uname -s = DER32", "uname -s", want="DER32")
        check_cmd(s, f"[{tag}] root is FFSv2 on the der32root wedge",
                  "mount | head -1", rx=r"^/dev/dk\d+ on / type ffs \(.*log")
        check_cmd(s, f"[{tag}] fstab mounts by GPT label",
                  "grep '^NAME=der32root' /etc/fstab | awk '{print $2}'", want="/")
        check_cmd(s, f"[{tag}] swap is in use", "swapctl -l | tail -1",
                  rx=r"^/dev/dk\d+\s+\d+")
        check_cmd(s, f"[{tag}] time zone", "readlink /etc/localtime",
                  want="/usr/share/zoneinfo/America/Boise")
        check_cmd(s, f"[{tag}] user alice in wheel", "id -Gn alice",
                  rx=r"\bwheel\b")
        check_cmd(s, f"[{tag}] alice has a home and a password hash",
                  "test -d /home/alice && grep '^alice:' /etc/master.passwd | cut -d: -f2 | cut -c1",
                  want="$")
        check_cmd(s, f"[{tag}] sshd left off (known 0.1 bug)", "grep -c '^sshd=YES' /etc/rc.conf",
                  want="0", status=1)
        check_cmd(s, f"[{tag}] CA certificates in place",
                  "ls /etc/openssl/certs | wc -l", rx=r"^\s*[1-9]\d+$")
        check_cmd(s, f"[{tag}] upkg works on the installed system",
                  "upkg sync >/dev/null 2>&1; upkg search lua | head -1", rx=r"lua")
        check_cmd(s, f"[{tag}] derfetch", "derfetch -n -l | head -3",
                  rx=r"OS: DER/32 0\.1 Genesis")
        check_cmd(s, f"[{tag}] install log kept", "test -s /var/log/der32-install.log && echo ok",
                  want="ok")
        if XFCE == "yes":
            check_cmd(s, f"[{tag}] X11 sets installed (XFCE option)",
                      "test -x /usr/X11R7/bin/Xorg && echo ok", want="ok")
        if uefi:
            check_cmd(s, f"[{tag}] booted through UEFI",
                      "sysctl -n machdep.bootmethod", want="UEFI")
        else:
            check_cmd(s, f"[{tag}] booted through BIOS",
                      "sysctl -n machdep.bootmethod", want="BIOS")
        check(f"[{tag}] powers off cleanly", s.poweroff())
    except Exception as e:
        check(f"[{tag}] session", False, str(e))
    finally:
        s.close()


boot_installed("bios", False)
if OVMF_CODE:
    boot_installed("uefi", True)
else:
    print("== UEFI boot skipped: no OVMF firmware on this host")

failed = [n for n, ok in results if not ok]
print(f"\ninstall test: {len(results) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
