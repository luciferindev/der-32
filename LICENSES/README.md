# Licensing

* **DER/32 original work** (upkg/, scripts/, conf/, ports/ recipes, docs/,
  and the DER/32 additions inside patches/): BSD 2-Clause, see
  `BSD-2-Clause.txt`. New files carry an `SPDX-License-Identifier`.
* **NetBSD** (src/, and the upstream code that patches/ modify): the
  original NetBSD licences and copyright notices, which are kept
  unchanged. They are listed in `src/sys/conf/copyright` and in each
  source file, and are mostly BSD-style. Some components under
  `src/external/` use their own licences (e.g. GPL for the GNU toolchain).
  DER/32 adds its own copyright line to the kernel banner and does not
  remove or replace any upstream notice.
* **Third-party packages** built from `ports/` keep their upstream
  licences, recorded in each recipe's `license` key (e.g. Lua: MIT; the
  licence text is installed with the package).
