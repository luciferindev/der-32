# DER/32 development log

Entries are chronological. Every "verified" claim names the test or
command that showed it. Raw logs come from `out/logs/` of the build that
produced them.

Build host for the entries below: Ubuntu 24.04 x86_64 container, 2 CPUs,
7 GiB RAM, **no KVM** (QEMU runs under TCG). The build path is the same on
Arch Linux, the intended development host.

---

## 2026-11-19 — Milestone 0: foundation

**Upstream pinned.** `UPSTREAM` = NetBSD `netbsd-10` branch @
`06f422a9d514cd1d9476676205760cc7e7c56a21` (2026-11-17, "Ticket #1422"),
which reports itself as 10.2_STABLE. The GitHub mirror has no release
tags, so the stable branch head is pinned by commit. `src/` is a shallow
submodule. `.gitmodules` uses `ignore = all` so the gitlink stays on the
upstream commit.

**Patch series.** The DER/32 changes are commits on `src` branch `der32`,
exported to `patches/` with `scripts/patches.sh export`. They cover the
kernel identity, the DER32 kernel config, userland identity, the image,
and the upkg reachover.

**Toolchain.** `build.sh tools`: verified, exit 0 (`out/logs/tools.log`).
* Issue found: the first attempt used `MKGCC=no` to save time. NetBSD then
  skips the *cross* gcc too, and the kernel failed with `CC=false` in
  `nbmkdep`. Fixed by keeping MKGCC/MKBINUTILS on, which also makes DER/32
  self-hosting.
* Follow-up issue: the aborted `distribution` from that attempt had left
  rpcgen-generated headers produced with `CPP=false` (e.g. a 220-byte
  `rpc/rpcb_prot.h`). `-u` treated them as current, and libc later failed
  with `unknown type name 'rpcblist'`. Fixed by purging `out/obj` (except
  the toolchain and kernel) and `out/destdir`. Documented in
  `building.md`.

**Kernel.** `build.sh kernel=DER32`: verified, exit 0 (28.6 MB
`netbsd`). The generated `vers.c` contains:

```
const char ostype[] = "DER32";
const char osrelease[] = "10.2_STABLE";
const char version[] = "DER/32 0.1 GENESIS (DER32) #0: Thu Nov 19 16:36:30 MST 2026\n"
const char der32_name[] = "DER/32";
const char der32_release[] = "0.1";
const char der32_codename[] = "Genesis";
```

`strings netbsd` shows the DER/32 copyright line, the version string, and
the `kern.der32` sysctl descriptions.

## 2026-11-19 — Milestone 2: upkg

upkg 0.1 is implemented in C (13 source files, about 6,000 lines). The
code builds warning-free:

* on the host with `-Wall -Wextra -Wshadow -Wformat=2 …` (`upkg/Makefile`);
* with the **DER/32 cross compiler** (`x86_64--netbsd-gcc`, sysroot =
  DER/32 DESTDIR), using NetBSD WARNS=5-equivalent flags including
  `-Wcast-qual -Wwrite-strings -Werror`, linking libfetch. All 13 objects
  compile.

**Test suite** `upkg/tests/run.sh`: **133 passed, 0 failed**. It runs on
an isolated temp root with three local repositories and a local HTTPS
server, and covers:

* Ed25519 keygen (private key mode 0600), signing, `repo add/remove/
  enable/disable/priority`, rejection of unsupported URL schemes.
* sync: tampered index rejected and cache untouched; untrusted signer
  rejected; a validly signed index of *another* repository rejected; an
  older (replayed) index rejected; missing signature rejected.
* install: dependency resolution; dependency/explicit reasons;
  **priority** (core 1.0 beats extra 2.0); provenance; explicit
  `extra/vim` and `community/vim`; unsatisfiable constraints; unknown
  package or repository; arch mismatch excluded; declared conflicts in both
  directions; file ownership conflicts; an unowned file in the way is
  never overwritten; dry run.
* removal: required package refused; cascade `-c`; autoremove of orphans;
  empty package directories removed.
* upgrade: contents replaced, dropped files removed, new files added,
  install reason kept, "nothing to do" afterwards.
* hostile packages rejected (each crafted directly with Python tarfile):
  path traversal, absolute path, outside prefix, escaping symlink,
  absolute symlink, wrong checksum, setuid, unlisted member, wrong arch,
  invalid metadata, non-package archive. Nothing escaped the root, no
  staged leftovers, database unchanged. Also: unsigned local packages
  refused, a package not matching the signed index checksum refused, and
  a symlink planted in place of a package directory refused.
* **crash recovery**: SIGKILL injected *after staging* (rolled back),
  *after the DB commit* (rolled forward automatically by the next
  command), during removal (completed), just before commit (rolled back).
  `upkg check` is consistent after each.
* lock contention refused while another upkg holds the lock; SIGINT
  aborts cleanly.
* **HTTPS**: an untrusted certificate is rejected; with `ca_file` sync and
  install succeed; plain HTTP is refused by default.
* `upkg build`: a C program is built from a ports recipe *as `nobody`*
  (the test checks the build uid), packaged and installed; a source
  checksum mismatch and a failing build script are both reported.
* `vercmp` ordering cases.

Bugs found and fixed by the tests: `vercmp` treated `isdigit()`'s non-1
truth value as "different segment types", so every comparison returned
1. libarchive reports directories with a trailing `/`, which the manifest
lookup did not match. Source extraction was rejecting its own rewritten
absolute paths.

## 2026-11-19 — Milestones 0, 1 and 3: bootable DER/32

**Userland.** `build.sh distribution`: verified, exit 0, `checkflist`
clean. Issues fixed on the way:
* `MKKERBEROS=no` breaks BIND's libdns on this branch: its `config.h`
  hard-codes `HAVE_GSSAPI`, so `gssapi/gssapi.h` is not found. Kerberos is
  re-enabled (the upstream default) rather than patching BIND.
* The build environment restarted twice and killed the build. Recovery
  procedure: delete every `out/obj` and `out/destdir` file written in the
  last minutes before the kill (half-written outputs), then resume with
  `-u`.

**upkg in base.** `/usr/sbin/upkg` is built by the base system
(`usr.sbin/upkg` reachover, in SUBDIR and in the base/man/debug set
lists). It is a NetBSD 10.2 PIE linked against base `libarchive.so.5`,
`libsqlite3.so.1`, `libfetch.so.4`, `libssl.so.15` and `libcrypto.so.15`.

**Release sets.** `build.sh sets`, `releasekernel=DER32`, and
`etc/Makefile kernset-DER32` (DER32 added to amd64 `KERNEL_SETS` by
patch 0007) produce `kern-DER32.tar.xz`.

**Test repositories.** `make repo` cross-builds `der32-hello` 1.0 and 1.1
and **Lua 5.4.7** (upstream tag v5.4.7, commit `1ab3208a1f`, distfile
SHA-256 `19885f2b…6a1c`). `file(1)` reports each binary as
"ELF 64-bit … for NetBSD 10.2". Each repository's INDEX is signed with the
DER/32 test key.

**Images.**
```
f8f6c6df713fcec9e0d03e34fcd9f035c3c74106ca39be685f5a5b67c39e7a2d  der32-0.1-amd64.img  (2 GiB raw, MBR 55aa)
aa1f65edcc9d76915919c20bb4b5457f2bacbe22022c9fa1b8ba8b693091310a  der32-0.1-amd64.iso  (524 MiB live)
```
Fixed: `rc.d/postfix` is enabled by NetBSD defaults but Postfix is not
built (`postfix=NO` in the image). On the ISO, `rc.d/root` runs
`umount -a`, which removed early overlays, and NetBSD's MNT_UNION only
merges lookups (existing files stayed read-only). The fix is
`rc.d/der32live`, ordered after `root`, which copies /etc, /var, /root
and /usr/local into tmpfs.

**QEMU boot test** (`scripts/test.sh qemu`; TCG, IDE disk, e1000;
transcript `out/logs/qemu-test.log`): **38 passed, 0 failed.** It boots to
getty in about 20 s and logs in over the serial console:

```
DER/32 UNIX Operating System
Release 0.1 — Genesis

der32/amd64 (tty00)

login: root
Password:
DER/32 0.1 GENESIS (DER32) #1: Thu Nov 19 17:37:09 MST 2026
...
# uname -s
DER32
# upkg --version
upkg 0.1
```

Verified on the running system: `uname -s/-m/-v`, `kern.ostype`,
`kern.der32.release`=0.1, `kern.der32.codename`=Genesis,
`kern.osrelease`=10.2_STABLE, `/etc/release`, hostname; BSD init as
pid 1; FFS root; POSIX sh; filesystem operations; the standard hierarchy;
native `cc` (gcc 10.5); DHCP on wm0 and a ping to the gateway. upkg:
sync with signature verification; search; install of `lua-hello` pulling
`lua` as a dependency; packaged Lua 5.4.7 runs, and the base system's own
`/usr/bin/lua` 5.4.6 is untouched (base/third-party separation);
priority (core 1.0) and explicit `community/der32-hello` upgrade to 1.1;
removal of a required package refused; `upkg check`; a **native source
build inside DER/32 as `nobody`**; remove + autoremove; clean
`shutdown -p`.

**Live ISO** (`qemu-test.py …iso`): **25 passed, 0 failed.** It covers
the cd9660 root, tmpfs layers, identity, network and an upkg install.

**Artifact checks** (`scripts/test.sh build`): 13/13.
**upkg suite**: 133/133.

## 2026-11-19 — derfetch in base

`/usr/bin/derfetch` (C, about 400 lines, `derfetch/`, reachover
`src/usr.bin/derfetch`, patch 0008, in the base/man/debug sets) prints
Genesis the tortoise beside information from the running kernel. Built
with WARNS=5 `-Werror` by the DER/32 toolchain; it links only
libsqlite3 and libc. Issue fixed: NetBSD's MAXHOSTNAMELEN (256) tripped
`-Wformat-truncation` on the title buffer.

Verified in QEMU (`derfetch -n` on the booted image):
```
OS: DER/32 0.1 Genesis amd64
Base: NetBSD 10.2_STABLE
Kernel: DER32 #1
Packages: 0 (upkg)
```
The package count follows upkg after installs. Disk image test 42/42,
live ISO 28/28, artifact checks 14/14.

## NetBSD binary packages, quips and eggs (patch 0009)

Goal: download real software. NetBSD's own binary packages fit, because
DER/32 keeps the NetBSD 10 ABI. Findings from the source before writing
anything:

* `pkg_add` compares a package's `OPSYS` with the compile-time
  `OPSYS_NAME` ("NetBSD", `pkg_install/dist/lib/lib.h`), not with
  `uname -s`, so `DER32` does not get in the way. A release mismatch
  (`10.2` vs `10.2_STABLE`) only warns.
* pkgsrc records `PKGTOOLS_VERSION=20091115` in packages; base
  pkg_install is 20210410, so the base `pkg_add` is new enough.
* pkgin's `$osrelease` strips `_STABLE`, and its NetBSD build ships a
  `repositories.conf` already naming cdn.NetBSD.org's 10.2 tree.
* `/etc/openssl/certs` was **empty** in the image and `certctl_init`
  defaults to NO, so HTTPS could not have verified anything.

Changes: `/etc/pkg_install.conf` with `PKG_PATH` for x86_64/10.2,
`certctl_init=YES` in the image, a tmpfs `/usr/pkg` on the live ISO
(the first ISO run failed with "Read-only file system"), derfetch counts
pkgsrc packages, and `rc.d/motd` appends a random line from
`/usr/share/misc/der32-quips`. Easter eggs: `upkg tortoise`, `upkg moo`,
and a hatch-day line in derfetch on 19 November.

The boot tests build a pkgsrc-format package inside the guest with
`pkg_create`: `pkg_add` refuses it with `OPSYS=Linux` ("was built for a
platform: Linux/x86_64 10.2 (pkg) vs. NetBSD/x86_64 10.2_STABLE (this
host)") and installs and runs it with `OPSYS=NetBSD`. Downloading from
cdn.NetBSD.org itself cannot be tested from the build sandbox (no route
to that host).

Disk image test 50/50, live ISO 36/36, upkg suite 136/136. Patch series
re-applied to pristine upstream gives a tree-identical result.

## Installer, UEFI, X11, online repositories (patches 0010-0014)

Goal: install DER/32 like a real OS, from USB or ISO, on BIOS and UEFI
machines, with XFCE as an option. PR #1 (screen-console image, Linux-side
image writer) was merged first.

**Build.** `xsrc/` is a second pinned submodule (NetBSD X11, `netbsd-10`
@ 4d8a282), and `MKX11=yes` with `MKLLVMRT=no` builds the X11 sets
without LLVM in Mesa. `build.sh` passes `-x -X xsrc` and fetches it
shallow on first use.

**Images (patch 0013).** `der32image` makes GPT + hybrid MBR images with
an EFI system partition (`bootx64.efi`, `bootia32.efi`) and writes
`/boot.cfg` with the console, because the UEFI loader reads nothing else.
There is a third console mode, `any`: the BIOS loader's `console=auto`
picks com0 if a terminal answers and the screen otherwise, getty runs on
`constty`, and the boot menu can force either. `DER32_SETSDIR` adds the
release sets in `/der32/sets` to make the USB installer image. The ISO
reuses that tree and gets a second, EFI El Torito entry (`efiboot.img`).
On the disk images root's password is expired (`pw_change` = 1), and
`pam_unix` returns `PAM_NEW_AUTHTOK_REQD`, so login asks for a new one
straight away.

**Installer (patch 0012, `installer/der32-install.sh`).** It writes a GPT
(`der32efi`, `der32swap`, `der32root`), runs `gpt biosboot` with
`gptmbr.bin` and `installboot bootxx_ffsv2` for BIOS, and puts
`EFI/BOOT/BOOTX64.EFI` on the ESP. It extracts the sets with `xz | tar`,
runs MAKEDEV, writes `fstab` by `NAME=` label, `rc.conf`, `boot.cfg` and
`localtime`, sets passwords with `pwhash` + `usermod -p`, and runs
`certctl rehash`. For XFCE it adds the X11 sets, then `pkg_add pkgin` and
`pkgin install xfce4` in a chroot, with a graceful fallback. `-f answers`
makes it unattended.

Bugs found by testing it end to end:

* The ISO's root is read-only, so `/targetroot` can't be created. It now
  falls back to `/mnt`.
* `postfix=YES` from `etc/defaults/rc.conf`, with no Postfix in the build,
  stalled boot. The installer writes `postfix=NO`.
* A stale `pwwork/` gave the installer image the disk images' expired
  password. `build.sh` now clears it per image.
* `ttyE1`/`ttyE2` gettys on a serial-console boot spammed "Device not
  configured". Only `constty` is on now.
* `rc.d/sshd` rewrites the end of `/etc/motd`, which left an indented
  duplicate quip. `rc.d/motd` now deletes quips anywhere (patch 0014).
* The test harness can't type a script into the live shell (TAB
  completion eats tabs), so it uploads base64 and runs `uudecode -m`.

**SSH (open).** Enabling `sshd` hung the boot. `ssh-keygen -y` on any
private key spins at 100% CPU right after `fstat()` on the key file, with
no further syscalls (ktrace). The same happens on the live ISO, under
`qemu64` and `-cpu max`, with or without a virtio RNG and with the host
name in `/etc/hosts`. Key generation works. Root cause unknown, so the
installer doesn't offer SSH for now (roadmap, first item).

**NetBSD bug seen.** `ld(4)` on virtio panicked during the shutdown cache
flush (`ld_virtio_vq_done1`). The installer test uses IDE disks.

**Repositories.** The images trust a release key
(`conf/upkg/keys/der32-release.pub`). `scripts/publish-repo.sh` re-signs
`make repo` output with it into `site/repo/`, which GitHub Pages serves,
and `repos.conf` lists `der32-{core,extra,community}` over HTTPS ahead of
the copies on the medium. New ports: `tree` 2.1.3 and `nnn` 5.1,
cross-built with the base curses and libedit. Their distfiles come from a
`#@git` recipe line as reproducible `git archive` tarballs (checksums
verified), and `mkrepo.sh` fetches the lua distfile the same way, so a
fresh clone no longer stops at `make repo`.

**CI.** `ci.yml` runs the upkg suite on every push. `release.yml` builds
everything on a runner, runs the tests and the installer test, and
publishes a GitHub Release with the images. It hasn't run yet.

## Outstanding

See `roadmap.md`, "Known limitations". The main items are pkgsrc
`OPSYS` mapping for source builds, a VGA-console image, an installer, replacing the
default root password, release signing keys and reproducible timestamps.
