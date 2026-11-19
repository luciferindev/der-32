/*
 * upkg -- utility routines.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "upkg.h"

int upkg_verbose = 0;
volatile sig_atomic_t upkg_interrupted = 0;

static void
vmsg(const char *pfx, const char *fmt, va_list ap)
{
	fflush(stdout);
	fprintf(stderr, "upkg: %s", pfx);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void
upkg_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg("error: ", fmt, ap);
	va_end(ap);
}

void
upkg_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg("warning: ", fmt, ap);
	va_end(ap);
}

void
upkg_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	fputc('\n', stdout);
	fflush(stdout);
	va_end(ap);
}

void
upkg_debug(const char *fmt, ...)
{
	va_list ap;

	if (!upkg_verbose)
		return;
	va_start(ap, fmt);
	vmsg("debug: ", fmt, ap);
	va_end(ap);
}

void
upkg_die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg("fatal: ", fmt, ap);
	va_end(ap);
	exit(2);
}

void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (p == NULL)
		upkg_die("out of memory");
	return p;
}

void *
xcalloc(size_t n, size_t m)
{
	void *p = calloc(n ? n : 1, m ? m : 1);

	if (p == NULL)
		upkg_die("out of memory");
	return p;
}

void *
xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (p == NULL)
		upkg_die("out of memory");
	return p;
}

char *
xstrdup(const char *s)
{
	char *p = strdup(s);

	if (p == NULL)
		upkg_die("out of memory");
	return p;
}

char *
xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

char *
xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *p;

	va_start(ap, fmt);
	if (vasprintf(&p, fmt, ap) < 0)
		upkg_die("out of memory");
	va_end(ap);
	return p;
}

void
sl_init(struct strlist *sl)
{
	sl->v = NULL;
	sl->n = sl->cap = 0;
}

void
sl_addn(struct strlist *sl, char *s)
{
	if (sl->n + 1 >= sl->cap) {
		sl->cap = sl->cap ? sl->cap * 2 : 8;
		sl->v = xrealloc(sl->v, sl->cap * sizeof(*sl->v));
	}
	sl->v[sl->n++] = s;
	sl->v[sl->n] = NULL;
}

void
sl_add(struct strlist *sl, const char *s)
{
	sl_addn(sl, xstrdup(s));
}

int
sl_contains(const struct strlist *sl, const char *s)
{
	size_t i;

	for (i = 0; i < sl->n; i++)
		if (strcmp(sl->v[i], s) == 0)
			return 1;
	return 0;
}

void
sl_free(struct strlist *sl)
{
	size_t i;

	for (i = 0; i < sl->n; i++)
		free(sl->v[i]);
	free(sl->v);
	sl_init(sl);
}

char *
trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

char *
path_join(const char *a, const char *b)
{
	size_t la = strlen(a);

	while (*b == '/')
		b++;
	if (la > 0 && a[la - 1] == '/')
		return xasprintf("%s%s", a, b);
	return xasprintf("%s/%s", a, b);
}

int
mkdir_p(const char *path, mode_t mode)
{
	char *p, *s;
	struct stat st;

	p = xstrdup(path);
	for (s = p + 1; ; s++) {
		if (*s != '/' && *s != '\0')
			continue;
		char c = *s;
		*s = '\0';
		if (mkdir(p, mode) == -1 && errno != EEXIST) {
			upkg_err("mkdir %s: %s", p, strerror(errno));
			free(p);
			return -1;
		}
		if (stat(p, &st) == -1 || !S_ISDIR(st.st_mode)) {
			upkg_err("%s: not a directory", p);
			free(p);
			return -1;
		}
		*s = c;
		if (c == '\0')
			break;
	}
	free(p);
	return 0;
}

int
read_file(const char *path, char **buf, size_t *len)
{
	int fd;
	struct stat st;
	size_t cap, n = 0;
	ssize_t r;
	char *b;

	if ((fd = open(path, O_RDONLY)) == -1)
		return -1;
	cap = (fstat(fd, &st) == 0 && st.st_size > 0) ?
	    (size_t)st.st_size + 1 : 4096;
	b = xmalloc(cap);
	for (;;) {
		if (n + 1 >= cap) {
			cap *= 2;
			b = xrealloc(b, cap);
		}
		r = read(fd, b + n, cap - n - 1);
		if (r == -1) {
			if (errno == EINTR)
				continue;
			free(b);
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	b[n] = '\0';
	*buf = b;
	if (len)
		*len = n;
	return 0;
}

static int
write_all(int fd, const char *data, size_t len)
{
	ssize_t w;

	while (len > 0) {
		w = write(fd, data, len);
		if (w == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += w;
		len -= (size_t)w;
	}
	return 0;
}

int
write_file_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
	char *tmp = xasprintf("%s.tmp.%ld", path, (long)getpid());
	int fd;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
	if (fd == -1) {
		upkg_err("cannot create %s: %s", tmp, strerror(errno));
		free(tmp);
		return -1;
	}
	if (write_all(fd, data, len) == -1 || fsync(fd) == -1) {
		upkg_err("cannot write %s: %s", tmp, strerror(errno));
		close(fd);
		unlink(tmp);
		free(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) == -1) {
		upkg_err("cannot rename %s: %s", tmp, strerror(errno));
		unlink(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

int
copy_file(const char *src, const char *dst)
{
	char *buf;
	size_t len;
	int rv;

	if (read_file(src, &buf, &len) == -1) {
		upkg_err("cannot read %s: %s", src, strerror(errno));
		return -1;
	}
	rv = write_file_atomic(dst, buf, len, 0644);
	free(buf);
	return rv;
}

int
is_hex(const char *s, size_t len)
{
	size_t i;

	if (strlen(s) != len)
		return 0;
	for (i = 0; i < len; i++)
		if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i]))
			return 0;
	return 1;
}

void
hexencode(const unsigned char *in, size_t n, char *out)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hx[in[i] >> 4];
		out[2 * i + 1] = hx[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int
hexdecode(const char *s, unsigned char *out, size_t n)
{
	size_t i;
	int hi, lo;

	if (strlen(s) != 2 * n)
		return -1;
	for (i = 0; i < n; i++) {
		hi = hexval((unsigned char)s[2 * i]);
		lo = hexval((unsigned char)s[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}

/* SHA-256 through OpenSSL's EVP interface. */

struct sha256_ctx {
	EVP_MD_CTX *md;
};

struct sha256_ctx *
sha256_new(void)
{
	struct sha256_ctx *c = xmalloc(sizeof(*c));

	if ((c->md = EVP_MD_CTX_new()) == NULL ||
	    EVP_DigestInit_ex(c->md, EVP_sha256(), NULL) != 1)
		upkg_die("EVP SHA-256 initialisation failed");
	return c;
}

void
sha256_update(struct sha256_ctx *c, const void *p, size_t n)
{
	if (EVP_DigestUpdate(c->md, p, n) != 1)
		upkg_die("EVP SHA-256 update failed");
}

void
sha256_final_hex(struct sha256_ctx *c, char out[SHA256_HEX_LEN + 1])
{
	unsigned char d[EVP_MAX_MD_SIZE];
	unsigned int n = 0;

	if (EVP_DigestFinal_ex(c->md, d, &n) != 1 || n != 32)
		upkg_die("EVP SHA-256 final failed");
	hexencode(d, 32, out);
	EVP_MD_CTX_free(c->md);
	free(c);
}

int
sha256_file(const char *path, char out[SHA256_HEX_LEN + 1], off_t *size)
{
	struct sha256_ctx *c;
	char buf[65536];
	ssize_t r;
	off_t total = 0;
	int fd;

	if ((fd = open(path, O_RDONLY)) == -1) {
		upkg_err("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	c = sha256_new();
	while ((r = read(fd, buf, sizeof(buf))) != 0) {
		if (r == -1) {
			if (errno == EINTR)
				continue;
			upkg_err("read %s: %s", path, strerror(errno));
			close(fd);
			sha256_final_hex(c, out);
			return -1;
		}
		sha256_update(c, buf, (size_t)r);
		total += r;
	}
	close(fd);
	sha256_final_hex(c, out);
	if (size)
		*size = total;
	return 0;
}
