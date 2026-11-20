# upkg — packages, repositories and formats

upkg manages third-party software on DER/32. It never touches the base
system: by default packages may only install below `/usr/local`
(`prefixes` in `upkg.conf`).

## Commands

```sh
upkg sync                      # fetch + verify every enabled repository index
upkg install curl              # resolve dependencies, verify, install
upkg install core/curl         # take curl from the "core" repository only
upkg install 'lua>=5.4'        # version constraint (<, <=, =, >=, >)
upkg remove curl               # refused if another package needs curl
upkg remove -c libfoo          # ... or remove the dependents as well
upkg autoremove                # remove orphaned dependencies
upkg upgrade                   # upgrade everything with a newer version
upkg search lua                # names and descriptions, all repositories
upkg info lua                  # installed + repository details
upkg list [-e|-d] [-f pkg]     # installed packages / owned files
upkg check [pkg]               # verify installed files against the database
upkg build lua                 # build a package from ports/
upkg repo list | add NAME URL [-p PRIO] [-k KEY] | remove NAME
upkg repo enable NAME | disable NAME | priority NAME N
upkg keygen PREFIX             # Ed25519 key pair for signing a repository
upkg create -i PKGINFO -d STAGEDIR [-o FILE]
upkg index -n REPO -k KEYFILE DIR
upkg recover                   # finish/undo an interrupted transaction
upkg vercmp A B
```

`-r ROOT` (or `UPKG_ROOT`) operates on another root. The test suite and
image builds use it. `-n` gives a dry run for install, remove, upgrade
and autoremove.

## Configuration

`/etc/upkg/upkg.conf`:

```ini
[options]
prefixes   = usr/local          # where packages may install
portsdir   = /usr/ports         # recipes for "upkg build"
builddir   = /var/tmp/upkg-build
build_user = nobody             # source builds started by root run as this user
ca_file    =                    # extra CA bundle for HTTPS repositories
allow_http = no                 # plain HTTP repositories are refused by default
```

`/etc/upkg/repos.conf`:

```ini
[core]
url = https://example.org/der32/core     # https://, file:// or an absolute path
priority = 100                           # higher wins
enabled = yes
key = der32-release                      # optional: accept only this key
```

Trusted public keys live in `/etc/upkg/keys/<name>.pub` (PEM, Ed25519).

## Candidate selection

For each requested package or dependency, upkg goes through the enabled
repositories from highest to lowest priority. The first repository that
has a package with the right name, a compatible architecture (the
system's, or `any`) and a version satisfying the constraint wins. Within
that repository the newest such version is chosen. `repo/name` limits the
search to one repository, and an error is raised if that repository is
unknown or disabled. Repositories with equal priority keep their order in
`repos.conf`.

## Trust model

1. `upkg sync` downloads `INDEX` and `INDEX.sig`. The signature must verify
   against a trusted key (or the repository's `key =`). The index header
   must name this repository, and its timestamp must not be older than the
   cached index. These checks block forged indexes, index substitution
   between repositories, and replay of old indexes. A failed check leaves
   the cached copy untouched.
2. The cached index is re-verified every time it is loaded, so the cache is
   never trusted on its own.
3. A package archive is accepted only if its size and SHA-256 match the
   signed index. Its `+PKGINFO` must also match the index entry (name,
   version, release, arch, dependencies, conflicts).
4. Every regular file is hashed while it is extracted and compared with
   `+MANIFEST`.
5. Local `.upkg` files have no signature, so they need `--allow-unsigned`.

The transport never affects integrity. HTTPS is still required for remote
repositories (`allow_http = no`) to protect privacy and availability.

## Package safety rules

An archive is rejected, and nothing is installed, if:

* a path is absolute, contains `..` / `.` / empty components or control
  characters, or lies outside the permitted prefixes;
* a symlink target is absolute, or leaves the permitted prefixes when
  resolved from the link's directory;
* a member is a hard link, device, FIFO or socket, or is not listed in the
  manifest, or a manifest entry is missing from the archive;
* a file is setuid, setgid or sticky;
* any checksum or size differs;
* the metadata is malformed, has unknown keys, a different schema
  `format`, or an invalid name/version/dependency;
* a file is owned by another package, or exists on disk without an owner
  (it is never overwritten);
* a declared conflict involves an installed or selected package;
* an upgrade would break an installed package's version constraint.

During extraction upkg writes every object itself, with `O_NOFOLLOW`, and
refuses to traverse a symlink in any parent directory. **No package script
is ever executed** during install, remove or upgrade.

## Transactions and recovery

Each modifying command runs as one transaction under an exclusive lock
(`/var/lib/upkg/lock`):

1. **Stage** — new files are written as `<path>.upkg-new`. Each action is
   recorded in `/var/lib/upkg/journal` *before* it is performed.
2. **Commit** — the journal is fsync'd. Then the database changes and a
   marker row `txn(id)` are committed in **one SQLite transaction**. That
   commit is the atomic decision point.
3. **Apply** — staged files are `rename(2)`d into place (atomic per file),
   obsolete files are unlinked and empty directories removed.
4. **Finish** — the marker and the journal are deleted.

At the start of every modifying command (or with `upkg recover`), a
leftover journal is resolved. If its marker is in the database, the
transaction is rolled forward; otherwise it is rolled back. Both
directions are idempotent. SIGINT/SIGTERM/SIGHUP abort cleanly while
staging and are deferred during commit and apply. The test suite kills
upkg with SIGKILL at each phase boundary and checks the result.

## Installed package database

`/var/lib/upkg/upkg.db` (SQLite, rollback journal, `synchronous=FULL`):

| table | contents |
|---|---|
| `packages` | name, version, release, arch, **repo** (provenance), **reason** (`explicit`/`dependency`), description, install time, full +PKGINFO |
| `files` | path → owning package, type, mode, size, sha256, link target |
| `dirs` | directory → package(s) (shared directories are reference-counted) |
| `deps` | package → dependency specification |
| `txn` | commit markers of in-flight transactions |
| `meta` | schema version (`1`) |

## Package format — `.upkg`, format 1

An xz-compressed POSIX pax tar archive. Its members are, in this order:

1. `+PKGINFO` — metadata, `key value` lines:

   ```
   format 1
   name lua
   version 5.4.7
   release 1
   arch amd64                 # or "any"
   description Lua, a powerful ... scripting language
   license MIT
   maintainer DER/32 Project
   origin https://github.com/lua/lua (tag v5.4.7, ...)
   depend libfoo>=1.2         # repeatable
   conflict lua51             # repeatable
   ```

   `name`: `[A-Za-z0-9][A-Za-z0-9+._-]*`. `version`:
   `[A-Za-z0-9][A-Za-z0-9+._~]*` (no `-`). `release`: integer, default 1.
   Unknown keys are an error.

2. `+MANIFEST` — one tab-separated record per object, sorted by path:

   ```
   f <TAB> mode <TAB> size <TAB> sha256 <TAB> path
   d <TAB> mode <TAB> path
   l <TAB> target <TAB> path
   ```

3. the payload, one member per manifest record, in manifest order.

Archives are reproducible: owners are always `root:wheel` and mtimes are
`SOURCE_DATE_EPOCH` (default 0). File names follow
`<name>-<version>-<release>-<arch>.upkg`.

## Repository format — `INDEX`, format 1

```
upkg-index 1
repo core
timestamp 1795131346

name lua
version 5.4.7
release 1
arch amd64
description ...
filename lua-5.4.7-1-amd64.upkg
size 123456
sha256 <64 hex digits>
```

Stanzas are separated by blank lines. The package keys are the `+PKGINFO`
keys plus `filename`, `size` and `sha256`. `INDEX.sig` holds the Ed25519
signature over the exact bytes of `INDEX`, as hex.

Publishing a repository:

```sh
upkg keygen der32-release              # once; keep der32-release.key offline
cp *.upkg /srv/der32/core/
upkg index -n core -k der32-release.key /srv/der32/core
# serve /srv/der32/core over HTTPS; ship der32-release.pub to /etc/upkg/keys/
```

## Versions

Versions are compared segment by segment: digit runs numerically, letter
runs lexically, other characters as separators. Numeric segments sort
above alphabetic ones, `~` sorts below everything (`1.0~rc1 < 1.0`), and
more segments win (`1.0.1 > 1.0`). The release number breaks ties.
Constraints with a `-release` (`foo=1.2-3`) compare it too.

## Source recipes (`ports/`)

```
ports/<category>/<name>/RECIPE   +PKGINFO keys, plus:
                                   source <url>        distfile
                                   source_sha256 <hex> mandatory with source
                                   builddep <spec>     must be installed
ports/<category>/<name>/build    POSIX sh, run with "sh -e"
```

The build script gets `SRCDIR`, `DESTDIR`, `PREFIX=/usr/local`, `PORTDIR`,
`JOBS`, `PKGNAME` and `PKGVERSION`, and a clean environment. It must
install into `$DESTDIR$PREFIX`. When `upkg build` runs as root, the source
is unpacked and built as `build_user` (default `nobody`). Recipes run only
when you explicitly ask for a build, never as a side effect of `sync`.
`scripts/mkrepo.sh` cross-builds the same recipes for the official test
repositories.

## NetBSD binary packages (pkgsrc)

DER/32 keeps the NetBSD 10 ABI, so NetBSD's official binary packages
install unmodified with the base `pkg_add(1)`.
`/etc/pkg_install.conf` points it at

    https://cdn.NetBSD.org/pub/pkgsrc/packages/NetBSD/x86_64/10.2/All

Quick start, as root, with networking up (the image runs `dhcpcd`):

```
pkg_add pkgin              # apt-like front end; its repositories.conf
                           # already names the cdn.NetBSD.org 10.2 tree
pkgin update
pkgin search fastfetch
pkgin install vim git
derfetch                   # "Packages: 0 (upkg), N (pkgsrc)"
```

How the two package managers coexist:

| | upkg | pkg_add / pkgin |
|---|---|---|
| packages | DER/32 repositories, `ports/` | NetBSD pkgsrc builds |
| prefix | `/usr/local` | `/usr/pkg` |
| database | `/var/lib/upkg/upkg.db` | `/usr/pkg/pkgdb`, `/var/db/pkgin` |
| trust | Ed25519-signed index + SHA-256 | HTTPS to cdn.NetBSD.org (packages are not signed) |
| install scripts | never | yes (pkgsrc INSTALL/DEINSTALL) |

Why it works although `uname -s` prints `DER32`: `pkg_add` compares a
package's `OPSYS` with its compile-time `OPSYS_NAME` ("NetBSD"), not with
`uname(3)`; the release check uses `uname -r` (`10.2_STABLE`, kept for ABI
compatibility) and only warns on a mismatch. pkgin's `$osrelease` strips
the `_STABLE` suffix. Packages record `PKGTOOLS_VERSION=20091115`, well
below the base pkg_install (20210410).

Limits: the Genesis profile builds no X11 sets, so packages that depend
on base X11 libraries will not run; use packages that pull in
`modular-xorg` from pkgsrc instead, or stay on the console. HTTPS needs
`/etc/openssl/certs`, which `rc.d/certctl_init` fills on first boot
(`certctl_init=YES` in the image's rc.conf); run `certctl rehash` by hand
if you disabled it. On the live ISO, `/usr/pkg` lives in RAM like
`/usr/local`, so packages are gone after a reboot.
