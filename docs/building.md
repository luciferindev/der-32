# Building DER/32

DER/32 cross-builds from an ordinary Linux (developed on Arch Linux) or BSD
host with NetBSD's `build.sh`. Nothing is installed on the host, no root
privileges are needed, and every artifact goes to `out/`.

## Requirements

| | Arch Linux | Debian/Ubuntu |
|---|---|---|
| compiler, make, git | `base-devel git` | `build-essential git` |
| compression, python | `xz python` | `xz-utils python3` |
| host upkg (tests, repo publishing) | `libarchive sqlite openssl curl pkgconf` | `libarchive-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev pkg-config` |
| QEMU (`make qemu`, `make test`) | `qemu-base` | `qemu-system-x86 qemu-utils` |
| UEFI boot tests (optional) | `edk2-ovmf` | `ovmf` |

About 25 GiB of free disk space. KVM (`/dev/kvm`) is used when available.
Without it QEMU falls back to TCG, which works but is slower.

`make check` reports anything missing and gives the command to install it.

## Quick start

```sh
git clone https://github.com/luciferindev/der-32
cd der-32
make check        # verify the host
make release      # tools, kernel, world + X11, sets, repos, all images
make test         # upkg suite + artifact checks + QEMU boot tests
make qemu         # boot DER/32 on this terminal (root / der32; Ctrl-A X quits)
```

The first build fetches the two pinned NetBSD trees, `src/` and `xsrc/`
(X11), shallow, about 2 GB together (`scripts/patches.sh apply` and
`build.sh`'s `need_xsrc`). Existing clones pick them up the same way.

## Targets

| target | does | output |
|---|---|---|
| `make check` | host environment report | — |
| `make patches` | fetch the pinned source, apply `patches/` | `src/` on branch `der32` |
| `make tools` | cross toolchain | `out/tools/` |
| `make kernel` | `DER32` kernel | `out/obj/sys/arch/amd64/compile/DER32/netbsd` |
| `make world` | complete userland, incl. upkg | `out/destdir/` |
| `make sets` | release sets + `kern-DER32.tgz` | `out/release/amd64/binary/sets/` |
| `make repo` | official test repositories (native packages, signed) | `out/repos/` |
| `make image` | ready-to-run disk, serial console | `out/images/der32-0.1-amd64.img` |
| `make image-pc` | ready-to-run disk, screen console | `out/images/der32-0.1-amd64-pc.img` |
| `make installer` | USB installer (live system + sets) | `out/images/der32-0.1-amd64-install.img` |
| `make iso` | live + install ISO (needs `make installer` first) | `out/images/der32-0.1-amd64.iso` |
| `make release` | all of the above | |
| `make qemu` | boot a working copy of the image | `out/images/der32-run.img` |
| `make test` | all automated tests | `out/logs/test-*.log`, `qemu-test.log` |
| `scripts/install-test.py ISO` | install to a blank disk, boot it with BIOS and UEFI | `out/logs/install-test-*.log` |
| `scripts/publish-repo.sh` | sign `out/repos` with the release key into `site/repo/` | `site/repo/` |
| `make upkg` / `make test-upkg` | host upkg build / its test suite | `upkg/upkg` |

Each step writes a full log to `out/logs/<step>.log`. On failure the driver
prints the first error lines and the log path. All NetBSD steps use
`build.sh -U -u`: they run unprivileged and resume incrementally, so after
an interruption you just re-run the same target.

`JOBS=n` sets the parallelism (default: CPUs + 1). `DER32_OUT=/path` moves
the output directory.

## Build configuration

`conf/mk.conf` is passed to `build.sh` as `MAKECONF`. The Genesis profile
builds the base system plus X11 (`MKX11=yes`, needed for the XFCE install
option; `MKLLVMRT=no` keeps LLVM out of Mesa). It turns off 32-bit compat
libraries, debug/profile libraries, ATF tests, rump, ZFS, DTrace, LDAP,
Postfix, BIND, Unbound, NSD, iSCSI, groff and kernel modules. GCC stays on,
so DER/32 is self-hosting (`cc` works on the system, and `upkg build` uses
it). Re-enabling a feature is a one-line change, plus a rebuild of `world`.

**MKGCC must stay `yes`.** With `MKGCC=no`, NetBSD skips the cross compiler
as well, and later steps fail with `CC=false`. If a build ever ran that
way, delete `out/obj` (except `out/obj/tools`) and `out/destdir` before
rebuilding, because stub headers generated with `CPP=false` would
otherwise be reused.

## The images

All images come from `src/distrib/amd64/der32image/Makefile` with
NetBSD's stock `Makefile.bootimage`. Each is a raw disk with a **GPT** (plus
a hybrid MBR), an EFI system partition holding `bootx64.efi`/`bootia32.efi`,
and an FFSv1 root. They boot from both **BIOS** (`gptmbr` →
`bootxx_ffsv1` → `/boot`) and **UEFI**. `/boot.cfg` carries the console,
because the UEFI loader reads only that file.

| `DER32_CONSOLE` | image | console |
|---|---|---|
| `serial` | `der32-0.1-amd64.img` | `com0` at 9600 baud, getty on `tty00` |
| `pc` | `der32-0.1-amd64-pc.img` | screen and keyboard, getty on `ttyE0` |
| `any` + `DER32_SETSDIR` | `der32-0.1-amd64-install.img` | `auto` (serial if a terminal answers, else screen), getty on `constty`; boot menu can force either |

Contents: sets `base etc comp misc man` plus `kern-DER32`, `hostname=der32`,
`dhcpcd=YES`, `certctl_init=YES`, `/etc/upkg/` (config, the release key and
the build's test key), `/usr/share/upkg/` (the repositories as built, plus
distfiles) and `/usr/ports/`. The installer image also carries every
release set in `/der32/sets/`, X11 sets included.

The root password is `der32` (`der32image/root.hash`). On the two disk
images it is **expired**: the first login makes you choose a new one. The
installer image and the ISO are live systems and don't expire it.

The ISO (`scripts/mkiso.sh`) reuses the installer's tree. It boots from
BIOS via El Torito (`bootxx_cd9660`, console `auto`) and from UEFI via an
`efiboot.img` El Torito entry. The root is read-only cd9660, and
`rc.d/der32live` copies `/etc`, `/var`, `/root`, `/usr/local` and `/usr/pkg`
into tmpfs.

## Installing

Boot the installer image (USB) or the ISO and run `der32-install`
(`installer/der32-install.sh`, installed to `/usr/sbin` by patch 0012):
GPT with `der32efi`, `der32swap` and `der32root` partitions, BIOS + UEFI
loaders, the sets, `fstab` by label, users, time zone and an optional
XFCE desktop. `der32-install -f answers` runs unattended, and
`scripts/install-test.py` uses that to install onto a blank QEMU disk and
boot the result with BIOS and with UEFI (OVMF). See `der32-install(8)`.

`scripts/install-image.sh` (`make install-image`) still copies a
prepared disk image onto a whole disk from a Linux host, with guards
(no partitions, no mounted or swapped devices, size check, typed
`ERASE /dev/sdX`). That gives a 2 GiB root no matter how big the disk is,
so `der32-install` is usually the better choice.

## Releases and the online repositories

`.github/workflows/release.yml` builds everything on a GitHub runner, runs
the test suites and the installer test, and publishes a GitHub Release with
the compressed images and `SHA256SUMS`. Trigger it by pushing a `v*` tag,
or from the Actions tab (`Run workflow`). `.github/workflows/ci.yml` runs
the fast upkg suite on every push.

The online upkg repositories are served from the website
(`https://luciferindev.github.io/der-32/repo/0.1/amd64/{core,extra,community}`).
`make repo` builds them, and `scripts/publish-repo.sh` re-signs the indexes
with the **release key** and copies them, with the distfiles, into
`site/repo/`. Images trust `conf/upkg/keys/der32-release.pub`. The private
key is never committed: keep it offline and point `UPKG_RELEASE_KEY` at it.

## Patches workflow

```sh
cd src
git checkout der32                 # patches/ applied on top of UPSTREAM_COMMIT
$EDITOR sys/...                    # change NetBSD code
git commit -a -m "area: what and why"
cd .. && scripts/patches.sh export # regenerates patches/*.patch
git add patches && git commit      # the outer repo records the delta
```

`scripts/patches.sh status` shows the pinned commit and whether the series
is applied. Do not `git add src` in the outer repository: the submodule
must stay pinned to the upstream commit (`.gitmodules` sets
`ignore = all`).

## Upstream synchronisation

1. Pick a new commit on `netbsd-10` (or a new branch) and put it in
   `UPSTREAM`.
2. `cd src && git fetch --depth 1 origin <commit>`, then
   `git rebase --onto <commit> <old-commit> der32` and resolve conflicts.
3. `scripts/patches.sh export`, then rebuild from scratch (`make clean
   release`) and run `make test`.
4. Commit `UPSTREAM`, the gitlink (`git update-index --cacheinfo
   160000,<commit>,src`) and `patches/` together.

## Troubleshooting

* **GCC 14+ host (current Arch)**: errors like `passing argument 2 of
  'ar_scan' from incompatible pointer type` in `tools/gmake`. GCC 14 made
  some old warnings into errors and GCC 15 defaults to C23. `build.sh`
  probes the host compiler and builds the *host tools* with `-std=gnu17`
  and those errors demoted to warnings. To override, set `HOST_CFLAGS`.

* **`Failed to make depend ... CC=false`**: see `MKGCC` above.
* **Interrupted build**: re-run the same `make` target, since `-u`
  resumes. If you see "unknown type" errors in generated headers, a
  generator was killed mid-write. Remove the affected file from `out/obj`
  and `out/destdir`, or clear both as described above.
* **`checkflist` failures in `make sets`**: a file is installed but not
  listed in `src/distrib/sets/lists/`. Add it there (with the right
  `MK*` conditions) in a patch.
