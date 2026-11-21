# DER/32 roadmap

Status values: **done** (implemented and verified by an automated test,
evidence in `devlog.md`), **partial**, **planned**.

## Milestone 0 — Foundation

| item | status |
|---|---|
| DER/32 repository, layout, licences | done |
| pinned NetBSD source (`UPSTREAM`, submodule) | done (netbsd-10 @ `06f422a9d514`, 10.2_STABLE) |
| build scripts with environment detection and logs | done |
| cross toolchain | done |
| DER32 kernel | done |
| bootable image, QEMU boot | done (38/38 in-system checks) |

## Milestone 1 — Independent identity

| item | status |
|---|---|
| `kern.ostype` = DER32, DER/32 version string, `kern.der32.*` | done |
| `/etc/release`, motd, getty banner, hostname | done |
| DER32 kernel in installation media | done (image + live ISO) |
| identity verified through the running system | done |

## Milestone 2 — upkg foundation

| item | status |
|---|---|
| upkg in C; package format 1; SQLite database | done |
| multiple repositories, priorities, enable/disable, `repo/pkg` | done |
| Ed25519-signed indexes, anti-substitution, anti-replay | done |
| checksums, arch checks, version constraints, dependency resolution | done |
| conflicts (declared + file ownership), dependency-aware removal, autoremove | done |
| upgrades, transaction locking, journal + crash recovery, SIGINT handling | done |
| HTTPS transport (libfetch on DER/32, libcurl on hosts) | done (host-tested; in-OS HTTPS needs a reachable server) |
| source recipes, unprivileged builds | done |
| integration tests (136 upkg checks, 50 disk / 36 live-ISO boot checks) | done |

## Milestone 3 — System integration

| item | status |
|---|---|
| upkg in the base image (`/usr/sbin/upkg`, `upkg.8`, set lists) | done |
| official test repositories | done (`make repo`) |
| real native packages (Lua 5.4.7, der32-hello) | done (cross-built) |
| installation + upgrade inside QEMU | done |

## Milestone 4 — Independent distribution

| item | status |
|---|---|
| reproducible release pipeline (`make release`) | partial: works, but timestamps are not yet pinned (`MKREPRO`) |
| documented upstream synchronisation | done (`building.md`) |
| maintained patch series | done (`patches/`, `scripts/patches.sh`) |
| repository publication tooling | done (`upkg keygen/create/index`, `mkrepo.sh`) |
| security update procedure | planned (see below) |
| 0.1 Genesis release artifacts | done as development builds (image, ISO, sets); not yet signed with a release key |

## Known limitations and next steps

* **pkgsrc (source)**: `uname -s` reports `DER32`, so building pkgsrc
  *from source* on DER/32 does not recognise the platform. Plan: carry a
  small pkgsrc patch mapping `OPSYS=DER32` to the NetBSD platform files,
  then add a `pkgsrc <path>` recipe type to `upkg build`. NetBSD's
  *binary* packages already work (see `docs/packages.md`), because
  `pkg_add` checks the compile-time OS name, not `uname -s`.
* **Console**: the existing serial image remains the tested default. An experimental
  VGA/`wscons` variant (`make image-pc`, `console=pc`, getty on `ttyE0`) has
  been implemented but needs QEMU VGA and real-machine validation.
* **Installer**: `scripts/install-image.sh` now provides a guarded Linux
  whole-disk writer with explicit confirmation. A real sysinst-based or
  scripted installer (partitioning, user creation, password) is still planned.
* **Default root password** `der32` on the image: replace with a
  first-boot password prompt.
* **Package scripts**: by design upkg runs no install scripts. Services
  that need users or groups need a declarative mechanism
  (`user`/`group` keys in `+PKGINFO`), not shell hooks.
* **Configuration files**: upgrades overwrite package-owned files. Add a
  `config <path>` key so modified configuration is preserved
  (`.upkgnew`).
* **Signing keys**: the Genesis image trusts only a *test* key generated
  at build time. A release key with an offline signing procedure, key
  rotation and revocation lists in the index format are planned.
* **Security updates**: plan to publish a signed advisory feed next to
  each repository and an `upkg audit` command. Base-system fixes ship as
  rebuilt sets from the patched `der32` branch.
* **Kernel modules** are disabled in the Genesis profile (`MKKMOD=no`).
  Re-enabling needs a `kern.der32`-aware module path policy.
* **Architectures**: amd64 only. The tree is architecture-neutral apart
  from `DER32` kernel configs and `der32image`.
