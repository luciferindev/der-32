/*
 * upkg -- dependency resolution and install/remove/upgrade transactions.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Candidate selection: among enabled repositories, the one with the highest
 * priority that has a package of the requested name, a compatible
 * architecture and a version satisfying the constraint wins; within that
 * repository the newest such version is chosen.  "repo/name" restricts the
 * search to one repository.
 */

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "upkg.h"

struct item {
	struct pkgmeta	*m;		/* index entry, or &pf.meta (local) */
	struct pkgfile	 pf;
	char		*path;		/* local archive */
	const char	*reponame;
	int		 explicit;
	int		 upgrade;	/* replaces an installed version */
	char		*oldver;
	char		*reason;	/* reason to record */
};

struct plan {
	struct item	*v;
	size_t		 n;
	struct strlist	 visiting;
};

static void
plan_free(struct plan *p)
{
	size_t i;

	for (i = 0; i < p->n; i++) {
		pkgfile_free(&p->v[i].pf);
		free(p->v[i].path);
		free(p->v[i].oldver);
		free(p->v[i].reason);
	}
	free(p->v);
	sl_free(&p->visiting);
	memset(p, 0, sizeof(*p));
}

static struct item *
plan_find(struct plan *p, const char *name)
{
	size_t i;

	for (i = 0; i < p->n; i++)
		if (strcmp(p->v[i].m->name, name) == 0)
			return &p->v[i];
	return NULL;
}

static int
arch_ok(const struct config *cfg, const char *arch)
{
	return strcmp(arch, cfg->arch) == 0 || strcmp(arch, "any") == 0;
}

static struct pkgmeta *
find_candidate(struct config *cfg, const struct depspec *d)
{
	size_t i, j;

	if (d->repo) {
		struct repo *r = config_repo(cfg, d->repo);

		if (r == NULL) {
			upkg_err("unknown repository '%s'", d->repo);
			return NULL;
		}
		if (!r->enabled) {
			upkg_err("repository '%s' is disabled", d->repo);
			return NULL;
		}
	}
	for (i = 0; i < cfg->nrepos; i++) {
		struct repo *r = &cfg->repos[i];
		struct pkgmeta *best = NULL;

		if (!r->enabled || !r->loaded)
			continue;
		if (d->repo && strcmp(d->repo, r->name) != 0)
			continue;
		for (j = 0; j < r->npkgs; j++) {
			struct pkgmeta *m = &r->pkgs[j];

			if (strcmp(m->name, d->name) != 0 || !arch_ok(cfg, m->arch) ||
			    !depspec_match(d, m->version, m->release))
				continue;
			if (best == NULL || fullvercmp(m->version, m->release,
			    best->version, best->release) > 0)
				best = m;
		}
		if (best)
			return best;
	}
	return NULL;
}

static int visit(struct config *, struct db *, struct plan *,
    const char *spec, int explicit, struct opts *);

static int
add_item(struct config *cfg, struct db *db, struct plan *p, struct pkgmeta *m,
    const char *reponame, int explicit, struct opts *o, struct item *proto)
{
	struct instpkg ip;
	struct item it;
	size_t i;
	int found;

	memset(&it, 0, sizeof(it));
	if (proto)
		it = *proto;
	it.m = m;
	it.reponame = reponame;
	it.explicit = explicit;
	found = db_get_pkg(db, m->name, &ip);
	if (found == -1)
		return -1;
	if (found) {
		it.upgrade = 1;
		it.oldver = xasprintf("%s-%u", ip.version, ip.release);
		/* an explicit request promotes a dependency; upgrades keep it */
		it.reason = xstrdup(explicit ? (o->as_deps ? "dependency" :
		    "explicit") : ip.reason);
		instpkg_free(&ip);
	} else
		it.reason = xstrdup(explicit && !o->as_deps ? "explicit" :
		    "dependency");

	if (!o->nodeps) {
		sl_add(&p->visiting, m->name);
		for (i = 0; i < m->depends.n; i++)
			if (visit(cfg, db, p, m->depends.v[i], 0, o) == -1)
				return -1;
		free(p->visiting.v[--p->visiting.n]);
		p->visiting.v[p->visiting.n] = NULL;
	}
	p->v = xrealloc(p->v, (p->n + 1) * sizeof(*p->v));
	p->v[p->n++] = it;
	return 0;
}

static int
visit(struct config *cfg, struct db *db, struct plan *p, const char *spec,
    int explicit, struct opts *o)
{
	struct depspec d;
	struct instpkg ip;
	struct pkgmeta *m;
	struct item *have;
	int found, rv = -1;

	if (depspec_parse(spec, &d, explicit) == -1) {
		upkg_err("invalid package specification '%s'", spec);
		return -1;
	}
	if ((have = plan_find(p, d.name)) != NULL) {
		if (!depspec_match(&d, have->m->version, have->m->release)) {
			upkg_err("conflicting requirements: %s needed, but %s-%s-%u "
			    "is already selected", spec, have->m->name,
			    have->m->version, have->m->release);
			goto out;
		}
		if (explicit) {
			have->explicit = 1;
			free(have->reason);
			have->reason = xstrdup(o->as_deps ? "dependency" : "explicit");
		}
		rv = 0;
		goto out;
	}
	if (sl_contains(&p->visiting, d.name)) {
		rv = 0;		/* dependency cycle: already being resolved */
		goto out;
	}
	found = db_get_pkg(db, d.name, &ip);
	if (found == -1)
		goto out;
	if (found && !explicit && depspec_match(&d, ip.version, ip.release)) {
		instpkg_free(&ip);
		rv = 0;		/* satisfied by what is installed */
		goto out;
	}
	if ((m = find_candidate(cfg, &d)) == NULL) {
		if (found && !explicit) {
			upkg_err("installed %s-%s-%u does not satisfy '%s' and no "
			    "repository provides a suitable version", ip.name,
			    ip.version, ip.release, spec);
		} else
			upkg_err("no package satisfies '%s'%s", spec,
			    explicit ? "" : " (required as a dependency)");
		if (found)
			instpkg_free(&ip);
		goto out;
	}
	if (found) {
		int c = fullvercmp(m->version, m->release, ip.version, ip.release);

		if (c <= 0 && explicit && depspec_match(&d, ip.version, ip.release)) {
			upkg_msg(" %s-%s-%u is up to date", ip.name, ip.version,
			    ip.release);
			if (!o->as_deps && strcmp(ip.reason, "explicit") != 0 &&
			    !o->dry_run) {
				db_set_reason(db, ip.name, "explicit");
				upkg_msg(" %s marked as explicitly installed",
				    ip.name);
			}
			instpkg_free(&ip);
			rv = 0;
			goto out;
		}
		instpkg_free(&ip);
	}
	rv = add_item(cfg, db, p, m, m->repo->name, explicit, o, NULL);
out:
	depspec_free(&d);
	return rv;
}

/* ------------------------------------------------------------ checks */

static int
validate_manifest(struct config *cfg, struct item *it)
{
	struct manifest *mf = &it->pf.manifest;
	size_t i;

	for (i = 0; i < mf->n; i++) {
		struct mentry *e = &mf->e[i];
		int in = path_in_prefixes(e->path, &cfg->prefixes);

		if (in == 0 || (in == 2 && e->type != M_DIR)) {
			upkg_err("%s: '%s' is outside the permitted install "
			    "prefixes (%s%s)", it->m->name, e->path,
			    cfg->prefixes.n ? cfg->prefixes.v[0] : "none",
			    cfg->prefixes.n > 1 ? ", ..." : "");
			return -1;
		}
		if (e->type == M_FILE && (e->mode & ~0777)) {
			upkg_err("%s: '%s' has forbidden mode %04o "
			    "(setuid/setgid/sticky)", it->m->name, e->path,
			    (unsigned)e->mode);
			return -1;
		}
		if (e->type == M_DIR && (e->mode & ~01777)) {
			upkg_err("%s: directory '%s' has forbidden mode %04o",
			    it->m->name, e->path, (unsigned)e->mode);
			return -1;
		}
		if (e->type == M_LINK &&
		    !link_target_ok(e->path, e->target, &cfg->prefixes)) {
			upkg_err("%s: unsafe symbolic link '%s' -> '%s'",
			    it->m->name, e->path, e->target);
			return -1;
		}
	}
	return 0;
}

static int
same_list(const struct strlist *a, const struct strlist *b)
{
	size_t i;

	if (a->n != b->n)
		return 0;
	for (i = 0; i < a->n; i++)
		if (strcmp(a->v[i], b->v[i]) != 0)
			return 0;
	return 1;
}

static int
fetch_and_verify(struct config *cfg, struct item *it, struct opts *o)
{
	if (it->path == NULL) {
		if (repo_fetch_pkg(cfg, it->m, &it->path) == -1)
			return -1;
		if (pkgfile_read_meta(it->path, &it->pf) == -1)
			return -1;
		/* the archive must be exactly what the signed index describes */
		struct pkgmeta *a = &it->pf.meta, *b = it->m;
		if (strcmp(a->name, b->name) || strcmp(a->version, b->version) ||
		    a->release != b->release || strcmp(a->arch, b->arch) ||
		    !same_list(&a->depends, &b->depends) ||
		    !same_list(&a->conflicts, &b->conflicts)) {
			upkg_err("%s: package metadata does not match the "
			    "repository index", it->path);
			return -1;
		}
	}
	(void)o;
	if (!arch_ok(cfg, it->pf.meta.arch)) {
		upkg_err("%s: architecture '%s' is not compatible with '%s'",
		    it->m->name, it->pf.meta.arch, cfg->arch);
		return -1;
	}
	return validate_manifest(cfg, it);
}

static int
in_plan_name(struct plan *p, const char *name)
{
	return plan_find(p, name) != NULL;
}

static int
check_conflicts(struct config *cfg, struct db *db, struct plan *p)
{
	struct instpkg *inst = NULL;
	size_t ninst = 0, i, j, k;
	int bad = 0;

	if (db_list_pkgs(db, &inst, &ninst) == -1)
		return -1;

	for (i = 0; i < p->n; i++) {
		struct item *it = &p->v[i];

		/* declared conflicts of the new package */
		for (j = 0; j < it->pf.meta.conflicts.n; j++) {
			struct depspec d;

			if (depspec_parse(it->pf.meta.conflicts.v[j], &d, 0) == -1)
				continue;
			struct item *o = plan_find(p, d.name);
			if (o && o != it &&
			    depspec_match(&d, o->m->version, o->m->release)) {
				upkg_err("%s conflicts with %s (both selected)",
				    it->m->name, o->m->name);
				bad = 1;
			}
			for (k = 0; k < ninst; k++)
				if (strcmp(inst[k].name, d.name) == 0 &&
				    !in_plan_name(p, d.name) &&
				    depspec_match(&d, inst[k].version,
				    inst[k].release)) {
					upkg_err("%s conflicts with installed "
					    "package %s-%s-%u", it->m->name,
					    inst[k].name, inst[k].version,
					    inst[k].release);
					bad = 1;
				}
			depspec_free(&d);
		}
		/* conflicts declared by installed packages */
		for (k = 0; k < ninst; k++) {
			if (in_plan_name(p, inst[k].name))
				continue;
			for (j = 0; j < inst[k].conflicts.n; j++) {
				struct depspec d;

				if (depspec_parse(inst[k].conflicts.v[j], &d, 0) == -1)
					continue;
				if (strcmp(d.name, it->m->name) == 0 &&
				    depspec_match(&d, it->m->version,
				    it->m->release)) {
					upkg_err("installed package %s conflicts "
					    "with %s", inst[k].name, it->m->name);
					bad = 1;
				}
				depspec_free(&d);
			}
		}

		/* filesystem conflicts */
		for (j = 0; j < it->pf.manifest.n; j++) {
			struct mentry *e = &it->pf.manifest.e[j];
			char *a = path_join(cfg->root, e->path), *owner = NULL;
			struct stat st;

			if (e->type == M_DIR) {
				if (lstat(a, &st) == 0 && !S_ISDIR(st.st_mode)) {
					upkg_err("%s: '%s' exists and is not a "
					    "directory", it->m->name, e->path);
					bad = 1;
				}
				free(a);
				continue;
			}
			if (db_file_owner(db, e->path, &owner) == -1) {
				free(a);
				free(inst);
				return -1;
			}
			if (owner && strcmp(owner, it->m->name) != 0) {
				upkg_err("%s: '%s' is already owned by %s",
				    it->m->name, e->path, owner);
				bad = 1;
			} else if (owner == NULL && lstat(a, &st) == 0) {
				upkg_err("%s: '%s' exists in the filesystem and "
				    "is not owned by any package", it->m->name,
				    e->path);
				bad = 1;
			} else if (owner && lstat(a, &st) == 0 &&
			    S_ISDIR(st.st_mode)) {
				upkg_err("%s: '%s' is a directory", it->m->name,
				    e->path);
				bad = 1;
			}
			free(owner);
			free(a);
			/* two new packages shipping the same file */
			for (k = 0; k < i; k++) {
				struct mentry *x = manifest_find(&p->v[k].pf.manifest,
				    e->path);

				if (x && x->type != M_DIR) {
					upkg_err("%s and %s both contain '%s'",
					    p->v[k].m->name, it->m->name, e->path);
					bad = 1;
				}
			}
		}
	}

	/* the result must still satisfy every installed package */
	for (k = 0; k < ninst; k++) {
		if (in_plan_name(p, inst[k].name))
			continue;
		for (j = 0; j < inst[k].depends.n; j++) {
			struct depspec d;
			struct item *o;

			if (depspec_parse(inst[k].depends.v[j], &d, 0) == -1)
				continue;
			if ((o = plan_find(p, d.name)) != NULL &&
			    !depspec_match(&d, o->m->version, o->m->release)) {
				upkg_err("%s-%s-%u would break %s (requires %s)",
				    o->m->name, o->m->version, o->m->release,
				    inst[k].name, inst[k].depends.v[j]);
				bad = 1;
			}
			depspec_free(&d);
		}
	}
	for (k = 0; k < ninst; k++)
		instpkg_free(&inst[k]);
	free(inst);
	return bad ? -1 : 0;
}

/* ------------------------------------------------------------ execution */

static int
strictly_inside_prefix(struct config *cfg, const char *path)
{
	size_t i;

	for (i = 0; i < cfg->prefixes.n; i++) {
		size_t n = strlen(cfg->prefixes.v[i]);

		if (strncmp(path, cfg->prefixes.v[i], n) == 0 && path[n] == '/')
			return 1;
	}
	return 0;
}

/* journal removal of `pkg's files and of its now-unused directories */
static int
stage_removal(struct config *cfg, struct db *db, struct txn *t,
    const char *pkg, struct manifest *keep, struct strlist *also_removed)
{
	struct manifest old;
	struct strlist dirs;
	size_t i;

	if (db_pkg_files(db, pkg, &old) == -1)
		return -1;
	for (i = 0; i < old.n; i++)
		if (keep == NULL || manifest_find(keep, old.e[i].path) == NULL)
			txn_unlink(t, old.e[i].path);
	manifest_free(&old);
	if (db_pkg_dirs(db, pkg, &dirs) == -1)
		return -1;
	/* dirs are ordered deepest first */
	for (i = 0; i < dirs.n; i++) {
		struct strlist owners;
		size_t j;
		int shared = 0;

		if (keep && manifest_find(keep, dirs.v[i]))
			continue;
		if (!strictly_inside_prefix(cfg, dirs.v[i]))
			continue;
		if (db_dir_owners(db, dirs.v[i], &owners) == -1) {
			sl_free(&dirs);
			return -1;
		}
		for (j = 0; j < owners.n; j++)
			if (strcmp(owners.v[j], pkg) != 0 && (also_removed == NULL ||
			    !sl_contains(also_removed, owners.v[j])))
				shared = 1;
		sl_free(&owners);
		if (!shared)
			txn_rmdir(t, dirs.v[i]);
	}
	sl_free(&dirs);
	return 0;
}

static int
execute(struct config *cfg, struct db *db, struct plan *p)
{
	struct txn *t;
	size_t i;

	if (db_begin(db) == -1)
		return -1;
	if ((t = txn_begin(cfg, db)) == NULL) {
		db_rollback(db);
		return -1;
	}
	for (i = 0; i < p->n; i++) {
		struct item *it = &p->v[i];

		if (upkg_interrupted)
			goto fail;
		if (it->upgrade)
			upkg_msg(":: upgrading %s (%s -> %s-%u)", it->m->name,
			    it->oldver, it->m->version, it->m->release);
		else
			upkg_msg(":: installing %s %s-%u", it->m->name,
			    it->m->version, it->m->release);
		manifest_sort(&it->pf.manifest);
		if (pkgfile_extract(it->path, &it->pf, t) == -1)
			goto fail;
		if (it->upgrade) {
			if (stage_removal(cfg, db, t, it->m->name,
			    &it->pf.manifest, NULL) == -1 ||
			    db_del_pkg(db, it->m->name) == -1)
				goto fail;
		}
		if (db_add_pkg(db, &it->pf.meta, &it->pf.manifest,
		    it->reponame, it->reason) == -1)
			goto fail;
	}
	crashpoint("after-stage");
	if (upkg_interrupted)
		goto fail;
	return txn_commit(t);
fail:
	if (upkg_interrupted)
		upkg_err("interrupted; rolling back");
	txn_abort(t);
	return -1;
}

static void
show_plan(struct plan *p)
{
	size_t i;

	upkg_msg(":: %zu package%s:", p->n, p->n == 1 ? "" : "s");
	for (i = 0; i < p->n; i++) {
		struct item *it = &p->v[i];

		if (it->upgrade)
			upkg_msg("   %-24s %s -> %s-%u  [%s]%s", it->m->name,
			    it->oldver, it->m->version, it->m->release,
			    it->reponame, it->explicit ? "" : " (dependency)");
		else
			upkg_msg("   %-24s %s-%u  [%s]%s", it->m->name,
			    it->m->version, it->m->release, it->reponame,
			    it->explicit ? "" : " (dependency)");
	}
}

static int
run_plan(struct config *cfg, struct db *db, struct plan *p, struct opts *o)
{
	size_t i;

	if (p->n == 0) {
		upkg_msg(":: nothing to do");
		return 0;
	}
	show_plan(p);
	if (o->dry_run)
		return 0;
	for (i = 0; i < p->n; i++)
		if (fetch_and_verify(cfg, &p->v[i], o) == -1)
			return -1;
	upkg_msg(":: checking for conflicts");
	if (check_conflicts(cfg, db, p) == -1)
		return -1;
	return execute(cfg, db, p);
}

static int
prepare(struct config *cfg, struct db **db)
{
	if (lock_acquire(cfg) == -1)
		return -1;
	if (db_open(cfg, db, 1) == -1)
		return -1;
	if (txn_recover(cfg, *db) == -1) {
		db_close(*db);
		return -1;
	}
	return 0;
}

static int
is_local_archive(const char *t)
{
	size_t n = strlen(t);

	return n > 5 && strcmp(t + n - 5, ".upkg") == 0 && access(t, F_OK) == 0;
}

int
op_install(struct config *cfg, char **targets, int n, struct opts *o)
{
	struct plan p;
	struct db *db;
	int i, rv = -1;

	memset(&p, 0, sizeof(p));
	sl_init(&p.visiting);
	if (prepare(cfg, &db) == -1)
		return -1;
	repo_load_all(cfg);
	upkg_msg(":: resolving dependencies");
	for (i = 0; i < n; i++) {
		if (is_local_archive(targets[i])) {
			struct item proto;

			if (!o->allow_unsigned) {
				upkg_err("%s: local package files carry no "
				    "repository signature; install from a "
				    "repository or pass --allow-unsigned",
				    targets[i]);
				goto out;
			}
			memset(&proto, 0, sizeof(proto));
			if (pkgfile_read_meta(targets[i], &proto.pf) == -1)
				goto out;
			proto.path = xstrdup(targets[i]);
			if (plan_find(&p, proto.pf.meta.name)) {
				upkg_err("%s selected twice", proto.pf.meta.name);
				pkgfile_free(&proto.pf);
				free(proto.path);
				goto out;
			}
			/* m must point at the copy stored in the plan */
			if (add_item(cfg, db, &p, &proto.pf.meta, "local", 1, o,
			    &proto) == -1)
				goto out;
			p.v[p.n - 1].m = &p.v[p.n - 1].pf.meta;
			continue;
		}
		if (visit(cfg, db, &p, targets[i], 1, o) == -1)
			goto out;
	}
	rv = run_plan(cfg, db, &p, o);
out:
	plan_free(&p);
	db_close(db);
	return rv;
}

int
op_upgrade(struct config *cfg, struct opts *o)
{
	struct plan p;
	struct db *db;
	struct instpkg *inst = NULL;
	size_t ninst = 0, i;
	int rv = -1;

	memset(&p, 0, sizeof(p));
	sl_init(&p.visiting);
	if (prepare(cfg, &db) == -1)
		return -1;
	repo_load_all(cfg);
	if (db_list_pkgs(db, &inst, &ninst) == -1)
		goto out;
	upkg_msg(":: checking %zu installed package%s for upgrades", ninst,
	    ninst == 1 ? "" : "s");
	for (i = 0; i < ninst; i++) {
		struct depspec d;
		struct pkgmeta *m;

		if (plan_find(&p, inst[i].name))
			continue;
		memset(&d, 0, sizeof(d));
		d.name = inst[i].name;
		d.op = DEP_ANY;
		if ((m = find_candidate(cfg, &d)) == NULL)
			continue;
		if (fullvercmp(m->version, m->release, inst[i].version,
		    inst[i].release) <= 0)
			continue;
		if (add_item(cfg, db, &p, m, m->repo->name,
		    strcmp(inst[i].reason, "explicit") == 0, o, NULL) == -1)
			goto out;
	}
	rv = run_plan(cfg, db, &p, o);
out:
	for (i = 0; i < ninst; i++)
		instpkg_free(&inst[i]);
	free(inst);
	plan_free(&p);
	db_close(db);
	return rv;
}

static int
remove_set(struct config *cfg, struct db *db, struct strlist *rm,
    struct opts *o)
{
	struct txn *t;
	size_t i;

	upkg_msg(":: %zu package%s to remove:", rm->n, rm->n == 1 ? "" : "s");
	for (i = 0; i < rm->n; i++)
		upkg_msg("   %s", rm->v[i]);
	if (o->dry_run)
		return 0;
	if (db_begin(db) == -1)
		return -1;
	if ((t = txn_begin(cfg, db)) == NULL) {
		db_rollback(db);
		return -1;
	}
	for (i = 0; i < rm->n; i++) {
		upkg_msg(":: removing %s", rm->v[i]);
		if (stage_removal(cfg, db, t, rm->v[i], NULL, rm) == -1 ||
		    db_del_pkg(db, rm->v[i]) == -1) {
			txn_abort(t);
			return -1;
		}
	}
	crashpoint("after-stage");
	if (upkg_interrupted) {
		upkg_err("interrupted; rolling back");
		txn_abort(t);
		return -1;
	}
	return txn_commit(t);
}

static int
depends_on(const struct instpkg *ip, const char *name)
{
	size_t j;

	for (j = 0; j < ip->depends.n; j++) {
		struct depspec d;
		int hit;

		if (depspec_parse(ip->depends.v[j], &d, 0) == -1)
			continue;
		hit = strcmp(d.name, name) == 0;
		depspec_free(&d);
		if (hit)
			return 1;
	}
	return 0;
}

int
op_remove(struct config *cfg, char **targets, int n, struct opts *o)
{
	struct db *db;
	struct instpkg *inst = NULL;
	struct strlist rm;
	size_t ninst = 0, i, k;
	int bad = 0, rv = -1, changed;

	sl_init(&rm);
	if (prepare(cfg, &db) == -1)
		return -1;
	if (db_list_pkgs(db, &inst, &ninst) == -1)
		goto out;
	for (i = 0; i < (size_t)n; i++) {
		for (k = 0; k < ninst; k++)
			if (strcmp(inst[k].name, targets[i]) == 0)
				break;
		if (k == ninst) {
			upkg_err("%s is not installed", targets[i]);
			bad = 1;
		} else if (!sl_contains(&rm, targets[i]))
			sl_add(&rm, targets[i]);
	}
	if (bad)
		goto out;
	/* dependency-aware: refuse, or with -c remove dependents too */
	do {
		changed = 0;
		for (k = 0; k < ninst; k++) {
			if (sl_contains(&rm, inst[k].name))
				continue;
			for (i = 0; i < rm.n; i++) {
				if (!depends_on(&inst[k], rm.v[i]))
					continue;
				if (o->cascade) {
					sl_add(&rm, inst[k].name);
					changed = 1;
				} else {
					upkg_err("%s is required by %s",
					    rm.v[i], inst[k].name);
					bad = 1;
				}
				break;
			}
		}
	} while (changed);
	if (bad) {
		upkg_err("remove the dependent packages first, or use -c "
		    "to remove them as well");
		goto out;
	}
	rv = remove_set(cfg, db, &rm, o);
out:
	for (k = 0; k < ninst; k++)
		instpkg_free(&inst[k]);
	free(inst);
	sl_free(&rm);
	db_close(db);
	return rv;
}

int
op_autoremove(struct config *cfg, struct opts *o)
{
	struct db *db;
	struct instpkg *inst = NULL;
	struct strlist rm;
	size_t ninst = 0, i, k;
	int changed, rv = -1;

	sl_init(&rm);
	if (prepare(cfg, &db) == -1)
		return -1;
	if (db_list_pkgs(db, &inst, &ninst) == -1)
		goto out;
	do {
		changed = 0;
		for (k = 0; k < ninst; k++) {
			int needed = 0;

			if (strcmp(inst[k].reason, "explicit") == 0 ||
			    sl_contains(&rm, inst[k].name))
				continue;
			for (i = 0; i < ninst && !needed; i++)
				if (!sl_contains(&rm, inst[i].name) &&
				    depends_on(&inst[i], inst[k].name))
					needed = 1;
			if (!needed) {
				sl_add(&rm, inst[k].name);
				changed = 1;
			}
		}
	} while (changed);
	if (rm.n == 0) {
		upkg_msg(":: no orphaned dependencies");
		rv = 0;
	} else
		rv = remove_set(cfg, db, &rm, o);
out:
	for (k = 0; k < ninst; k++)
		instpkg_free(&inst[k]);
	free(inst);
	sl_free(&rm);
	db_close(db);
	return rv;
}
