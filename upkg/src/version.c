/*
 * upkg -- version comparison and dependency specifications.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Versions are compared segment by segment.  A segment is a maximal run of
 * digits (compared numerically) or letters (compared lexically); any other
 * character is a separator.  A numeric segment sorts after an alphabetic
 * one, and '~' sorts before everything (so "1.0~rc1" < "1.0").
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "upkg.h"

int
vercmp(const char *a, const char *b)
{
	while (*a || *b) {
		/* '~' marks a pre-release and sorts lowest */
		if (*a == '~' || *b == '~') {
			if (*a != '~')
				return 1;
			if (*b != '~')
				return -1;
			a++, b++;
			continue;
		}
		while (*a && !isalnum((unsigned char)*a) && *a != '~')
			a++;
		while (*b && !isalnum((unsigned char)*b) && *b != '~')
			b++;
		if (*a == '~' || *b == '~')
			continue;
		if (!*a || !*b)
			break;

		const char *sa = a, *sb = b;
		int num = !!isdigit((unsigned char)*a);

		if (num != !!isdigit((unsigned char)*b))
			return num ? 1 : -1;
		if (num) {
			while (*sa == '0')
				sa++;
			while (*sb == '0')
				sb++;
			a = sa;
			b = sb;
			while (isdigit((unsigned char)*a))
				a++;
			while (isdigit((unsigned char)*b))
				b++;
			size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
			if (la != lb)
				return la < lb ? -1 : 1;
			int c = memcmp(sa, sb, la);
			if (c)
				return c < 0 ? -1 : 1;
		} else {
			while (isalpha((unsigned char)*a))
				a++;
			while (isalpha((unsigned char)*b))
				b++;
			size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
			int c = strncmp(sa, sb, la < lb ? la : lb);
			if (c)
				return c < 0 ? -1 : 1;
			if (la != lb)
				return la < lb ? -1 : 1;
		}
	}
	if (!*a && !*b)
		return 0;
	/* the version with remaining segments is newer */
	return *a ? 1 : -1;
}

int
fullvercmp(const char *v1, unsigned r1, const char *v2, unsigned r2)
{
	int c = vercmp(v1, v2);

	if (c)
		return c;
	return r1 < r2 ? -1 : r1 > r2;
}

int
valid_pkgname(const char *s)
{
	const char *p;

	if (!isalnum((unsigned char)*s) || strlen(s) > 128)
		return 0;
	for (p = s; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("+._-", *p))
			return 0;
	return 1;
}

int
valid_version(const char *s)
{
	const char *p;

	if (!isalnum((unsigned char)*s) || strlen(s) > 64)
		return 0;
	for (p = s; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("+._~", *p))
			return 0;
	return 1;
}

const char *
dep_op_str(enum dep_op op)
{
	switch (op) {
	case DEP_LT: return "<";
	case DEP_LE: return "<=";
	case DEP_EQ: return "=";
	case DEP_GE: return ">=";
	case DEP_GT: return ">";
	default:     return "";
	}
}

/*
 * Parse "[repo/]name[op version[-release]]".
 */
int
depspec_parse(const char *spec, struct depspec *d, int allow_repo)
{
	const char *s = spec, *slash, *op;
	size_t nlen;

	memset(d, 0, sizeof(*d));
	slash = strchr(s, '/');
	if (slash) {
		if (!allow_repo)
			return -1;
		d->repo = xstrndup(s, (size_t)(slash - s));
		if (!valid_pkgname(d->repo))
			goto bad;
		s = slash + 1;
	}
	op = strpbrk(s, "<>=");
	nlen = op ? (size_t)(op - s) : strlen(s);
	d->name = xstrndup(s, nlen);
	if (!valid_pkgname(d->name))
		goto bad;
	if (op == NULL) {
		d->op = DEP_ANY;
		return 0;
	}
	if (strncmp(op, ">=", 2) == 0)
		d->op = DEP_GE, op += 2;
	else if (strncmp(op, "<=", 2) == 0)
		d->op = DEP_LE, op += 2;
	else if (*op == '>')
		d->op = DEP_GT, op++;
	else if (*op == '<')
		d->op = DEP_LT, op++;
	else if (*op == '=')
		d->op = DEP_EQ, op++;
	d->version = xstrdup(op);
	{
		char *dash = strchr(d->version, '-');
		char *ver = dash ? xstrndup(d->version, (size_t)(dash - d->version)) :
		    xstrdup(d->version);
		int ok = valid_version(ver);

		if (ok && dash) {
			const char *r = dash + 1;
			ok = *r != '\0';
			for (; *r; r++)
				if (!isdigit((unsigned char)*r))
					ok = 0;
		}
		free(ver);
		if (!ok)
			goto bad;
	}
	return 0;
bad:
	depspec_free(d);
	return -1;
}

void
depspec_free(struct depspec *d)
{
	free(d->repo);
	free(d->name);
	free(d->version);
	memset(d, 0, sizeof(*d));
}

int
depspec_match(const struct depspec *d, const char *version, unsigned release)
{
	int c;

	if (d->op == DEP_ANY)
		return 1;
	const char *dash = strchr(d->version, '-');
	if (dash) {
		char *v = xstrndup(d->version, (size_t)(dash - d->version));
		c = fullvercmp(version, release, v,
		    (unsigned)strtoul(dash + 1, NULL, 10));
		free(v);
	} else
		c = vercmp(version, d->version);
	switch (d->op) {
	case DEP_LT: return c < 0;
	case DEP_LE: return c <= 0;
	case DEP_EQ: return c == 0;
	case DEP_GE: return c >= 0;
	case DEP_GT: return c > 0;
	default:     return 1;
	}
}
