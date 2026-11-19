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

## In progress

* `build.sh distribution` (userland) is running.
* Next: hook `usr.sbin/upkg` into SUBDIR and the set lists, `build.sh
  sets` + `releasekernel=DER32`, `make repo`, image, QEMU boot test, ISO.
