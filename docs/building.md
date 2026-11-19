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

About 20 GiB of free disk space. KVM (`/dev/kvm`) is used when available.
Without it QEMU falls back to TCG, which works but is slower.

`make check` reports anything missing and gives the command to install it.

## Quick start

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/luciferindev/der-32
cd der-32
make check        # verify the host
make release      # tools, kernel, world, sets, test repos, image, iso
make test         # upkg suite + artifact checks + QEMU boot test
make qemu         # boot DER/32 on this terminal (root / der32; Ctrl-A X quits)
```

If you cloned without `--recurse-submodules`, the first build fetches the
pinned NetBSD commit for you (`scripts/patches.sh apply`).

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
| `make image` | bootable raw disk image | `out/images/der32-0.1-amd64.img` |
| `make iso` | bootable live ISO | `out/images/der32-0.1-amd64.iso` |
| `make release` | all of the above | |
| `make qemu` | boot a working copy of the image | `out/images/der32-run.img` |
| `make test` | all automated tests | `out/logs/test-*.log`, `qemu-test.log` |
| `make upkg` / `make test-upkg` | host upkg build / its test suite | `upkg/upkg` |

Each step writes a full log to `out/logs/<step>.log`. On failure the driver
prints the first error lines and the log path. All NetBSD steps use
`build.sh -U -u`: they run unprivileged and resume incrementally, so after
an interruption you just re-run the same target.

`JOBS=n` sets the parallelism (default: CPUs + 1). `DER32_OUT=/path` moves
the output directory.

## Build configuration

`conf/mk.conf` is passed to `build.sh` as `MAKECONF`. The Genesis profile
builds a lean, text-console base system. It turns off X11, 32-bit compat
libraries, debug/profile libraries, ATF tests, rump, ZFS, DTrace, Kerberos,
LDAP, Postfix, BIND, Unbound, NSD, iSCSI, groff and kernel modules. GCC
stays on, so DER/32 is self-hosting (`cc` works on the system, and
`upkg build` uses it). Re-enabling a feature is a one-line change, plus a
rebuild of `world`.

**MKGCC must stay `yes`.** With `MKGCC=no`, NetBSD skips the cross compiler
as well, and later steps fail with `CC=false`. If a build ever ran that
way, delete `out/obj` (except `out/obj/tools`) and `out/destdir` before
rebuilding, because stub headers generated with `CPP=false` would
otherwise be reused.

## The image

`der32-0.1-amd64.img` is a raw disk: MBR, NetBSD disklabel, FFSv1 root,
no swap. It is made by `src/distrib/amd64/der32image/Makefile` with
NetBSD's stock `Makefile.bootimage`:

* sets `base etc comp misc man` + `kern-DER32`
* `installboot -o console=com0,speed=9600`, with getty on `tty00`
* `hostname=der32`, `dhcpcd=YES`, `rc_configured=YES`
* root password `der32` (bcrypt hash in `der32image/root.hash`).
  **Change it** (`passwd`) on any machine that is not a throwaway VM.
* `/etc/upkg/` (config + the test signing key), and
  `/usr/share/upkg/repos/` with the official test repositories
* `/usr/ports/` (the recipes) and `/usr/share/upkg/distfiles/`

Write it to a disk with `dd if=out/images/der32-0.1-amd64.img of=/dev/sdX
bs=1M` to install DER/32 on real hardware. The console is serial, so
attach a terminal to COM1 at 9600 baud. A VGA-console variant is on the
roadmap.

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

* **`Failed to make depend ... CC=false`**: see `MKGCC` above.
* **Interrupted build**: re-run the same `make` target, since `-u`
  resumes. If you see "unknown type" errors in generated headers, a
  generator was killed mid-write. Remove the affected file from `out/obj`
  and `out/destdir`, or clear both as described above.
* **`checkflist` failures in `make sets`**: a file is installed but not
  listed in `src/distrib/sets/lists/`. Add it there (with the right
  `MK*` conditions) in a patch.
