"""Small QEMU serial-console driver shared by the DER/32 test scripts.

A Session runs qemu-system-x86_64 with the guest's serial port on a pipe,
keeps the whole transcript (also written to a log file), and offers
expect()/send()/run() in the style of expect(1).
"""
import os
import re
import shutil
import subprocess
import threading
import time

OVMF_CODE = next((p for p in ("/usr/share/OVMF/OVMF_CODE_4M.fd",
                              "/usr/share/OVMF/OVMF_CODE.fd",
                              "/usr/share/edk2/x64/OVMF_CODE.4m.fd",
                              "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd")
                  if os.path.exists(p)), None)
OVMF_VARS = OVMF_CODE and next((p for p in (
    OVMF_CODE.replace("CODE", "VARS"),
    "/usr/share/OVMF/OVMF_VARS_4M.fd",
    "/usr/share/edk2/x64/OVMF_VARS.4m.fd") if os.path.exists(p)), None)


def accel():
    if os.access("/dev/kvm", os.W_OK):
        return ["-accel", "kvm", "-cpu", "host"]
    return ["-accel", "tcg"]


class Session:
    def __init__(self, drives, log, uefi=False, mem=1024, workdir="."):
        """drives: list of -drive/-cdrom argument lists."""
        args = ["qemu-system-x86_64", *accel(), "-m", str(mem), "-smp", "2"]
        if uefi:
            if not OVMF_CODE or not OVMF_VARS:
                raise RuntimeError("UEFI test needs OVMF firmware")
            vars_copy = os.path.join(workdir, "ovmf-vars.fd")
            shutil.copy(OVMF_VARS, vars_copy)
            args += ["-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
                     "-drive", f"if=pflash,format=raw,file={vars_copy}"]
        for d in drives:
            args += d
        args += ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
                 "-display", "none", "-serial", "stdio", "-monitor", "none",
                 "-no-reboot"]
        self.cmdline = args
        self.logf = open(log, "wb")
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.pos = 0
        self.seq = 0
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            b = self.proc.stdout.read1(4096)
            if not b:
                break
            with self.lock:
                self.buf.extend(b)
            self.logf.write(b)
            self.logf.flush()

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")

    def expect(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            m = rx.search(self.text(), self.pos)
            if m:
                self.pos = m.end()
                return m
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        raise TimeoutError(f"timed out waiting for {pattern!r}")

    def send(self, s):
        for ch in s:          # 9600 baud: pace the input
            self.proc.stdin.write(ch.encode())
            self.proc.stdin.flush()
            time.sleep(0.003)

    def run(self, cmd, timeout=300):
        """Run a shell command in the guest; return (status, output)."""
        self.seq += 1
        n = self.seq
        self.send(f"echo @@B''EGIN{n}@@; {cmd}; echo @@E''ND{n}@@ $?\n")
        # the serial line occasionally drops a newline: press Return again
        try:
            self.expect(rf"@@BEGIN{n}@@\r*\n", 20)
        except TimeoutError:
            self.send("\n")
            self.expect(rf"@@BEGIN{n}@@\r*\n", timeout)
        start = self.pos
        m = self.expect(rf"@@END{n}@@ (\d+)", timeout)
        out = self.text()[start:m.start()].replace("\r", "").strip("\n")
        return int(m.group(1)), out

    def login(self, user, password, timeout=900):
        """Wait for the getty, log in; returns the getty banner match."""
        m = self.expect(r"(\S+)/amd64 \((\S+)\)\r*\n\r*\nlogin: ", timeout)
        self.send(user + "\n")
        self.expect(r"Password:", 60)
        self.send(password + "\n")
        time.sleep(3)
        self.send("PS1='# '; export PS1\n")
        time.sleep(1)
        return m

    def poweroff(self, timeout=180):
        self.send("shutdown -p now\n")
        end = time.time() + timeout
        while time.time() < end and self.proc.poll() is None:
            time.sleep(1)
        if self.proc.poll() is None:
            self.proc.kill()
            return False
        return True

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.logf.close()
