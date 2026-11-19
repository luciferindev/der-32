/*
 * upkg -- repositories: signed indexes, synchronisation and publication.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A repository is a directory (local or served over HTTPS) holding:
 *
 *	INDEX		package index (see docs/packages.md)
 *	INDEX.sig	Ed25519 signature over INDEX
 *	*.upkg		package archives
 *
 * The INDEX header names the repository and carries a timestamp; upkg
 * refuses an index for a different repository (substitution) or one older
 * than the copy it already has (rollback/replay).
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "upkg.h"

static char *
sync_path(const struct config *cfg, const struct repo *r, const char *suffix)
{
	return xasprintf("%s/sync/%s.%s", cfg->dbdir, r->name, suffix);
}

static int
valid_filename(const char *f)
{
	size_t n = strlen(f);

	if (n < 6 || strcmp(f + n - 5, ".upkg") != 0 || strchr(f, '/') ||
	    f[0] == '.')
		return 0;
	return safe_relpath(f);
}

/*
 * Parse an INDEX.  Packages are appended to r->pkgs.
 */
int
index_parse(const char *text, struct repo *r)
{
	const char *p = text, *e;
	int stanza = 0, rv = -1;
	char *hdr_repo = NULL;

	r->timestamp = -1;
	while (*p) {
		while (*p == '\n')
			p++;
		if (!*p)
			break;
		e = strstr(p, "\n\n");
		char *st = e ? xstrndup(p, (size_t)(e - p)) : xstrdup(p);
		p = e ? e + 2 : p + strlen(p);
		stanza++;
		if (stanza == 1) {
			char *save = NULL, *line;
			int fmt = 0;

			for (line = strtok_r(st, "\n", &save); line;
			    line = strtok_r(NULL, "\n", &save)) {
				char *k = trim(line), *v = k;

				while (*v && *v != ' ')
					v++;
				if (*v)
					*v++ = '\0';
				v = trim(v);
				if (strcmp(k, "upkg-index") == 0)
					fmt = atoi(v);
				else if (strcmp(k, "repo") == 0) {
					free(hdr_repo);
					hdr_repo = xstrdup(v);
				} else if (strcmp(k, "timestamp") == 0)
					r->timestamp = strtoll(v, NULL, 10);
			}
			free(st);
			if (fmt != UPKG_INDEX_FORMAT) {
				upkg_err("%s: unsupported index format", r->name);
				goto out;
			}
			if (hdr_repo == NULL || strcmp(hdr_repo, r->name) != 0) {
				upkg_err("%s: index belongs to repository '%s'",
				    r->name, hdr_repo ? hdr_repo : "(none)");
				goto out;
			}
			if (r->timestamp < 0) {
				upkg_err("%s: index has no timestamp", r->name);
				goto out;
			}
			continue;
		}
		struct pkgmeta m;
		char *what = xasprintf("%s/INDEX stanza %d", r->name, stanza);
		int bad = pkgmeta_parse(st, &m, what) || pkgmeta_validate(&m, what);
		free(st);
		if (!bad && (m.filename == NULL || !valid_filename(m.filename) ||
		    m.sha256 == NULL || !is_hex(m.sha256, SHA256_HEX_LEN) ||
		    m.size < 0)) {
			upkg_err("%s: missing or invalid filename/size/sha256",
			    what);
			pkgmeta_free(&m);
			bad = 1;
		}
		free(what);
		if (bad)
			goto out;
		m.repo = r;
		r->pkgs = xrealloc(r->pkgs, (r->npkgs + 1) * sizeof(*r->pkgs));
		r->pkgs[r->npkgs++] = m;
	}
	if (stanza == 0) {
		upkg_err("%s: empty index", r->name);
		goto out;
	}
	rv = 0;
out:
	free(hdr_repo);
	return rv;
}

static void
repo_clear(struct repo *r)
{
	size_t i;

	for (i = 0; i < r->npkgs; i++)
		pkgmeta_free(&r->pkgs[i]);
	free(r->pkgs);
	r->pkgs = NULL;
	r->npkgs = 0;
	r->loaded = 0;
}

static int
verify_index(struct config *cfg, struct repo *r, const char *text, size_t len,
    const char *sig)
{
	char *signer = NULL;

	if (sig_verify_dir(cfg->keydir, r->key, text, len, sig, &signer) == -1) {
		upkg_err("%s: index signature verification FAILED", r->name);
		return -1;
	}
	upkg_debug("%s: index signed by key '%s'", r->name, signer);
	free(signer);
	return 0;
}

int
repo_sync(struct config *cfg, struct repo *r)
{
	char *dir = path_join(cfg->dbdir, "sync");
	char *idx_url = xasprintf("%s/INDEX", r->url);
	char *sig_url = xasprintf("%s/INDEX.sig", r->url);
	char *idx_tmp = sync_path(cfg, r, "INDEX.new");
	char *sig_tmp = sync_path(cfg, r, "INDEX.sig.new");
	char *idx = sync_path(cfg, r, "INDEX");
	char *sig = sync_path(cfg, r, "INDEX.sig");
	char *text = NULL, *sigtext = NULL;
	size_t len;
	long long oldts = -1;
	struct repo probe;
	int rv = -1;

	if (mkdir_p(dir, 0755) == -1)
		goto out;
	if (fetch_url(cfg, idx_url, idx_tmp) == -1 ||
	    fetch_url(cfg, sig_url, sig_tmp) == -1)
		goto out;
	if (read_file(idx_tmp, &text, &len) == -1 ||
	    read_file(sig_tmp, &sigtext, NULL) == -1) {
		upkg_err("%s: cannot read downloaded index", r->name);
		goto out;
	}
	if (verify_index(cfg, r, text, len, sigtext) == -1)
		goto out;

	memset(&probe, 0, sizeof(probe));
	probe.name = r->name;
	if (index_parse(text, &probe) == -1) {
		repo_clear(&probe);
		goto out;
	}
	if (r->loaded || (access(idx, F_OK) == 0 && repo_load(cfg, r) == 0))
		oldts = r->timestamp;
	if (oldts > probe.timestamp) {
		upkg_err("%s: refusing index older than the current one "
		    "(%lld < %lld): possible replay attack", r->name,
		    probe.timestamp, oldts);
		repo_clear(&probe);
		goto out;
	}
	upkg_msg(":: %-12s %zu packages (index %lld)", r->name, probe.npkgs,
	    probe.timestamp);
	repo_clear(&probe);
	if (rename(sig_tmp, sig) == -1 || rename(idx_tmp, idx) == -1) {
		upkg_err("%s: cannot install index: %s", r->name,
		    strerror(errno));
		goto out;
	}
	repo_clear(r);
	rv = 0;
out:
	unlink(idx_tmp);
	unlink(sig_tmp);
	free(text);
	free(sigtext);
	free(dir);
	free(idx_url);
	free(sig_url);
	free(idx_tmp);
	free(sig_tmp);
	free(idx);
	free(sig);
	return rv;
}

int
repo_load(struct config *cfg, struct repo *r)
{
	char *idx = sync_path(cfg, r, "INDEX"), *sig = sync_path(cfg, r, "INDEX.sig");
	char *text = NULL, *sigtext = NULL;
	size_t len;
	int rv = -1;

	if (r->loaded) {
		rv = 0;
		goto out;
	}
	if (read_file(idx, &text, &len) == -1 ||
	    read_file(sig, &sigtext, NULL) == -1) {
		if (errno == ENOENT)
			upkg_warn("repository '%s' not synchronised; run "
			    "'upkg sync'", r->name);
		else
			upkg_err("%s: %s", idx, strerror(errno));
		goto out;
	}
	/* the cached copy is re-verified: the cache is not a trust anchor */
	if (verify_index(cfg, r, text, len, sigtext) == -1)
		goto out;
	repo_clear(r);
	if (index_parse(text, r) == -1) {
		repo_clear(r);
		goto out;
	}
	r->loaded = 1;
	rv = 0;
out:
	free(text);
	free(sigtext);
	free(idx);
	free(sig);
	return rv;
}

int
repo_load_all(struct config *cfg)
{
	size_t i;
	int n = 0;

	for (i = 0; i < cfg->nrepos; i++) {
		if (!cfg->repos[i].enabled)
			continue;
		if (repo_load(cfg, &cfg->repos[i]) == 0)
			n++;
	}
	return n;
}

int
repo_fetch_pkg(struct config *cfg, struct pkgmeta *m, char **outpath)
{
	char *dir = path_join(cfg->cachedir, m->repo->name);
	char *path = path_join(dir, m->filename);
	char *url = xasprintf("%s/%s", m->repo->url, m->filename);
	char sum[SHA256_HEX_LEN + 1];
	off_t size;
	int rv = -1;

	if (mkdir_p(dir, 0755) == -1)
		goto out;
	if (access(path, F_OK) == 0 && sha256_file(path, sum, &size) == 0 &&
	    strcmp(sum, m->sha256) == 0 && size == m->size) {
		upkg_debug("using cached %s", path);
	} else {
		upkg_msg("  fetching %s/%s", m->repo->name, m->filename);
		if (fetch_url(cfg, url, path) == -1)
			goto out;
		if (sha256_file(path, sum, &size) == -1)
			goto out;
		if (strcmp(sum, m->sha256) != 0 || size != m->size) {
			upkg_err("%s: checksum mismatch (expected %s, got %s); "
			    "file removed", m->filename, m->sha256, sum);
			unlink(path);
			goto out;
		}
	}
	*outpath = path;
	path = NULL;
	rv = 0;
out:
	free(dir);
	free(path);
	free(url);
	return rv;
}

/* ------------------------------------------------------------ publication */

static int
meta_cmp(const void *a, const void *b)
{
	const struct pkgmeta *x = a, *y = b;
	int c = strcmp(x->name, y->name);

	return c ? c : -fullvercmp(x->version, x->release, y->version, y->release);
}

int
repo_build_index(const char *dir, const char *reponame, const char *keyfile)
{
	DIR *d;
	struct dirent *de;
	struct pkgmeta *pk = NULL;
	size_t npk = 0, i, len;
	char *buf, *s, *sighex = NULL, *p;
	int rv = -1;

	if (!valid_pkgname(reponame)) {
		upkg_err("invalid repository name '%s'", reponame);
		return -1;
	}
	if ((d = opendir(dir)) == NULL) {
		upkg_err("%s: %s", dir, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		struct pkgfile pf;
		char sum[SHA256_HEX_LEN + 1];
		off_t size;

		if (!valid_filename(de->d_name))
			continue;
		p = path_join(dir, de->d_name);
		if (pkgfile_read_meta(p, &pf) == -1 ||
		    sha256_file(p, sum, &size) == -1) {
			free(p);
			closedir(d);
			goto out;
		}
		free(p);
		char *canon = pkgmeta_filename(&pf.meta);
		if (strcmp(canon, de->d_name) != 0) {
			upkg_err("%s: file name does not match metadata "
			    "(expected %s)", de->d_name, canon);
			free(canon);
			pkgfile_free(&pf);
			closedir(d);
			goto out;
		}
		free(canon);
		pk = xrealloc(pk, (npk + 1) * sizeof(*pk));
		pk[npk] = pf.meta;
		memset(&pf.meta, 0, sizeof(pf.meta));
		pk[npk].filename = xstrdup(de->d_name);
		pk[npk].sha256 = xstrdup(sum);
		pk[npk].size = (long long)size;
		npk++;
		pkgfile_free(&pf);
	}
	closedir(d);
	if (npk)
		qsort(pk, npk, sizeof(*pk), meta_cmp);

	buf = xasprintf("upkg-index %d\nrepo %s\ntimestamp %lld\n",
	    UPKG_INDEX_FORMAT, reponame, (long long)time(NULL));
	len = strlen(buf);
	for (i = 0; i < npk; i++) {
		s = pkgmeta_format(&pk[i], 1);
		buf = xrealloc(buf, len + strlen(s) + 2);
		buf[len++] = '\n';
		memcpy(buf + len, s, strlen(s) + 1);
		len += strlen(s);
		free(s);
	}
	if (sig_sign(keyfile, buf, len, &sighex) == -1) {
		free(buf);
		goto out;
	}
	p = path_join(dir, "INDEX");
	s = path_join(dir, "INDEX.sig");
	char *sigline = xasprintf("%s\n", sighex);
	/* write the signature last so readers never see INDEX/sig mismatch */
	if (write_file_atomic(p, buf, len, 0644) == 0 &&
	    write_file_atomic(s, sigline, strlen(sigline), 0644) == 0) {
		upkg_msg("indexed %zu package%s in %s (repo %s)", npk,
		    npk == 1 ? "" : "s", dir, reponame);
		rv = 0;
	}
	free(sigline);
	free(p);
	free(s);
	free(buf);
out:
	for (i = 0; i < npk; i++)
		pkgmeta_free(&pk[i]);
	free(pk);
	free(sighex);
	return rv;
}
