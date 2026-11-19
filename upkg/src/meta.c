/*
 * upkg -- package metadata (+PKGINFO), manifests (+MANIFEST) and path
 * safety rules.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * +PKGINFO is a line-oriented "key value" text file (see docs/packages.md).
 * +MANIFEST is one tab-separated record per filesystem object:
 *
 *	f <TAB> mode <TAB> size <TAB> sha256 <TAB> path
 *	d <TAB> mode <TAB> path
 *	l <TAB> target <TAB> path
 *
 * Paths are relative to the installation root, normalised, and may not
 * contain control characters.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upkg.h"

void
pkgmeta_free(struct pkgmeta *m)
{
	free(m->name);
	free(m->version);
	free(m->arch);
	free(m->description);
	free(m->license);
	free(m->maintainer);
	free(m->origin);
	free(m->filename);
	free(m->sha256);
	sl_free(&m->depends);
	sl_free(&m->conflicts);
	memset(m, 0, sizeof(*m));
}

static int
set_once(char **dst, const char *v, const char *key, const char *what, int line)
{
	if (*dst != NULL) {
		upkg_err("%s:%d: duplicate key '%s'", what, line, key);
		return -1;
	}
	*dst = xstrdup(v);
	return 0;
}

/*
 * Parse one metadata stanza.  Unknown keys are rejected so that a newer,
 * incompatible schema is never silently misread; the "format" key gates
 * schema evolution.
 */
int
pkgmeta_parse(const char *text, struct pkgmeta *m, const char *what)
{
	char *buf = xstrdup(text), *line, *next, *key, *val;
	int lineno = 0, rv = -1, have_release = 0;

	memset(m, 0, sizeof(*m));
	sl_init(&m->depends);
	sl_init(&m->conflicts);
	m->size = -1;
	for (line = buf; line != NULL; line = next) {
		lineno++;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		line = trim(line);
		if (*line == '\0' || *line == '#')
			continue;
		key = line;
		for (val = key; *val && !isspace((unsigned char)*val); val++)
			continue;
		if (*val)
			*val++ = '\0';
		val = trim(val);
		for (const char *p = val; *p; p++)
			if (iscntrl((unsigned char)*p) && *p != '\t') {
				upkg_err("%s:%d: control character in value",
				    what, lineno);
				goto out;
			}

		if (strcmp(key, "format") == 0) {
			if (strtol(val, NULL, 10) != UPKG_PKG_FORMAT) {
				upkg_err("%s: unsupported metadata format '%s'",
				    what, val);
				goto out;
			}
		} else if (strcmp(key, "name") == 0) {
			if (set_once(&m->name, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "version") == 0) {
			if (set_once(&m->version, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "release") == 0) {
			char *e;
			unsigned long r;

			errno = 0;
			r = strtoul(val, &e, 10);
			if (*val == '\0' || *e != '\0' || errno || r > 1000000) {
				upkg_err("%s:%d: bad release '%s'", what,
				    lineno, val);
				goto out;
			}
			m->release = (unsigned)r;
			have_release = 1;
		} else if (strcmp(key, "arch") == 0) {
			if (set_once(&m->arch, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "description") == 0) {
			if (set_once(&m->description, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "license") == 0) {
			if (set_once(&m->license, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "maintainer") == 0) {
			if (set_once(&m->maintainer, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "origin") == 0) {
			if (set_once(&m->origin, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "depend") == 0) {
			sl_add(&m->depends, val);
		} else if (strcmp(key, "conflict") == 0) {
			sl_add(&m->conflicts, val);
		} else if (strcmp(key, "filename") == 0) {
			if (set_once(&m->filename, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "sha256") == 0) {
			if (set_once(&m->sha256, val, key, what, lineno))
				goto out;
		} else if (strcmp(key, "size") == 0) {
			char *e;

			m->size = strtoll(val, &e, 10);
			if (*val == '\0' || *e != '\0' || m->size < 0) {
				upkg_err("%s:%d: bad size", what, lineno);
				goto out;
			}
		} else {
			upkg_err("%s:%d: unknown key '%s'", what, lineno, key);
			goto out;
		}
	}
	if (!have_release)
		m->release = 1;
	rv = 0;
out:
	free(buf);
	if (rv)
		pkgmeta_free(m);
	return rv;
}

int
pkgmeta_validate(const struct pkgmeta *m, const char *what)
{
	struct depspec d;
	size_t i;

	if (m->name == NULL || !valid_pkgname(m->name)) {
		upkg_err("%s: missing or invalid package name", what);
		return -1;
	}
	if (m->version == NULL || !valid_version(m->version)) {
		upkg_err("%s: missing or invalid version", what);
		return -1;
	}
	if (m->arch == NULL || !valid_pkgname(m->arch)) {
		upkg_err("%s: missing or invalid arch", what);
		return -1;
	}
	for (i = 0; i < m->depends.n; i++) {
		if (depspec_parse(m->depends.v[i], &d, 0) == -1) {
			upkg_err("%s: invalid dependency '%s'", what,
			    m->depends.v[i]);
			return -1;
		}
		depspec_free(&d);
	}
	for (i = 0; i < m->conflicts.n; i++) {
		if (depspec_parse(m->conflicts.v[i], &d, 0) == -1) {
			upkg_err("%s: invalid conflict '%s'", what,
			    m->conflicts.v[i]);
			return -1;
		}
		depspec_free(&d);
	}
	return 0;
}

static void
appendkv(char **buf, size_t *len, const char *key, const char *val)
{
	size_t kn = strlen(key), vn = strlen(val);

	*buf = xrealloc(*buf, *len + kn + vn + 3);
	memcpy(*buf + *len, key, kn);
	(*buf)[*len + kn] = ' ';
	memcpy(*buf + *len + kn + 1, val, vn);
	(*buf)[*len + kn + 1 + vn] = '\n';
	*len += kn + vn + 2;
	(*buf)[*len] = '\0';
}

char *
pkgmeta_format(const struct pkgmeta *m, int idx)
{
	char *buf = xstrdup(""), num[32];
	size_t len = 0, i;

	if (!idx) {
		snprintf(num, sizeof(num), "%d", UPKG_PKG_FORMAT);
		appendkv(&buf, &len, "format", num);
	}
	appendkv(&buf, &len, "name", m->name);
	appendkv(&buf, &len, "version", m->version);
	snprintf(num, sizeof(num), "%u", m->release);
	appendkv(&buf, &len, "release", num);
	appendkv(&buf, &len, "arch", m->arch);
	if (m->description)
		appendkv(&buf, &len, "description", m->description);
	if (m->license)
		appendkv(&buf, &len, "license", m->license);
	if (m->maintainer)
		appendkv(&buf, &len, "maintainer", m->maintainer);
	if (m->origin)
		appendkv(&buf, &len, "origin", m->origin);
	for (i = 0; i < m->depends.n; i++)
		appendkv(&buf, &len, "depend", m->depends.v[i]);
	for (i = 0; i < m->conflicts.n; i++)
		appendkv(&buf, &len, "conflict", m->conflicts.v[i]);
	if (idx) {
		appendkv(&buf, &len, "filename", m->filename);
		snprintf(num, sizeof(num), "%lld", m->size);
		appendkv(&buf, &len, "size", num);
		appendkv(&buf, &len, "sha256", m->sha256);
	}
	return buf;
}

char *
pkgmeta_fullver(const struct pkgmeta *m)
{
	return xasprintf("%s-%u", m->version, m->release);
}

char *
pkgmeta_filename(const struct pkgmeta *m)
{
	return xasprintf("%s-%s-%u-%s.upkg", m->name, m->version, m->release,
	    m->arch);
}

/* ------------------------------------------------------------ manifests */

void
manifest_free(struct manifest *mf)
{
	size_t i;

	for (i = 0; i < mf->n; i++) {
		free(mf->e[i].path);
		free(mf->e[i].sha256);
		free(mf->e[i].target);
	}
	free(mf->e);
	memset(mf, 0, sizeof(*mf));
}

int
manifest_add(struct manifest *mf, enum mtype t, const char *path, mode_t mode,
    long long size, const char *sha256, const char *target)
{
	struct mentry *e;

	if (mf->n == mf->cap) {
		mf->cap = mf->cap ? mf->cap * 2 : 64;
		mf->e = xrealloc(mf->e, mf->cap * sizeof(*mf->e));
	}
	e = &mf->e[mf->n++];
	memset(e, 0, sizeof(*e));
	e->type = t;
	e->path = xstrdup(path);
	e->mode = mode;
	e->size = size;
	e->sha256 = sha256 ? xstrdup(sha256) : NULL;
	e->target = target ? xstrdup(target) : NULL;
	return 0;
}

/*
 * A safe relative path: non-empty, not absolute, no empty, "." or ".."
 * components, no control characters, and no component beginning with '+'
 * at the top level (reserved for package metadata).
 */
int
safe_relpath(const char *p)
{
	const char *c, *s;

	if (*p == '\0' || *p == '/' || *p == '+' || strlen(p) > 1024)
		return 0;
	for (s = p; *s; s++)
		if (iscntrl((unsigned char)*s))
			return 0;
	if (strstr(p, UPKG_STAGE_SUFFIX) != NULL)
		return 0;
	for (c = p; ; ) {
		const char *e = strchr(c, '/');
		size_t n = e ? (size_t)(e - c) : strlen(c);

		if (n == 0)
			return 0;
		if (n == 1 && c[0] == '.')
			return 0;
		if (n == 2 && c[0] == '.' && c[1] == '.')
			return 0;
		if (!e)
			break;
		c = e + 1;
	}
	return 1;
}

int
path_in_prefixes(const char *path, const struct strlist *pfx)
{
	size_t i, n;

	for (i = 0; i < pfx->n; i++) {
		n = strlen(pfx->v[i]);
		if (strncmp(path, pfx->v[i], n) == 0 &&
		    (path[n] == '\0' || path[n] == '/'))
			return 1;
		/* parent directories of a prefix may be listed as 'd' */
		if (strncmp(pfx->v[i], path, strlen(path)) == 0 &&
		    pfx->v[i][strlen(path)] == '/')
			return 2;
	}
	return 0;
}

/*
 * Symbolic link targets must be relative and, resolved lexically from the
 * link's directory, stay inside an allowed prefix.
 */
int
link_target_ok(const char *linkpath, const char *target,
    const struct strlist *pfx)
{
	char *buf, *parts[512];
	size_t np = 0, i;
	char *dir, *p, *tok, *save = NULL;
	const char *c;
	int ok;

	if (*target == '\0' || *target == '/' || strlen(target) > 1024)
		return 0;
	for (c = target; *c; c++)
		if (iscntrl((unsigned char)*c))
			return 0;
	dir = xstrdup(linkpath);
	if ((p = strrchr(dir, '/')) != NULL)
		*p = '\0';
	else
		*dir = '\0';
	buf = xasprintf("%s/%s", dir, target);
	free(dir);
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (strcmp(tok, ".") == 0)
			continue;
		if (strcmp(tok, "..") == 0) {
			if (np == 0) {
				free(buf);
				return 0;
			}
			np--;
			continue;
		}
		if (np == sizeof(parts) / sizeof(parts[0])) {
			free(buf);
			return 0;
		}
		parts[np++] = tok;
	}
	if (np == 0) {
		free(buf);
		return 0;
	}
	size_t len = 0;
	for (i = 0; i < np; i++)
		len += strlen(parts[i]) + 1;
	char *res = xmalloc(len + 1);
	*res = '\0';
	for (i = 0; i < np; i++) {
		if (i)
			strcat(res, "/");
		strcat(res, parts[i]);
	}
	ok = path_in_prefixes(res, pfx) == 1;
	free(res);
	free(buf);
	return ok;
}

int
manifest_parse(const char *text, struct manifest *mf)
{
	char *buf = xstrdup(text), *line, *next, *f[6];
	int lineno = 0, nf, rv = -1;

	memset(mf, 0, sizeof(*mf));
	for (line = buf; line && *line; line = next) {
		lineno++;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (*line == '\0')
			continue;
		nf = 0;
		f[nf++] = line;
		for (char *p = line; *p && nf < 6; p++)
			if (*p == '\t') {
				*p = '\0';
				f[nf++] = p + 1;
			}
		if (strlen(f[0]) != 1)
			goto bad;
		char *e;
		switch (f[0][0]) {
		case 'f': {
			if (nf != 5)
				goto bad;
			unsigned long mode = strtoul(f[1], &e, 8);
			if (*e || mode > 07777)
				goto bad;
			long long size = strtoll(f[2], &e, 10);
			if (*e || size < 0)
				goto bad;
			if (!is_hex(f[3], SHA256_HEX_LEN))
				goto bad;
			manifest_add(mf, M_FILE, f[4], (mode_t)mode, size, f[3], NULL);
			break;
		}
		case 'd': {
			if (nf != 3)
				goto bad;
			unsigned long mode = strtoul(f[1], &e, 8);
			if (*e || mode > 07777)
				goto bad;
			manifest_add(mf, M_DIR, f[2], (mode_t)mode, 0, NULL, NULL);
			break;
		}
		case 'l':
			if (nf != 3)
				goto bad;
			manifest_add(mf, M_LINK, f[2], 0777, 0, NULL, f[1]);
			break;
		default:
			goto bad;
		}
	}
	rv = 0;
	goto out;
bad:
	upkg_err("+MANIFEST:%d: malformed record", lineno);
	manifest_free(mf);
out:
	free(buf);
	return rv;
}

static int
mentry_cmp(const void *a, const void *b)
{
	return strcmp(((const struct mentry *)a)->path,
	    ((const struct mentry *)b)->path);
}

void
manifest_sort(struct manifest *mf)
{
	if (mf->n)
		qsort(mf->e, mf->n, sizeof(*mf->e), mentry_cmp);
}

struct mentry *
manifest_find(struct manifest *mf, const char *path)
{
	size_t lo = 0, hi = mf->n;

	/* binary search; the manifest is sorted by manifest_sort() */
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(path, mf->e[mid].path);

		if (c == 0)
			return &mf->e[mid];
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

char *
manifest_format(const struct manifest *mf)
{
	size_t cap = 256, len = 0, i;
	char *buf = xmalloc(cap), *line;

	*buf = '\0';
	for (i = 0; i < mf->n; i++) {
		const struct mentry *e = &mf->e[i];

		switch (e->type) {
		case M_FILE:
			line = xasprintf("f\t%04o\t%lld\t%s\t%s\n",
			    (unsigned)e->mode, e->size, e->sha256, e->path);
			break;
		case M_DIR:
			line = xasprintf("d\t%04o\t%s\n", (unsigned)e->mode,
			    e->path);
			break;
		default:
			line = xasprintf("l\t%s\t%s\n", e->target, e->path);
			break;
		}
		size_t n = strlen(line);
		while (len + n + 1 > cap)
			cap *= 2;
		buf = xrealloc(buf, cap);
		memcpy(buf + len, line, n + 1);
		len += n;
		free(line);
	}
	return buf;
}
