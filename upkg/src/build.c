/*
 * upkg -- building packages from source recipes ("upkg build").
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A port is a directory containing:
 *
 *	RECIPE	metadata: the +PKGINFO keys plus
 *		    source <url>          distfile (https://, file:// or path)
 *		    source_sha256 <hex>   mandatory checksum of the distfile
 *		    builddep <spec>       required installed build dependency
 *	build	a POSIX sh script run with "sh -e"
 *
 * The build script is run only when the user explicitly asks for it, never
 * as a side effect of synchronising a repository.  When upkg runs as root,
 * extraction and the build script run as the unprivileged build_user
 * (default "nobody").  The script sees:
 *
 *	SRCDIR   unpacked source (the single top-level directory, if any)
 *	DESTDIR  staging root; install into $DESTDIR$PREFIX
 *	PREFIX   /usr/local
 *	JOBS     suggested make -j value
 *	PORTDIR  the port directory (for files shipped with the recipe)
 */

#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "upkg.h"

struct recipe {
	char		*dir;
	char		*pkginfo;	/* text accepted by pkgmeta_parse */
	struct pkgmeta	 meta;
	char		*source;
	char		*source_sha256;
	struct strlist	 builddeps;
};

static void
recipe_free(struct recipe *r)
{
	free(r->dir);
	free(r->pkginfo);
	pkgmeta_free(&r->meta);
	free(r->source);
	free(r->source_sha256);
	sl_free(&r->builddeps);
}

static int
is_port(const char *dir)
{
	char *p = path_join(dir, "RECIPE");
	int ok = access(p, R_OK) == 0;

	free(p);
	return ok;
}

static char *
find_port(struct config *cfg, const char *target)
{
	DIR *d;
	struct dirent *de;
	char *p;

	if (strchr(target, '/') || strcmp(target, ".") == 0)
		return is_port(target) ? xstrdup(target) : NULL;
	p = path_join(cfg->portsdir, target);
	if (is_port(p))
		return p;
	free(p);
	if ((d = opendir(cfg->portsdir)) == NULL)
		return NULL;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		char *cat = path_join(cfg->portsdir, de->d_name);
		p = path_join(cat, target);
		free(cat);
		if (is_port(p)) {
			closedir(d);
			return p;
		}
		free(p);
	}
	closedir(d);
	return NULL;
}

static int
recipe_load(struct config *cfg, const char *dir, struct recipe *r)
{
	char *path = path_join(dir, "RECIPE"), *text, *line, *next;
	size_t len = 0, cap = 256;
	int have_arch = 0, rv = -1;

	memset(r, 0, sizeof(*r));
	sl_init(&r->builddeps);
	r->dir = xstrdup(dir);
	if (read_file(path, &text, NULL) == -1) {
		upkg_err("%s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	r->pkginfo = xmalloc(cap);
	*r->pkginfo = '\0';
	for (line = text; line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		char *t = trim(line), *v = t;

		while (*v && *v != ' ' && *v != '\t')
			v++;
		char save = *v;
		*v = '\0';
		char *val = trim(save ? v + 1 : v);
		if (strcmp(t, "source") == 0) {
			free(r->source);
			r->source = xstrdup(val);
			continue;
		}
		if (strcmp(t, "source_sha256") == 0) {
			free(r->source_sha256);
			r->source_sha256 = xstrdup(val);
			continue;
		}
		if (strcmp(t, "builddep") == 0) {
			sl_add(&r->builddeps, val);
			continue;
		}
		if (strcmp(t, "arch") == 0)
			have_arch = 1;
		*v = save;
		size_t n = strlen(t);
		while (len + n + 2 > cap)
			cap *= 2;
		r->pkginfo = xrealloc(r->pkginfo, cap);
		memcpy(r->pkginfo + len, t, n);
		len += n;
		r->pkginfo[len++] = '\n';
		r->pkginfo[len] = '\0';
	}
	free(text);
	if (!have_arch) {
		char *s = xasprintf("%sarch %s\n", r->pkginfo, cfg->arch);
		free(r->pkginfo);
		r->pkginfo = s;
	}
	if (pkgmeta_parse(r->pkginfo, &r->meta, path) == -1 ||
	    pkgmeta_validate(&r->meta, path) == -1)
		goto out;
	if (r->source && (r->source_sha256 == NULL ||
	    !is_hex(r->source_sha256, SHA256_HEX_LEN))) {
		upkg_err("%s: 'source' requires a valid 'source_sha256'", path);
		goto out;
	}
	rv = 0;
out:
	free(path);
	return rv;
}

static int
rm_rf(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *de;

	if (lstat(path, &st) == -1)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	chmod(path, 0700);
	if ((d = opendir(path)) == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char *p = path_join(path, de->d_name);
		rm_rf(p);
		free(p);
	}
	closedir(d);
	return rmdir(path);
}

static int
extract_source(const char *archive, const char *dest)
{
	struct archive *a = archive_read_new(), *w = archive_write_disk_new();
	struct archive_entry *e;
	int r, rv = -1;

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME |
	    ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
	    ARCHIVE_EXTRACT_SECURE_SYMLINKS |
	    ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS);
	if (archive_read_open_filename(a, archive, 65536) != ARCHIVE_OK) {
		upkg_err("%s: %s", archive, archive_error_string(a));
		goto out;
	}
	/* extract relative to dest; libarchive's SECURE_* flags police paths */
	if (chdir(dest) == -1) {
		upkg_err("chdir %s: %s", dest, strerror(errno));
		goto out;
	}
	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
		if (archive_write_header(w, e) != ARCHIVE_OK) {
			upkg_err("extract: %s", archive_error_string(w));
			goto out;
		}
		const void *buf;
		size_t sz;
		la_int64_t off;
		while ((r = archive_read_data_block(a, &buf, &sz, &off)) ==
		    ARCHIVE_OK)
			if (archive_write_data_block(w, buf, sz, off) != ARCHIVE_OK) {
				upkg_err("extract: %s", archive_error_string(w));
				goto out;
			}
		if (r != ARCHIVE_EOF) {
			upkg_err("extract: %s", archive_error_string(a));
			goto out;
		}
		archive_write_finish_entry(w);
	}
	if (r != ARCHIVE_EOF) {
		upkg_err("%s: %s", archive, archive_error_string(a));
		goto out;
	}
	rv = 0;
out:
	archive_read_free(a);
	archive_write_free(w);
	return rv;
}

static char *
single_topdir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char *only = NULL;
	int n = 0;

	if (d == NULL)
		return xstrdup(dir);
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		n++;
		free(only);
		only = xstrdup(de->d_name);
	}
	closedir(d);
	if (n == 1) {
		char *p = path_join(dir, only);
		struct stat st;

		free(only);
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
			return p;
		free(p);
		return xstrdup(dir);
	}
	free(only);
	return xstrdup(dir);
}

static int
check_builddeps(struct config *cfg, struct recipe *r)
{
	struct db *db;
	size_t i;
	int bad = 0;

	if (r->builddeps.n == 0)
		return 0;
	if (db_open(cfg, &db, 1) == -1)
		return -1;
	for (i = 0; i < r->builddeps.n; i++) {
		struct depspec d;
		struct instpkg ip;
		int f;

		if (depspec_parse(r->builddeps.v[i], &d, 0) == -1) {
			upkg_err("invalid builddep '%s'", r->builddeps.v[i]);
			bad = 1;
			continue;
		}
		f = db_get_pkg(db, d.name, &ip);
		if (f != 1 || !depspec_match(&d, ip.version, ip.release)) {
			upkg_err("build dependency '%s' is not installed "
			    "(upkg install '%s')", r->builddeps.v[i],
			    r->builddeps.v[i]);
			bad = 1;
		}
		if (f == 1)
			instpkg_free(&ip);
		depspec_free(&d);
	}
	db_close(db);
	return bad ? -1 : 0;
}

int
op_build(struct config *cfg, const char *target, struct opts *o)
{
	struct recipe r;
	char *portdir, *work = NULL, *srcdir = NULL, *dest = NULL;
	char *distfile = NULL, *script = NULL, *out = NULL, *fname;
	struct passwd *pw = NULL;
	int rv = -1, status;
	pid_t pid;

	if ((portdir = find_port(cfg, target)) == NULL) {
		upkg_err("no port named '%s' (looked in %s)", target,
		    cfg->portsdir);
		return -1;
	}
	if (recipe_load(cfg, portdir, &r) == -1) {
		free(portdir);
		return -1;
	}
	upkg_msg(":: building %s %s-%u from %s", r.meta.name, r.meta.version,
	    r.meta.release, portdir);
	script = path_join(portdir, "build");
	if (access(script, R_OK) == -1) {
		upkg_err("%s: %s", script, strerror(errno));
		goto out;
	}
	if (check_builddeps(cfg, &r) == -1)
		goto out;
	if (geteuid() == 0) {
		if ((pw = getpwnam(cfg->build_user)) == NULL) {
			upkg_err("build user '%s' does not exist",
			    cfg->build_user);
			goto out;
		}
		if (pw->pw_uid == 0) {
			upkg_err("refusing to build as root; set build_user");
			goto out;
		}
	}

	if (mkdir_p(cfg->builddir, 0755) == -1)
		goto out;
	work = xasprintf("%s/%s-%s.XXXXXX", cfg->builddir, r.meta.name,
	    r.meta.version);
	if (mkdtemp(work) == NULL) {
		upkg_err("mkdtemp %s: %s", work, strerror(errno));
		free(work);
		work = NULL;
		goto out;
	}
	if (pw && chown(work, pw->pw_uid, pw->pw_gid) == -1) {
		upkg_err("chown %s: %s", work, strerror(errno));
		goto out;
	}
	srcdir = path_join(work, "src");
	dest = path_join(work, "dest");

	if (r.source) {
		char *dd = path_join(cfg->cachedir, "distfiles");
		const char *base = strrchr(r.source, '/');
		char sum[SHA256_HEX_LEN + 1];

		base = base ? base + 1 : r.source;
		if (mkdir_p(dd, 0755) == -1 || !safe_relpath(base)) {
			free(dd);
			goto out;
		}
		distfile = path_join(dd, base);
		free(dd);
		if (access(distfile, F_OK) == -1 || sha256_file(distfile, sum,
		    NULL) == -1 || strcmp(sum, r.source_sha256) != 0) {
			upkg_msg("  fetching %s", r.source);
			if (fetch_url(cfg, r.source, distfile) == -1)
				goto out;
		}
		if (sha256_file(distfile, sum, NULL) == -1)
			goto out;
		if (strcmp(sum, r.source_sha256) != 0) {
			upkg_err("%s: checksum mismatch (expected %s, got %s)",
			    distfile, r.source_sha256, sum);
			unlink(distfile);
			goto out;
		}
		chmod(distfile, 0644);
	}

	fflush(stdout);
	if ((pid = fork()) == -1) {
		upkg_err("fork: %s", strerror(errno));
		goto out;
	}
	if (pid == 0) {
		char *sd;

		if (pw) {
			if (setgroups(1, &pw->pw_gid) == -1 ||
			    setgid(pw->pw_gid) == -1 || setuid(pw->pw_uid) == -1 ||
			    setuid(0) != -1) {
				fprintf(stderr, "upkg: cannot drop privileges\n");
				_exit(126);
			}
		}
		umask(022);
		if (mkdir(srcdir, 0755) == -1 || mkdir(dest, 0755) == -1) {
			perror("upkg: mkdir");
			_exit(126);
		}
		if (distfile && extract_source(distfile, srcdir) == -1)
			_exit(126);
		sd = single_topdir(srcdir);
		if (chdir(sd) == -1) {
			perror("upkg: chdir");
			_exit(126);
		}
		char *env[] = {
			xstrdup("PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/pkg/bin:"
			    "/usr/local/bin"),
			xasprintf("HOME=%s", work),
			xasprintf("SRCDIR=%s", sd),
			xasprintf("DESTDIR=%s", dest),
			xstrdup("PREFIX=/usr/local"),
			xasprintf("JOBS=%ld", sysconf(_SC_NPROCESSORS_ONLN) > 0 ?
			    sysconf(_SC_NPROCESSORS_ONLN) : 1L),
			xasprintf("UPKG_ARCH=%s", cfg->arch),
			xasprintf("PKGNAME=%s", r.meta.name),
			xasprintf("PKGVERSION=%s", r.meta.version),
			xasprintf("PORTDIR=%s", portdir),
			xstrdup("LC_ALL=C"),
			NULL,
		};
		execle("/bin/sh", "sh", "-e", script, (char *)NULL, env);
		perror("upkg: exec /bin/sh");
		_exit(127);
	}
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR) {
			upkg_err("waitpid: %s", strerror(errno));
			goto out;
		}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		upkg_err("build of %s failed (status %d); work directory kept "
		    "at %s", r.meta.name, WIFEXITED(status) ?
		    WEXITSTATUS(status) : -1, work);
		free(work);
		work = NULL;
		goto out;
	}

	fname = pkgmeta_filename(&r.meta);
	if (o->output) {
		struct stat st;

		if (stat(o->output, &st) == 0 && S_ISDIR(st.st_mode))
			out = path_join(o->output, fname);
		else
			out = xstrdup(o->output);
	} else
		out = xstrdup(fname);
	free(fname);
	{
		char *info = pkgmeta_format(&r.meta, 0);

		rv = pkgfile_create(info, dest, out);
		free(info);
	}
	if (rv == 0)
		upkg_msg(":: built %s", out);
out:
	if (work && rv == 0)
		rm_rf(work);
	free(work);
	free(srcdir);
	free(dest);
	free(distfile);
	free(script);
	free(out);
	free(portdir);
	recipe_free(&r);
	return rv;
}
