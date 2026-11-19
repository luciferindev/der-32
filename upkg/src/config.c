/*
 * upkg -- configuration: /etc/upkg/upkg.conf and /etc/upkg/repos.conf.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Both files use a minimal INI dialect:
 *
 *	# comment
 *	[section]
 *	key = value
 */

#include <sys/utsname.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upkg.h"

int
parse_bool(const char *s, int *out)
{
	if (!strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "1") ||
	    !strcmp(s, "on"))
		*out = 1;
	else if (!strcmp(s, "no") || !strcmp(s, "false") || !strcmp(s, "0") ||
	    !strcmp(s, "off"))
		*out = 0;
	else
		return -1;
	return 0;
}

int
ini_parse(const char *text, const char *fname,
    int (*cb)(void *, const char *, const char *, const char *, int), void *arg)
{
	char *buf = xstrdup(text), *line, *next, *section = NULL, *eq;
	int lineno = 0, rv = 0;

	for (line = buf; line; line = next) {
		lineno++;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		line = trim(line);
		if (*line == '\0' || *line == '#' || *line == ';')
			continue;
		if (*line == '[') {
			char *e = strchr(line, ']');

			if (e == NULL || e[1] != '\0') {
				upkg_err("%s:%d: malformed section header",
				    fname, lineno);
				rv = -1;
				break;
			}
			*e = '\0';
			free(section);
			section = xstrdup(trim(line + 1));
			if ((rv = cb(arg, section, NULL, NULL, lineno)) != 0)
				break;
			continue;
		}
		if ((eq = strchr(line, '=')) == NULL) {
			upkg_err("%s:%d: expected 'key = value'", fname, lineno);
			rv = -1;
			break;
		}
		*eq = '\0';
		if (section == NULL) {
			upkg_err("%s:%d: key outside of a section", fname, lineno);
			rv = -1;
			break;
		}
		if ((rv = cb(arg, section, trim(line), trim(eq + 1), lineno)) != 0)
			break;
	}
	free(section);
	free(buf);
	return rv;
}

struct ctx {
	struct config	*cfg;
	const char	*fname;
};

static int
opts_cb(void *arg, const char *sec, const char *key, const char *val, int line)
{
	struct ctx *c = arg;
	struct config *cfg = c->cfg;

	if (key == NULL) {
		if (strcmp(sec, "options") != 0) {
			upkg_err("%s:%d: unknown section [%s]", c->fname, line, sec);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "arch") == 0) {
		free(cfg->arch);
		cfg->arch = xstrdup(val);
	} else if (strcmp(key, "prefixes") == 0) {
		char *s = xstrdup(val), *tok, *save = NULL;

		sl_free(&cfg->prefixes);
		for (tok = strtok_r(s, " \t,", &save); tok;
		    tok = strtok_r(NULL, " \t,", &save)) {
			while (*tok == '/')
				tok++;
			size_t n = strlen(tok);
			while (n > 0 && tok[n - 1] == '/')
				tok[--n] = '\0';
			if (!safe_relpath(tok)) {
				upkg_err("%s:%d: bad prefix '%s'", c->fname,
				    line, tok);
				free(s);
				return -1;
			}
			sl_add(&cfg->prefixes, tok);
		}
		free(s);
	} else if (strcmp(key, "portsdir") == 0) {
		free(cfg->portsdir);
		cfg->portsdir = xstrdup(val);
	} else if (strcmp(key, "builddir") == 0) {
		free(cfg->builddir);
		cfg->builddir = xstrdup(val);
	} else if (strcmp(key, "build_user") == 0) {
		free(cfg->build_user);
		cfg->build_user = xstrdup(val);
	} else if (strcmp(key, "ca_file") == 0) {
		free(cfg->ca_file);
		cfg->ca_file = *val ? xstrdup(val) : NULL;
	} else if (strcmp(key, "allow_http") == 0) {
		if (parse_bool(val, &cfg->allow_http) == -1)
			goto badval;
	} else {
		upkg_err("%s:%d: unknown option '%s'", c->fname, line, key);
		return -1;
	}
	return 0;
badval:
	upkg_err("%s:%d: bad value for '%s'", c->fname, line, key);
	return -1;
}

static int
repos_cb(void *arg, const char *sec, const char *key, const char *val, int line)
{
	struct ctx *c = arg;
	struct config *cfg = c->cfg;
	struct repo *r;

	if (key == NULL) {
		if (!valid_pkgname(sec)) {
			upkg_err("%s:%d: invalid repository name '%s'",
			    c->fname, line, sec);
			return -1;
		}
		if (config_repo(cfg, sec) != NULL) {
			upkg_err("%s:%d: duplicate repository '%s'",
			    c->fname, line, sec);
			return -1;
		}
		cfg->repos = xrealloc(cfg->repos,
		    (cfg->nrepos + 1) * sizeof(*cfg->repos));
		r = &cfg->repos[cfg->nrepos++];
		memset(r, 0, sizeof(*r));
		r->name = xstrdup(sec);
		r->enabled = 1;
		return 0;
	}
	r = &cfg->repos[cfg->nrepos - 1];
	if (strcmp(key, "url") == 0) {
		free(r->url);
		r->url = xstrdup(val);
	} else if (strcmp(key, "priority") == 0) {
		char *e;
		long p = strtol(val, &e, 10);

		if (*val == '\0' || *e || p < -1000000 || p > 1000000)
			goto badval;
		r->priority = (int)p;
	} else if (strcmp(key, "enabled") == 0) {
		if (parse_bool(val, &r->enabled) == -1)
			goto badval;
	} else if (strcmp(key, "key") == 0) {
		free(r->key);
		if (!valid_pkgname(val))
			goto badval;
		r->key = xstrdup(val);
	} else {
		upkg_err("%s:%d: unknown repository option '%s'", c->fname,
		    line, key);
		return -1;
	}
	return 0;
badval:
	upkg_err("%s:%d: bad value for '%s'", c->fname, line, key);
	return -1;
}

struct repo *
config_repo(struct config *cfg, const char *name)
{
	size_t i;

	for (i = 0; i < cfg->nrepos; i++)
		if (strcmp(cfg->repos[i].name, name) == 0)
			return &cfg->repos[i];
	return NULL;
}

static int
repo_prio_cmp(const void *a, const void *b)
{
	const struct repo *ra = a, *rb = b;

	return (rb->priority > ra->priority) - (rb->priority < ra->priority);
}

static const char *
native_arch(void)
{
	static struct utsname u;

	if (uname(&u) == -1)
		return "unknown";
	if (strcmp(u.machine, "x86_64") == 0)
		return "amd64";
	if (strcmp(u.machine, "aarch64") == 0)
		return "evbarm64";
	return u.machine;
}

int
config_load(struct config *cfg, const char *root)
{
	struct ctx c;
	char *path, *text;
	size_t i;

	memset(cfg, 0, sizeof(*cfg));
	if (root == NULL || *root == '\0')
		root = "/";
	cfg->root = xstrdup(root);
	cfg->confdir = path_join(root, "etc/upkg");
	cfg->keydir = path_join(cfg->confdir, "keys");
	cfg->dbdir = path_join(root, "var/lib/upkg");
	cfg->cachedir = path_join(root, "var/cache/upkg");
	cfg->arch = xstrdup(native_arch());
	cfg->portsdir = xstrdup("/usr/ports");
	cfg->builddir = xstrdup("/var/tmp/upkg-build");
	cfg->build_user = xstrdup("nobody");
	sl_init(&cfg->prefixes);
	sl_add(&cfg->prefixes, UPKG_DEFAULT_PREFIX);

	c.cfg = cfg;
	path = path_join(cfg->confdir, "upkg.conf");
	c.fname = path;
	if (read_file(path, &text, NULL) == 0) {
		int rv = ini_parse(text, path, opts_cb, &c);

		free(text);
		if (rv) {
			free(path);
			return -1;
		}
	} else if (errno != ENOENT) {
		upkg_err("cannot read %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	free(path);

	path = path_join(cfg->confdir, "repos.conf");
	c.fname = path;
	if (read_file(path, &text, NULL) == 0) {
		int rv = ini_parse(text, path, repos_cb, &c);

		free(text);
		if (rv) {
			free(path);
			return -1;
		}
	} else if (errno != ENOENT) {
		upkg_err("cannot read %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	free(path);

	for (i = 0; i < cfg->nrepos; i++)
		if (cfg->repos[i].url == NULL) {
			upkg_err("repository '%s' has no url", cfg->repos[i].name);
			return -1;
		}
	/* stable sort: equal priorities keep configuration order */
	for (i = 1; i < cfg->nrepos; i++) {
		struct repo tmp = cfg->repos[i];
		size_t j = i;

		while (j > 0 && repo_prio_cmp(&cfg->repos[j - 1], &tmp) > 0) {
			cfg->repos[j] = cfg->repos[j - 1];
			j--;
		}
		cfg->repos[j] = tmp;
	}
	return 0;
}

int
config_save_repos(const struct config *cfg)
{
	char *path = path_join(cfg->confdir, "repos.conf"), *buf, *s;
	size_t len, i;
	int rv;

	buf = xstrdup("# upkg repository configuration -- see upkg(8).\n"
	    "# Higher priority wins when a package exists in several "
	    "repositories.\n");
	len = strlen(buf);
	for (i = 0; i < cfg->nrepos; i++) {
		const struct repo *r = &cfg->repos[i];

		s = xasprintf("\n[%s]\nurl = %s\npriority = %d\nenabled = %s\n"
		    "%s%s%s", r->name, r->url, r->priority,
		    r->enabled ? "yes" : "no", r->key ? "key = " : "",
		    r->key ? r->key : "", r->key ? "\n" : "");
		buf = xrealloc(buf, len + strlen(s) + 1);
		memcpy(buf + len, s, strlen(s) + 1);
		len += strlen(s);
		free(s);
	}
	if (mkdir_p(cfg->confdir, 0755) == -1) {
		free(buf);
		free(path);
		return -1;
	}
	rv = write_file_atomic(path, buf, len, 0644);
	free(buf);
	free(path);
	return rv;
}

void
config_free(struct config *cfg)
{
	size_t i, j;

	for (i = 0; i < cfg->nrepos; i++) {
		struct repo *r = &cfg->repos[i];

		free(r->name);
		free(r->url);
		free(r->key);
		for (j = 0; j < r->npkgs; j++)
			pkgmeta_free(&r->pkgs[j]);
		free(r->pkgs);
	}
	free(cfg->repos);
	free(cfg->root);
	free(cfg->confdir);
	free(cfg->keydir);
	free(cfg->dbdir);
	free(cfg->cachedir);
	free(cfg->arch);
	free(cfg->portsdir);
	free(cfg->builddir);
	free(cfg->build_user);
	free(cfg->ca_file);
	sl_free(&cfg->prefixes);
	memset(cfg, 0, sizeof(*cfg));
}
