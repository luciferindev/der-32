/*
 * upkg -- reading, verifying, extracting and creating .upkg archives.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A .upkg file is an xz-compressed POSIX (pax) tar archive whose first two
 * members are +PKGINFO and +MANIFEST, followed by the payload in manifest
 * order.  Payload members must correspond one-to-one with manifest records;
 * every regular file is hashed while it is staged and compared with the
 * manifest digest.  Archive entries are never handed to libarchive's
 * extractor: upkg writes every object itself, beneath the root, through the
 * transaction layer, which refuses to follow symlinks.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "upkg.h"

#define MAX_PKGINFO	(1 << 20)
#define MAX_MANIFEST	(64 << 20)

static struct archive *
open_read(const char *path)
{
	struct archive *a = archive_read_new();

	archive_read_support_filter_all(a);
	archive_read_support_format_tar(a);
	if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) {
		upkg_err("%s: %s", path, archive_error_string(a));
		archive_read_free(a);
		return NULL;
	}
	return a;
}

static char *
read_member(struct archive *a, struct archive_entry *e, const char *path,
    const char *want, la_int64_t max)
{
	la_int64_t size;
	char *buf;
	la_ssize_t r;

	if (strcmp(archive_entry_pathname(e), want) != 0 ||
	    archive_entry_filetype(e) != AE_IFREG) {
		upkg_err("%s: not a upkg package (expected %s, found '%s')",
		    path, want, archive_entry_pathname(e));
		return NULL;
	}
	size = archive_entry_size(e);
	if (size < 0 || size > max) {
		upkg_err("%s: %s too large", path, want);
		return NULL;
	}
	buf = xmalloc((size_t)size + 1);
	r = archive_read_data(a, buf, (size_t)size);
	if (r != size) {
		upkg_err("%s: short read of %s", path, want);
		free(buf);
		return NULL;
	}
	buf[size] = '\0';
	if (strlen(buf) != (size_t)size) {
		upkg_err("%s: NUL byte in %s", path, want);
		free(buf);
		return NULL;
	}
	return buf;
}

static int
read_header(struct archive *a, const char *path, struct pkgfile *pf)
{
	struct archive_entry *e;
	char *info = NULL, *man = NULL;
	int rv = -1;

	if (archive_read_next_header(a, &e) != ARCHIVE_OK) {
		upkg_err("%s: %s", path, archive_error_string(a));
		return -1;
	}
	if ((info = read_member(a, e, path, UPKG_PKGINFO, MAX_PKGINFO)) == NULL)
		goto out;
	if (archive_read_next_header(a, &e) != ARCHIVE_OK) {
		upkg_err("%s: truncated package", path);
		goto out;
	}
	if ((man = read_member(a, e, path, UPKG_MANIFEST, MAX_MANIFEST)) == NULL)
		goto out;
	if (pkgmeta_parse(info, &pf->meta, path) == -1)
		goto out;
	if (pkgmeta_validate(&pf->meta, path) == -1 ||
	    manifest_parse(man, &pf->manifest) == -1) {
		pkgmeta_free(&pf->meta);
		goto out;
	}
	if (pf->meta.filename || pf->meta.sha256 || pf->meta.size != -1) {
		upkg_err("%s: index-only keys in +PKGINFO", path);
		goto bad;
	}
	for (size_t i = 0; i < pf->manifest.n; i++) {
		struct mentry *me = &pf->manifest.e[i];

		if (!safe_relpath(me->path)) {
			upkg_err("%s: unsafe path in manifest: '%s'", path,
			    me->path);
			goto bad;
		}
		if (i > 0 && strcmp(pf->manifest.e[i - 1].path, me->path) >= 0) {
			upkg_err("%s: manifest not sorted or has duplicates "
			    "near '%s'", path, me->path);
			goto bad;
		}
	}
	rv = 0;
	goto out;
bad:
	pkgmeta_free(&pf->meta);
	manifest_free(&pf->manifest);
out:
	free(info);
	free(man);
	return rv;
}

int
pkgfile_read_meta(const char *path, struct pkgfile *pf)
{
	struct archive *a;
	int rv;

	memset(pf, 0, sizeof(*pf));
	if ((a = open_read(path)) == NULL)
		return -1;
	rv = read_header(a, path, pf);
	archive_read_free(a);
	if (rv == 0)
		pf->path = xstrdup(path);
	return rv;
}

void
pkgfile_free(struct pkgfile *pf)
{
	pkgmeta_free(&pf->meta);
	manifest_free(&pf->manifest);
	free(pf->path);
	memset(pf, 0, sizeof(*pf));
}

int
pkgfile_extract(const char *path, struct pkgfile *pf, struct txn *t)
{
	struct archive *a;
	struct archive_entry *e;
	struct pkgfile hdr;
	char *cur_name = NULL;
	int r, rv = -1;
	size_t i;

	if ((a = open_read(path)) == NULL)
		return -1;
	/* re-read and compare the header: the file may have been swapped */
	memset(&hdr, 0, sizeof(hdr));
	if (read_header(a, path, &hdr) == -1)
		goto out;
	{
		char *m1 = manifest_format(&hdr.manifest);
		char *m2 = manifest_format(&pf->manifest);
		int same = strcmp(m1, m2) == 0;

		free(m1);
		free(m2);
		pkgmeta_free(&hdr.meta);
		manifest_free(&hdr.manifest);
		if (!same) {
			upkg_err("%s: package changed while being installed",
			    path);
			goto out;
		}
	}
	for (i = 0; i < pf->manifest.n; i++)
		pf->manifest.e[i].seen = 0;

	while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
		const char *raw = archive_entry_pathname(e);
		char *name = NULL;
		struct mentry *me;

		/* tar writers mark directories with a trailing '/' */
		if (raw) {
			size_t rl = strlen(raw);

			name = xstrdup(raw);
			if (archive_entry_filetype(e) == AE_IFDIR && rl > 1 &&
			    name[rl - 1] == '/')
				name[rl - 1] = '\0';
		}
		free(cur_name);
		cur_name = name;

		if (upkg_interrupted) {
			upkg_err("interrupted");
			goto out;
		}
		if (name == NULL || (me = manifest_find(&pf->manifest, name)) == NULL) {
			upkg_err("%s: archive member '%s' is not in the "
			    "manifest", path, name ? name : "(null)");
			goto out;
		}
		if (me->seen) {
			upkg_err("%s: duplicate archive member '%s'", path, name);
			goto out;
		}
		me->seen = 1;
		if (archive_entry_hardlink(e) != NULL) {
			upkg_err("%s: hard link '%s' not permitted", path, name);
			goto out;
		}
		switch (archive_entry_filetype(e)) {
		case AE_IFDIR:
			if (me->type != M_DIR)
				goto mismatch;
			if (txn_stage_dir(t, me->path, me->mode) == -1)
				goto out;
			break;
		case AE_IFLNK: {
			const char *tg = archive_entry_symlink(e);

			if (me->type != M_LINK || tg == NULL ||
			    strcmp(tg, me->target) != 0)
				goto mismatch;
			if (txn_stage_link(t, me->path, me->target) == -1)
				goto out;
			break;
		}
		case AE_IFREG: {
			struct sha256_ctx *h;
			char sum[SHA256_HEX_LEN + 1], buf[65536];
			long long total = 0;
			la_ssize_t n;
			int fd;

			if (me->type != M_FILE)
				goto mismatch;
			if (txn_stage_file_open(t, me->path, me->mode, &fd) == -1)
				goto out;
			h = sha256_new();
			while ((n = archive_read_data(a, buf, sizeof(buf))) > 0) {
				sha256_update(h, buf, (size_t)n);
				total += n;
				if (total > me->size) {
					break;
				}
				const char *p = buf;
				la_ssize_t left = n;
				while (left > 0) {
					ssize_t w = write(fd, p, (size_t)left);
					if (w == -1 && errno == EINTR)
						continue;
					if (w == -1) {
						upkg_err("write %s: %s", me->path,
						    strerror(errno));
						sha256_final_hex(h, sum);
						close(fd);
						goto out;
					}
					p += w;
					left -= w;
				}
			}
			sha256_final_hex(h, sum);
			if (n < 0) {
				upkg_err("%s: %s", path, archive_error_string(a));
				close(fd);
				goto out;
			}
			if (fsync(fd) == -1 || close(fd) == -1) {
				upkg_err("%s: %s", me->path, strerror(errno));
				goto out;
			}
			if (total != me->size || strcmp(sum, me->sha256) != 0) {
				upkg_err("%s: checksum mismatch for '%s'", path,
				    me->path);
				goto out;
			}
			break;
		}
		default:
			upkg_err("%s: '%s' has an unsupported file type", path,
			    name);
			goto out;
		}
		continue;
mismatch:
		upkg_err("%s: archive member '%s' does not match its manifest "
		    "record", path, name);
		goto out;
	}
	if (r != ARCHIVE_EOF) {
		upkg_err("%s: %s", path, archive_error_string(a));
		goto out;
	}
	for (i = 0; i < pf->manifest.n; i++)
		if (!pf->manifest.e[i].seen) {
			upkg_err("%s: manifest entry '%s' missing from archive",
			    path, pf->manifest.e[i].path);
			goto out;
		}
	rv = 0;
out:
	free(cur_name);
	archive_read_free(a);
	return rv;
}

/* ------------------------------------------------------------ creation */

static int
walk(const char *base, const char *rel, struct manifest *mf)
{
	char *dir = rel ? path_join(base, rel) : xstrdup(base);
	DIR *d;
	struct dirent *de;
	int rv = 0;

	if ((d = opendir(dir)) == NULL) {
		upkg_err("%s: %s", dir, strerror(errno));
		free(dir);
		return -1;
	}
	while (rv == 0 && (de = readdir(d)) != NULL) {
		struct stat st;
		char *r, *full;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		r = rel ? xasprintf("%s/%s", rel, de->d_name) :
		    xstrdup(de->d_name);
		full = path_join(base, r);
		if (!safe_relpath(r)) {
			upkg_err("unsafe path '%s' in staging tree", r);
			rv = -1;
		} else if (lstat(full, &st) == -1) {
			upkg_err("%s: %s", full, strerror(errno));
			rv = -1;
		} else if (S_ISDIR(st.st_mode)) {
			manifest_add(mf, M_DIR, r, st.st_mode & 07777, 0, NULL,
			    NULL);
			rv = walk(base, r, mf);
		} else if (S_ISREG(st.st_mode)) {
			char sum[SHA256_HEX_LEN + 1];
			off_t sz;

			if (st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) {
				upkg_err("%s: setuid/setgid/sticky files are "
				    "not permitted in packages", r);
				rv = -1;
			} else if (sha256_file(full, sum, &sz) == -1)
				rv = -1;
			else
				manifest_add(mf, M_FILE, r, st.st_mode & 0777,
				    (long long)sz, sum, NULL);
		} else if (S_ISLNK(st.st_mode)) {
			char tg[1025];
			ssize_t n = readlink(full, tg, sizeof(tg) - 1);

			if (n < 0) {
				upkg_err("%s: %s", full, strerror(errno));
				rv = -1;
			} else {
				tg[n] = '\0';
				manifest_add(mf, M_LINK, r, 0777, 0, NULL, tg);
			}
		} else {
			upkg_err("%s: unsupported file type", r);
			rv = -1;
		}
		free(r);
		free(full);
	}
	closedir(d);
	free(dir);
	return rv;
}

static int
add_member(struct archive *a, const char *name, const char *data, size_t len,
    time_t mtime)
{
	struct archive_entry *e = archive_entry_new();
	int rv = 0;

	archive_entry_set_pathname(e, name);
	archive_entry_set_filetype(e, AE_IFREG);
	archive_entry_set_perm(e, 0644);
	archive_entry_set_size(e, (la_int64_t)len);
	archive_entry_set_uid(e, 0);
	archive_entry_set_gid(e, 0);
	archive_entry_set_uname(e, "root");
	archive_entry_set_gname(e, "wheel");
	archive_entry_set_mtime(e, mtime, 0);
	if (archive_write_header(a, e) != ARCHIVE_OK ||
	    archive_write_data(a, data, len) != (la_ssize_t)len) {
		upkg_err("archive: %s", archive_error_string(a));
		rv = -1;
	}
	archive_entry_free(e);
	return rv;
}

int
pkgfile_create(const char *pkginfo_text, const char *stagedir,
    const char *outpath)
{
	struct pkgmeta m;
	struct manifest mf;
	struct archive *a = NULL;
	char *info = NULL, *man = NULL, *tmp = NULL;
	const char *sde = getenv("SOURCE_DATE_EPOCH");
	time_t mtime = sde ? (time_t)strtoll(sde, NULL, 10) : 0;
	size_t i;
	int rv = -1;

	memset(&mf, 0, sizeof(mf));
	if (pkgmeta_parse(pkginfo_text, &m, "PKGINFO") == -1)
		return -1;
	if (pkgmeta_validate(&m, "PKGINFO") == -1)
		goto out;
	if (m.filename || m.sha256 || m.size != -1) {
		upkg_err("PKGINFO: index-only keys are not allowed");
		goto out;
	}
	if (walk(stagedir, NULL, &mf) == -1)
		goto out;
	manifest_sort(&mf);
	for (i = 0; i < mf.n; i++)
		if (mf.e[i].type == M_LINK && (mf.e[i].target[0] == '/')) {
			upkg_err("%s: absolute symlink targets are not "
			    "permitted (-> %s)", mf.e[i].path, mf.e[i].target);
			goto out;
		}
	info = pkgmeta_format(&m, 0);
	man = manifest_format(&mf);

	tmp = xasprintf("%s.tmp", outpath);
	a = archive_write_new();
	archive_write_set_format_pax_restricted(a);
	archive_write_add_filter_xz(a);
	if (archive_write_open_filename(a, tmp) != ARCHIVE_OK) {
		upkg_err("%s: %s", tmp, archive_error_string(a));
		goto out;
	}
	if (add_member(a, UPKG_PKGINFO, info, strlen(info), mtime) == -1 ||
	    add_member(a, UPKG_MANIFEST, man, strlen(man), mtime) == -1)
		goto out;
	for (i = 0; i < mf.n; i++) {
		struct mentry *me = &mf.e[i];
		struct archive_entry *e = archive_entry_new();
		char *full = path_join(stagedir, me->path);
		int bad = 0;

		archive_entry_set_pathname(e, me->path);
		archive_entry_set_uid(e, 0);
		archive_entry_set_gid(e, 0);
		archive_entry_set_uname(e, "root");
		archive_entry_set_gname(e, "wheel");
		archive_entry_set_mtime(e, mtime, 0);
		switch (me->type) {
		case M_DIR:
			archive_entry_set_filetype(e, AE_IFDIR);
			archive_entry_set_perm(e, me->mode);
			bad = archive_write_header(a, e) != ARCHIVE_OK;
			break;
		case M_LINK:
			archive_entry_set_filetype(e, AE_IFLNK);
			archive_entry_set_perm(e, 0777);
			archive_entry_set_symlink(e, me->target);
			bad = archive_write_header(a, e) != ARCHIVE_OK;
			break;
		case M_FILE: {
			char buf[65536];
			ssize_t n;
			long long total = 0;
			int fd;

			archive_entry_set_filetype(e, AE_IFREG);
			archive_entry_set_perm(e, me->mode);
			archive_entry_set_size(e, me->size);
			if (archive_write_header(a, e) != ARCHIVE_OK) {
				bad = 1;
				break;
			}
			if ((fd = open(full, O_RDONLY | O_NOFOLLOW)) == -1) {
				upkg_err("%s: %s", full, strerror(errno));
				bad = 1;
				break;
			}
			while ((n = read(fd, buf, sizeof(buf))) > 0) {
				total += n;
				if (archive_write_data(a, buf, (size_t)n) != n) {
					bad = 1;
					break;
				}
			}
			close(fd);
			if (n < 0 || total != me->size) {
				upkg_err("%s: changed while packaging", full);
				bad = 1;
			}
			break;
		}
		}
		archive_entry_free(e);
		free(full);
		if (bad) {
			upkg_err("archive: %s", archive_error_string(a) ?
			    archive_error_string(a) : "write failed");
			goto out;
		}
	}
	if (archive_write_close(a) != ARCHIVE_OK) {
		upkg_err("archive: %s", archive_error_string(a));
		goto out;
	}
	if (rename(tmp, outpath) == -1) {
		upkg_err("rename %s: %s", outpath, strerror(errno));
		goto out;
	}
	upkg_msg("created %s (%zu entries)", outpath, mf.n);
	rv = 0;
out:
	if (a)
		archive_write_free(a);
	if (rv && tmp)
		unlink(tmp);
	free(tmp);
	free(info);
	free(man);
	manifest_free(&mf);
	pkgmeta_free(&m);
	return rv;
}
