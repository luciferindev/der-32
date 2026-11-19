# DER/32 package repositories

DER/32 0.1 defines three repository tiers. `upkg` handles them as ordinary
configured repositories with priorities:

| repo | priority | contents |
|---|---|---|
| `core` | 100 | packages maintained with the base system (toolchain-adjacent, libraries) |
| `extra` | 80 | additional maintained packages |
| `community` | 50 | community-maintained packages |

These directories hold no binaries. Repositories are *built*:
`make repo` (scripts/mkrepo.sh) cross-compiles the recipes in `ports/`
with the DER/32 toolchain against the DER/32 userland, packs them as
`.upkg`, and signs each `INDEX` with the DER/32 test key in
`out/repos/{core,extra,community}`. The disk image ships them in
`/usr/share/upkg/repos/`, so that `upkg sync && upkg install lua-hello`
works offline on a fresh system.

Genesis test repository contents:

| repo | package | notes |
|---|---|---|
| core | `der32-hello 1.0` | native C program; reads `kern.der32.*` via sysctl(3) |
| core | `lua 5.4.7` | upstream Lua, cross-built natively for DER/32 |
| extra | `lua-hello 1.0` | `arch any` script, `depend lua>=5.4` |
| community | `der32-hello 1.1` | newer version in a lower-priority repo: priority and `repo/name` selection test |

Publishing a real repository is described in `docs/packages.md`.
