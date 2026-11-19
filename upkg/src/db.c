/*
 * upkg -- installed package database (SQLite), /var/lib/upkg/upkg.db.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "upkg.h"

struct db {
	sqlite3 *h;
};

static const char schema[] =
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS packages("
    "  name TEXT PRIMARY KEY, version TEXT NOT NULL, release INTEGER NOT NULL,"
    "  arch TEXT NOT NULL, repo TEXT, reason TEXT NOT NULL,"
    "  description TEXT, installed INTEGER NOT NULL, pkginfo TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS files("
    "  path TEXT PRIMARY KEY, pkg TEXT NOT NULL, type TEXT NOT NULL,"
    "  mode INTEGER, size INTEGER, sha256 TEXT, target TEXT);"
    "CREATE INDEX IF NOT EXISTS files_pkg ON files(pkg);"
    "CREATE TABLE IF NOT EXISTS dirs("
    "  path TEXT NOT NULL, pkg TEXT NOT NULL, mode INTEGER,"
    "  PRIMARY KEY(path, pkg));"
    "CREATE INDEX IF NOT EXISTS dirs_pkg ON dirs(pkg);"
    "CREATE TABLE IF NOT EXISTS deps(pkg TEXT NOT NULL, spec TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS deps_pkg ON deps(pkg);"
    "CREATE TABLE IF NOT EXISTS txn(id TEXT PRIMARY KEY);";

static int
exec(struct db *d, const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(d->h, sql, NULL, NULL, &err) != SQLITE_OK) {
		upkg_err("database: %s", err ? err : "error");
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

static sqlite3_stmt *
prep(struct db *d, const char *sql)
{
	sqlite3_stmt *s = NULL;

	if (sqlite3_prepare_v2(d->h, sql, -1, &s, NULL) != SQLITE_OK) {
		upkg_err("database: %s", sqlite3_errmsg(d->h));
		return NULL;
	}
	return s;
}

static char *
col_str(sqlite3_stmt *s, int i)
{
	const unsigned char *t = sqlite3_column_text(s, i);

	return t ? xstrdup((const char *)t) : NULL;
}

int
db_open(struct config *cfg, struct db **out, int create)
{
	struct db *d = xcalloc(1, sizeof(*d));
	char *path = path_join(cfg->dbdir, "upkg.db");
	int flags = SQLITE_OPEN_READWRITE;

	if (create) {
		if (mkdir_p(cfg->dbdir, 0755) == -1) {
			free(path);
			free(d);
			return -1;
		}
		flags |= SQLITE_OPEN_CREATE;
	}
	if (sqlite3_open_v2(path, &d->h, flags, NULL) != SQLITE_OK) {
		upkg_err("cannot open database %s: %s", path,
		    d->h ? sqlite3_errmsg(d->h) : "error");
		sqlite3_close(d->h);
		free(path);
		free(d);
		return -1;
	}
	free(path);
	sqlite3_busy_timeout(d->h, 5000);
	/* rollback journal + FULL sync: durable across power loss */
	if (exec(d, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
	    "PRAGMA foreign_keys=ON;") == -1 || exec(d, schema) == -1) {
		db_close(d);
		return -1;
	}
	{
		char *q = xasprintf("INSERT OR IGNORE INTO meta VALUES"
		    "('schema', '%d');", UPKG_DB_SCHEMA);
		int rv = exec(d, q);

		free(q);
		if (rv == -1) {
			db_close(d);
			return -1;
		}
	}
	*out = d;
	return 0;
}

void
db_close(struct db *d)
{
	if (d) {
		sqlite3_close(d->h);
		free(d);
	}
}

int db_begin(struct db *d) { return exec(d, "BEGIN IMMEDIATE;"); }
int db_commit(struct db *d) { return exec(d, "COMMIT;"); }
int
db_rollback(struct db *d)
{
	if (sqlite3_get_autocommit(d->h))
		return 0;	/* no transaction open */
	return exec(d, "ROLLBACK;");
}

void
instpkg_free(struct instpkg *p)
{
	free(p->name);
	free(p->version);
	free(p->arch);
	free(p->repo);
	free(p->reason);
	free(p->description);
	sl_free(&p->depends);
	sl_free(&p->conflicts);
	memset(p, 0, sizeof(*p));
}

static int
load_deps(struct db *d, struct instpkg *p)
{
	sqlite3_stmt *s = prep(d, "SELECT spec FROM deps WHERE pkg = ?1 "
	    "ORDER BY rowid;");

	sl_init(&p->depends);
	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, p->name, -1, SQLITE_STATIC);
	while (sqlite3_step(s) == SQLITE_ROW)
		sl_addn(&p->depends, col_str(s, 0));
	sqlite3_finalize(s);
	return 0;
}

static void
row_to_pkg(sqlite3_stmt *s, struct instpkg *p)
{
	memset(p, 0, sizeof(*p));
	p->name = col_str(s, 0);
	p->version = col_str(s, 1);
	p->release = (unsigned)sqlite3_column_int(s, 2);
	p->arch = col_str(s, 3);
	p->repo = col_str(s, 4);
	p->reason = col_str(s, 5);
	p->description = col_str(s, 6);
	p->installed = sqlite3_column_int64(s, 7);
	sl_init(&p->conflicts);
	{
		const char *info = (const char *)sqlite3_column_text(s, 8);
		struct pkgmeta m;

		if (info && pkgmeta_parse(info, &m, p->name) == 0) {
			p->conflicts = m.conflicts;
			sl_init(&m.conflicts);
			pkgmeta_free(&m);
		}
	}
}

#define PKGCOLS "name, version, release, arch, repo, reason, description, installed, pkginfo"

/* returns 1 if found, 0 if not, -1 on error */
int
db_get_pkg(struct db *d, const char *name, struct instpkg *p)
{
	sqlite3_stmt *s = prep(d, "SELECT " PKGCOLS " FROM packages "
	    "WHERE name = ?1;");
	int rc;

	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
	rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		row_to_pkg(s, p);
		sqlite3_finalize(s);
		return load_deps(d, p) == 0 ? 1 : -1;
	}
	sqlite3_finalize(s);
	return rc == SQLITE_DONE ? 0 : -1;
}

int
db_list_pkgs(struct db *d, struct instpkg **out, size_t *n)
{
	sqlite3_stmt *s = prep(d, "SELECT " PKGCOLS " FROM packages "
	    "ORDER BY name;");
	struct instpkg *v = NULL;
	size_t c = 0, i;

	if (s == NULL)
		return -1;
	while (sqlite3_step(s) == SQLITE_ROW) {
		v = xrealloc(v, (c + 1) * sizeof(*v));
		row_to_pkg(s, &v[c++]);
	}
	sqlite3_finalize(s);
	for (i = 0; i < c; i++)
		load_deps(d, &v[i]);
	*out = v;
	*n = c;
	return 0;
}

int
db_file_owner(struct db *d, const char *path, char **owner)
{
	sqlite3_stmt *s = prep(d, "SELECT pkg FROM files WHERE path = ?1;");
	int rc;

	*owner = NULL;
	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_step(s);
	if (rc == SQLITE_ROW)
		*owner = col_str(s, 0);
	sqlite3_finalize(s);
	return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int
db_pkg_files(struct db *d, const char *pkg, struct manifest *mf)
{
	sqlite3_stmt *s = prep(d, "SELECT path, type, mode, size, sha256, "
	    "target FROM files WHERE pkg = ?1 ORDER BY path;");

	memset(mf, 0, sizeof(*mf));
	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, pkg, -1, SQLITE_STATIC);
	while (sqlite3_step(s) == SQLITE_ROW) {
		const char *t = (const char *)sqlite3_column_text(s, 1);

		manifest_add(mf, t && *t == 'l' ? M_LINK : M_FILE,
		    (const char *)sqlite3_column_text(s, 0),
		    (mode_t)sqlite3_column_int(s, 2),
		    sqlite3_column_int64(s, 3),
		    (const char *)sqlite3_column_text(s, 4),
		    (const char *)sqlite3_column_text(s, 5));
	}
	sqlite3_finalize(s);
	return 0;
}

int
db_pkg_dirs(struct db *d, const char *pkg, struct strlist *sl)
{
	sqlite3_stmt *s = prep(d, "SELECT path FROM dirs WHERE pkg = ?1 "
	    "ORDER BY path DESC;");

	sl_init(sl);
	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, pkg, -1, SQLITE_STATIC);
	while (sqlite3_step(s) == SQLITE_ROW)
		sl_addn(sl, col_str(s, 0));
	sqlite3_finalize(s);
	return 0;
}

/* packages owning directory `path' */
int
db_dir_owners(struct db *d, const char *path, struct strlist *sl)
{
	sqlite3_stmt *s = prep(d, "SELECT pkg FROM dirs WHERE path = ?1;");

	sl_init(sl);
	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
	while (sqlite3_step(s) == SQLITE_ROW)
		sl_addn(sl, col_str(s, 0));
	sqlite3_finalize(s);
	return 0;
}

int
db_del_pkg(struct db *d, const char *name)
{
	static const char *q[] = {
		"DELETE FROM packages WHERE name = ?1;",
		"DELETE FROM files WHERE pkg = ?1;",
		"DELETE FROM dirs WHERE pkg = ?1;",
		"DELETE FROM deps WHERE pkg = ?1;",
	};
	size_t i;

	for (i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		sqlite3_stmt *s = prep(d, q[i]);

		if (s == NULL)
			return -1;
		sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
		if (sqlite3_step(s) != SQLITE_DONE) {
			upkg_err("database: %s", sqlite3_errmsg(d->h));
			sqlite3_finalize(s);
			return -1;
		}
		sqlite3_finalize(s);
	}
	return 0;
}

int
db_add_pkg(struct db *d, const struct pkgmeta *m, const struct manifest *mf,
    const char *repo, const char *reason)
{
	sqlite3_stmt *s;
	char *info = pkgmeta_format(m, 0);
	size_t i;
	int rv = -1;

	s = prep(d, "INSERT INTO packages VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);");
	if (s == NULL)
		goto out;
	sqlite3_bind_text(s, 1, m->name, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 2, m->version, -1, SQLITE_STATIC);
	sqlite3_bind_int(s, 3, (int)m->release);
	sqlite3_bind_text(s, 4, m->arch, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 5, repo, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 6, reason, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 7, m->description, -1, SQLITE_STATIC);
	sqlite3_bind_int64(s, 8, (sqlite3_int64)time(NULL));
	sqlite3_bind_text(s, 9, info, -1, SQLITE_STATIC);
	if (sqlite3_step(s) != SQLITE_DONE)
		goto dberr;
	sqlite3_finalize(s);

	if ((s = prep(d, "INSERT INTO deps VALUES(?1, ?2);")) == NULL)
		goto out;
	for (i = 0; i < m->depends.n; i++) {
		sqlite3_reset(s);
		sqlite3_bind_text(s, 1, m->name, -1, SQLITE_STATIC);
		sqlite3_bind_text(s, 2, m->depends.v[i], -1, SQLITE_STATIC);
		if (sqlite3_step(s) != SQLITE_DONE)
			goto dberr;
	}
	sqlite3_finalize(s);

	if ((s = prep(d, "INSERT INTO files VALUES(?1,?2,?3,?4,?5,?6,?7);")) == NULL)
		goto out;
	for (i = 0; i < mf->n; i++) {
		const struct mentry *e = &mf->e[i];

		if (e->type == M_DIR)
			continue;
		sqlite3_reset(s);
		sqlite3_bind_text(s, 1, e->path, -1, SQLITE_STATIC);
		sqlite3_bind_text(s, 2, m->name, -1, SQLITE_STATIC);
		sqlite3_bind_text(s, 3, e->type == M_FILE ? "f" : "l", -1,
		    SQLITE_STATIC);
		sqlite3_bind_int(s, 4, (int)e->mode);
		sqlite3_bind_int64(s, 5, e->size);
		if (e->sha256)
			sqlite3_bind_text(s, 6, e->sha256, -1, SQLITE_STATIC);
		else
			sqlite3_bind_null(s, 6);
		if (e->target)
			sqlite3_bind_text(s, 7, e->target, -1, SQLITE_STATIC);
		else
			sqlite3_bind_null(s, 7);
		if (sqlite3_step(s) != SQLITE_DONE)
			goto dberr;
	}
	sqlite3_finalize(s);

	if ((s = prep(d, "INSERT INTO dirs VALUES(?1, ?2, ?3);")) == NULL)
		goto out;
	for (i = 0; i < mf->n; i++) {
		const struct mentry *e = &mf->e[i];

		if (e->type != M_DIR)
			continue;
		sqlite3_reset(s);
		sqlite3_bind_text(s, 1, e->path, -1, SQLITE_STATIC);
		sqlite3_bind_text(s, 2, m->name, -1, SQLITE_STATIC);
		sqlite3_bind_int(s, 3, (int)e->mode);
		if (sqlite3_step(s) != SQLITE_DONE)
			goto dberr;
	}
	sqlite3_finalize(s);
	rv = 0;
	goto out;
dberr:
	upkg_err("database: %s", sqlite3_errmsg(d->h));
	sqlite3_finalize(s);
out:
	free(info);
	return rv;
}

int
db_set_reason(struct db *d, const char *name, const char *reason)
{
	sqlite3_stmt *s = prep(d, "UPDATE packages SET reason = ?2 "
	    "WHERE name = ?1;");
	int rc;

	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 2, reason, -1, SQLITE_STATIC);
	rc = sqlite3_step(s);
	sqlite3_finalize(s);
	return rc == SQLITE_DONE ? 0 : -1;
}

static int
txn_stmt(struct db *d, const char *sql, const char *id)
{
	sqlite3_stmt *s = prep(d, sql);
	int rc;

	if (s == NULL)
		return -1;
	sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
	rc = sqlite3_step(s);
	sqlite3_finalize(s);
	if (rc == SQLITE_ROW)
		return 1;
	if (rc == SQLITE_DONE)
		return 0;
	upkg_err("database: %s", sqlite3_errmsg(d->h));
	return -1;
}

int
db_txn_mark(struct db *d, const char *id)
{
	return txn_stmt(d, "INSERT INTO txn VALUES(?1);", id) == -1 ? -1 : 0;
}

int
db_txn_exists(struct db *d, const char *id)
{
	return txn_stmt(d, "SELECT id FROM txn WHERE id = ?1;", id);
}

int
db_txn_clear(struct db *d, const char *id)
{
	return txn_stmt(d, "DELETE FROM txn WHERE id = ?1;", id) == -1 ? -1 : 0;
}
