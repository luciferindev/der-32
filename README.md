<p align="center"><img src="docs/art/logo.png" alt="DER/32 logo: Genesis the tortoise in a bowler hat" width="200"></p>

# DER/32

**DER/32** ("Derivative 32") is a NetBSD-derived Unix operating system for
amd64. It has its own system identity, release engineering and kernel
configuration, and a native package manager, **upkg**.

```
DER/32 UNIX Operating System
Release 0.1 — Genesis

der32/amd64 (tty00)

login:
```

* NetBSD 10 kernel, libc and userland, kept close to upstream and
  maintained as an auditable patch series (`patches/`) on a pinned NetBSD
  commit (`UPSTREAM`).
* Traditional Unix: BSD `init` + `rc.d`, POSIX `/bin/sh`, FFS, plain-text
  configuration, text console. No systemd, no service manager, no
  mandatory GUI.
* `uname -s` → `DER32`, the kernel version string `DER/32 0.1 GENESIS`, and
  `sysctl kern.der32.*` are all implemented in the kernel build, not
  patched-in strings.
* **upkg**: a multi-repository package manager written in C. It has
  Ed25519-signed indexes, SHA-256 verified packages, dependency resolution,
  conflict detection, repository priorities, crash-safe transactions on
  SQLite, and unprivileged source builds. Online repositories are served
  from the project site, and a copy ships on every medium.
* **der32-install**: a real installer. It sets up GPT, makes the disk boot
  from both BIOS and UEFI, installs the sets, and creates users. It can
  optionally add X11 and the XFCE desktop.

## Install

Boot the USB installer image or the ISO, log in as `root` (password
`der32`), and run `der32-install`. The
[install guide](https://luciferindev.github.io/der-32/install.html) has the
details. Images come from the
[releases page](https://github.com/luciferindev/der-32/releases), or you can
build them yourself:

## Mascot

Meet **Genesis**, the DER/32 tortoise: slow, steady, carries its house on
its back. Run `derfetch` on DER/32 to see Genesis beside your system info. The logo, Genesis in a bowler hat, is [`docs/art/logo.png`](docs/art/logo.png); the original drawing is [`docs/art/genesis.png`](docs/art/genesis.png).
A fastfetch logo version is in [`share/fastfetch/`](share/fastfetch/README.md):

```
                   _.-''''''-._
               .-'  \________/  '-.
             .'      |      |      '.
   .---.    /--------'      '-------\
  /     \  |      /  '-.__.-'  \     |
 |  --   '-|_____/______________\____|_.
  '-.________________________________ __)
             |  |               |  |
             '--'               '--'
```

## Build

On Arch Linux (or any Linux/BSD host, no root needed):

```sh
sudo pacman -S --needed base-devel git xz python pkgconf \
    libarchive sqlite openssl curl qemu-base
git clone https://github.com/luciferindev/der-32
cd der-32
make check     # host report
make release   # toolchain, kernel, userland + X11, sets, repos, all images
make test      # automated tests, including full QEMU boots
make qemu      # boot it: login root / password der32, Ctrl-A X to quit
```

The first build fetches the pinned NetBSD `src` and `xsrc` trees (about
2 GB). `make release` leaves these in `out/images/`:

| image | what |
|---|---|
| `der32-0.1-amd64-install.img` | USB installer: live system plus sets, BIOS + UEFI |
| `der32-0.1-amd64.iso` | the same as a live + install ISO, BIOS + UEFI |
| `der32-0.1-amd64.img` | ready-to-run disk, serial console (QEMU, servers) |
| `der32-0.1-amd64-pc.img` | ready-to-run disk, screen and keyboard |

The individual targets are `make tools kernel world sets repo image
image-pc installer iso`. See [docs/building.md](docs/building.md).

## upkg in one minute

```sh
upkg sync                       # verify + cache every repository index
upkg install lua-hello          # resolves lua from core, lua-hello from extra
upkg install community/der32-hello   # explicit repository selection
upkg remove lua                 # refused: lua-hello depends on it
upkg remove lua-hello && upkg autoremove
upkg repo add web https://example.org/der32/core -p 90
```

See [docs/packages.md](docs/packages.md) for the package and repository
formats, the trust model and the transaction design.

## NetBSD packages

DER/32 keeps the NetBSD 10 ABI, so NetBSD's own binary packages work
out of the box under `/usr/pkg`, next to upkg's `/usr/local`:

```
pkg_add pkgin && pkgin update
pkgin install vim git fastfetch
```

`/etc/pkg_install.conf` points at cdn.NetBSD.org's `x86_64/10.2` tree.
Details and limits are in [docs/packages.md](docs/packages.md#netbsd-binary-packages-pkgsrc).

## Layout

| path | |
|---|---|
| `UPSTREAM`, `src/`, `xsrc/`, `patches/` | pinned NetBSD (+ X11) and the DER/32 patch series |
| `installer/` | `der32-install(8)`, the disk installer, in base |
| `upkg/` | package manager (C), test suite, man page |
| `derfetch/` | `derfetch(1)`: system info next to Genesis, in base |
| `ports/` | source recipes (`upkg build`) |
| `repos/` | repository tiers: core, extra, community |
| `conf/` | build profile (`mk.conf`) and default `/etc/upkg` |
| `scripts/` | build, image, ISO, repository, QEMU, installer-test and site drivers |
| `site/` | the website (GitHub Pages), including the online package repositories |
| `share/fastfetch/` | fastfetch logo + config (Genesis) |
| `docs/` | [architecture](docs/architecture.md), [building](docs/building.md), [packages](docs/packages.md), [roadmap](docs/roadmap.md), [development log](docs/devlog.md) |

## Status

0.1 Genesis is under active development. The development log
([docs/devlog.md](docs/devlog.md)) records what has been verified and how,
and [docs/roadmap.md](docs/roadmap.md) lists what remains.

## Licence

DER/32's own code is BSD-2-Clause. NetBSD code keeps its original licences
and notices. See [LICENSES/](LICENSES/README.md).
