# DER/32 architecture

DER/32 ("Derivative 32") is a NetBSD-derived Unix operating system. It keeps
NetBSD's proven kernel, libc, userland and build system, and adds its own
system identity, release engineering, kernel configuration and native
package manager, **upkg**.

## Source layout

```
DER32/
├── UPSTREAM          pinned NetBSD source revision (url, branch, commit)
├── patches/          the complete DER/32 delta to NetBSD, as a patch series
├── src/              git submodule: pristine NetBSD at UPSTREAM_COMMIT,
│                     with patches/ applied on the local branch "der32"
├── upkg/             upkg sources (C), tests, man page
├── ports/            source recipes for "upkg build"
├── repos/            documentation of the official repositories
├── conf/             build configuration (mk.conf) and default /etc/upkg
├── scripts/          build, image, repository, QEMU and test drivers
├── docs/             this documentation and the development log
└── out/              build output (never committed)
```

### Why a submodule plus a patch series

The NetBSD tree is about 3 GB. Copying it into the DER/32 repository would
hide DER/32's changes among hundreds of thousands of upstream files. Instead:

* `src/` is a submodule pinned to an exact upstream commit (`UPSTREAM`). The
  complete upstream tree is present and buildable, and anyone can confirm
  it is unmodified NetBSD.
* Every DER/32 change to that tree is one commit in `patches/`, written with
  `git format-patch`. `scripts/patches.sh apply` recreates the `der32`
  branch from the pinned commit and these patches. `export` regenerates the
  patches after you edit `src/` and commit on `der32`.
* To move to a newer upstream, update `UPSTREAM` and re-apply the series
  (see `docs/building.md`, "Upstream synchronisation").

## System identity

The identity is implemented in the build definitions and kernel
interfaces, not by editing display strings.

| Interface | Value | Mechanism |
|---|---|---|
| `kern.ostype` / `uname -s` | `DER32` | `sys/conf/newvers.sh` reads `sys/conf/der32.release` |
| `kern.version` / `uname -v` | `DER/32 0.1 GENESIS (DER32) #n: …` | same |
| `kern.der32.name/release/codename` | `DER/32`, `0.1`, `Genesis` | new `sys/kern/kern_der32.c` (sysctl tree) |
| `kern.osrelease` / `uname -r` | `10.2_STABLE` | **unchanged on purpose**, see below |
| `/etc/release` | `DER/32 0.1 Genesis/amd64`, `Based on NetBSD …` | `etc/Makefile` includes `der32.release` |
| boot banner, motd, getty | DER/32 | `etc/` templates, `etc/rc.d/motd` |

`sys/conf/der32.release` is the single source of truth. It is valid both
as POSIX sh and as BSD make, so the kernel scripts and the userland
makefiles read the same values.

**Why `kern.osrelease` stays NetBSD's.** The kernel module loader builds
its search path from it (`/stand/amd64/<osrelease>/modules`). Binary
compatibility checks and the ELF `.note.netbsd.ident` section are tied to
`__NetBSD_Version__` as well. Changing them would break existing NetBSD
APIs and binaries, which the project rules forbid. DER/32's own release
number is published separately through `kern.der32.release`.

**Known consequence.** Software that branches on `uname -s` = `NetBSD`
(notably pkgsrc's bootstrap) will not recognise DER32 until taught to. See
`roadmap.md`.

## Kernel

`sys/arch/amd64/conf/DER32` includes `GENERIC` and sets `ident "DER32"`. It
keeps every GENERIC driver, so the kernel runs under QEMU/KVM (IDE/AHCI/
virtio storage, e1000/virtio network, ns16550 serial console) and on real
hardware. Future DER/32 kernel policy goes in this file as explicit
`options`/`no options` lines, so the difference from GENERIC stays
reviewable.

## Boot and userland

* Bootloader: NetBSD `mbr` → `bootxx_ffsv1` → `/boot`, installed with
  `installboot -o console=com0,speed=9600`. The image uses the serial
  console, which suits QEMU `-nographic` and headless machines.
* Init: traditional BSD `init(8)` + `/etc/rc` + `rc.d`. No service manager.
* Shell: NetBSD `/bin/sh` (POSIX).
* Root filesystem: FFSv1 on an MBR/disklabel partition.
* Login: `getty` on `tty00` (the console UART) with the DER/32 banner.

## upkg

See `packages.md`. In outline: about 6,000 lines of C, built two ways:

* **in base**: `src/usr.sbin/upkg/Makefile` is a NetBSD reachover Makefile
  that compiles `upkg/src` with BSD make, linking the base `libarchive`,
  `libsqlite3`, `libfetch` and OpenSSL `libcrypto`. It installs
  `/usr/sbin/upkg` and `upkg.8` and is listed in the base sets.
* **on a development host**: `upkg/Makefile` links the same code against
  the host's libarchive, sqlite3, OpenSSL and libcurl (transport only), for
  the test suite and for publishing repositories.

Components:

| File | Responsibility |
|---|---|
| `config.c` | `/etc/upkg/upkg.conf`, `/etc/upkg/repos.conf` (INI) |
| `repo.c` | signed indexes: sync, verify, anti-replay, publication |
| `sig.c` | Ed25519 keygen/sign/verify (OpenSSL EVP) |
| `fetch.c` | `file://`, local paths, `https://` (libfetch or libcurl) |
| `pkgfile.c` | `.upkg` read/verify/extract/create (libarchive) |
| `meta.c` | `+PKGINFO`/`+MANIFEST` schema, path safety rules |
| `version.c` | version ordering, dependency constraints |
| `db.c` | SQLite database `/var/lib/upkg/upkg.db` |
| `txn.c` | locking, journal, atomic commit, crash recovery |
| `ops.c` | dependency resolution, conflicts, install/remove/upgrade |
| `build.c` | source recipes, unprivileged builds |

## Build pipeline

```
make tools    build.sh tools                cross toolchain (gcc, binutils, nbmake…)
make kernel   build.sh kernel=DER32         out/obj/sys/arch/amd64/compile/DER32/netbsd
make world    build.sh distribution         out/destdir (includes /usr/sbin/upkg)
make sets     build.sh sets releasekernel   out/release/amd64/binary/sets/*.tgz
make repo     scripts/mkrepo.sh             out/repos (cross-built native packages)
make image    der32image/Makefile           out/images/der32-0.1-amd64.img
make iso      scripts/mkiso.sh              out/images/der32-0.1-amd64.iso
make test     scripts/test.sh               upkg suite + artifact checks + QEMU boot
```

Everything runs unprivileged-style (`build.sh -U`): file ownership comes
from the METALOG and the mtree specifications, not from the host. No step
writes outside `out/` or touches a physical disk.
