/*
 * derfetch -- show DER/32 system information next to Genesis the tortoise.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Everything shown is read from the running system: uname(3), sysctl(3)
 * (kern.der32.*, kern.boottime, hw.*, machdep.cpu_brand, vm.uvmexp2),
 * statvfs(2) and the upkg database.  Development hosts without the
 * NetBSD interfaces fall back to /proc where possible.
 */

#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/utsname.h>
#ifdef __NetBSD__
#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#endif

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define DERFETCH_VERSION "0.1"
#define UPKG_DB "/var/lib/upkg/upkg.db"

/* Genesis, from share/fastfetch/der32.txt; '\1' outline, '\2' eye. */
static const char *logo[] = {
	"\1                   _.-''''''-._",
	"\1               .-'  \\________/  '-.",
	"\1             .'      |      |      '.",
	"\1   .---.    /--------'      '-------\\",
	"\1  /     \\  |      /  '-.__.-'  \\     |",
	"\1 |  \2--\1   '-|_____/______________\\____|_.",
	"\1  '-.________________________________ __)",
	"\1             |  |               |  |",
	"\1             '--'               '--'",
};
#define LOGO_LINES	(sizeof(logo) / sizeof(logo[0]))
#define LOGO_WIDTH	41

static int color = 1;

static const char *
c(const char *seq)
{
	return color ? seq : "";
}
#define RESET	c("\033[0m")
#define BOLD	c("\033[1m")
#define OUTLINE	c("\033[1;37m")
#define ACCENT	c("\033[1;33m")

struct line {
	char key[16];
	char val[160];
};

static struct line lines[24];
static size_t nlines;

static void
add(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
add(const char *key, const char *fmt, ...)
{
	va_list ap;

	if (nlines == sizeof(lines) / sizeof(lines[0]))
		return;
	snprintf(lines[nlines].key, sizeof(lines[nlines].key), "%s", key);
	va_start(ap, fmt);
	vsnprintf(lines[nlines].val, sizeof(lines[nlines].val), fmt, ap);
	va_end(ap);
	nlines++;
}

#ifdef __NetBSD__
static int
sysctl_str(const char *name, char *buf, size_t len)
{
	size_t l = len;

	if (sysctlbyname(name, buf, &l, NULL, 0) == -1)
		return -1;
	buf[len - 1] = '\0';
	return 0;
}
#endif

static void
human(unsigned long long b, char *out, size_t len)
{
	const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)b;
	int i = 0;

	while (v >= 1024 && i < 4) {
		v /= 1024;
		i++;
	}
	snprintf(out, len, i < 2 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

static void
do_os(const struct utsname *u)
{
#ifdef __NetBSD__
	char name[32], rel[32], code[32];

	if (sysctl_str("kern.der32.name", name, sizeof(name)) == 0 &&
	    sysctl_str("kern.der32.release", rel, sizeof(rel)) == 0 &&
	    sysctl_str("kern.der32.codename", code, sizeof(code)) == 0) {
		add("OS", "%s %s %s %s", name, rel, code, u->machine);
		add("Base", "NetBSD %s", u->release);
		return;
	}
#endif
	add("OS", "%s %s %s", u->sysname, u->release, u->machine);
}

static void
do_kernel(const struct utsname *u)
{
	/* "DER/32 0.1 GENESIS (DER32) #1: ..." -> config name and build */
	const char *o = strchr(u->version, '('), *e, *h;

	if (o && (e = strchr(o, ')')) != NULL) {
		h = strchr(e, '#');
		if (h) {
			const char *end = strchr(h, ':');
			int hl = end ? (int)(end - h) : (int)strcspn(h, " \n");

			add("Kernel", "%.*s %.*s", (int)(e - o - 1), o + 1,
			    hl, h);
			return;
		}
	}
	add("Kernel", "%s %s", u->sysname, u->release);
}

static void
do_uptime(void)
{
	long secs = -1;
#ifdef __NetBSD__
	struct timeval bt;
	size_t l = sizeof(bt);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };

	if (sysctl(mib, 2, &bt, &l, NULL, 0) == 0)
		secs = (long)(time(NULL) - bt.tv_sec);
#else
	FILE *f = fopen("/proc/uptime", "r");
	double up;

	if (f) {
		if (fscanf(f, "%lf", &up) == 1)
			secs = (long)up;
		fclose(f);
	}
#endif
	if (secs < 0)
		return;
	long d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60;
	if (d)
		add("Uptime", "%ld day%s, %ld h, %ld min", d, d == 1 ? "" : "s", h, m);
	else if (h)
		add("Uptime", "%ld h, %ld min", h, m);
	else
		add("Uptime", "%ld min", m);
}

static void
do_packages(void)
{
	sqlite3 *db;
	sqlite3_stmt *s;
	const char *root = getenv("UPKG_ROOT");
	char path[MAXPATHLEN];
	long n = -1;

	snprintf(path, sizeof(path), "%s%s", root ? root : "", UPKG_DB);
	if (access(path, R_OK) != 0) {
		add("Packages", "0 (upkg)");
		return;
	}
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
	    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM packages", -1, &s,
	    NULL) == SQLITE_OK) {
		if (sqlite3_step(s) == SQLITE_ROW)
			n = sqlite3_column_int64(s, 0);
		sqlite3_finalize(s);
	}
	sqlite3_close(db);
	if (n >= 0)
		add("Packages", "%ld (upkg)", n);
}

static void
do_shell(void)
{
	const char *sh = getenv("SHELL"), *b;
	struct passwd *pw;

	if (sh == NULL && (pw = getpwuid(getuid())) != NULL)
		sh = pw->pw_shell;
	if (sh == NULL)
		return;
	b = strrchr(sh, '/');
	add("Shell", "%s", b ? b + 1 : sh);
	if (getenv("TERM"))
		add("Terminal", "%s", getenv("TERM"));
}

static void
do_cpu(void)
{
	char model[128] = "";
	long n = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __NetBSD__
	if (sysctl_str("machdep.cpu_brand", model, sizeof(model)) == -1)
		sysctl_str("hw.model", model, sizeof(model));
#else
	FILE *f = fopen("/proc/cpuinfo", "r");
	char buf[256];

	while (f && fgets(buf, sizeof(buf), f))
		if (strncmp(buf, "model name", 10) == 0) {
			char *p = strchr(buf, ':');
			if (p)
				snprintf(model, sizeof(model), "%s", p + 2);
			model[strcspn(model, "\n")] = '\0';
			break;
		}
	if (f)
		fclose(f);
#endif
	/* collapse runs of spaces some CPUs pad their brand string with */
	char *r = model, *w = model;
	for (; *r; r++)
		if (!(*r == ' ' && (w == model || w[-1] == ' ')))
			*w++ = *r;
	*w = '\0';
	if (w > model && w[-1] == ' ')
		w[-1] = '\0';
	if (*model)
		add("CPU", "%s (%ld)", model, n > 0 ? n : 1L);
}

static void
do_memory(void)
{
	unsigned long long total = 0, used = 0;
	char a[32], b[32];
#ifdef __NetBSD__
	struct uvmexp_sysctl u;
	size_t l = sizeof(u);
	int mib[2] = { CTL_VM, VM_UVMEXP2 };

	if (sysctl(mib, 2, &u, &l, NULL, 0) == -1)
		return;
	total = (unsigned long long)u.npages * (unsigned long long)u.pagesize;
	used = (unsigned long long)(u.active + u.wired) *
	    (unsigned long long)u.pagesize;
#else
	FILE *f = fopen("/proc/meminfo", "r");
	char key[64];
	unsigned long long v, avail = 0;

	if (f == NULL)
		return;
	while (fscanf(f, "%63s %llu kB\n", key, &v) == 2) {
		if (strcmp(key, "MemTotal:") == 0)
			total = v * 1024;
		else if (strcmp(key, "MemAvailable:") == 0)
			avail = v * 1024;
	}
	fclose(f);
	used = total - avail;
#endif
	human(used, a, sizeof(a));
	human(total, b, sizeof(b));
	add("Memory", "%s / %s (%llu%%)", a, b, total ? used * 100 / total : 0);
}

static void
do_disk(void)
{
	struct statvfs s;
	char a[32], b[32];
	unsigned long long total, used;

	if (statvfs("/", &s) == -1)
		return;
	total = (unsigned long long)s.f_blocks * s.f_frsize;
	used = total - (unsigned long long)s.f_bfree * s.f_frsize;
	human(used, a, sizeof(a));
	human(total, b, sizeof(b));
	add("Disk (/)", "%s / %s (%llu%%)", a, b, total ? used * 100 / total : 0);
}

static void
print_logo_line(size_t i, int show)
{
	const char *p;
	int width = 0;

	if (!show)
		return;
	if (i >= LOGO_LINES) {
		printf("%*s", LOGO_WIDTH + 3, "");
		return;
	}
	for (p = logo[i]; *p; p++) {
		if (*p == '\1')
			fputs(OUTLINE, stdout);
		else if (*p == '\2')
			fputs(ACCENT, stdout);
		else {
			putchar(*p);
			width++;
		}
	}
	fputs(RESET, stdout);
	printf("%*s", LOGO_WIDTH - width + 3, "");
}

static void
usage(void)
{
	fprintf(stderr, "usage: derfetch [-lnV]\n"
	    "  -l  no logo    -n  no color    -V  version\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct utsname u;
	struct passwd *pw;
	char host[MAXHOSTNAMELEN], title[MAXHOSTNAMELEN + 64];
	int ch, show_logo = 1;
	size_t i, rows;

	while ((ch = getopt(argc, argv, "lnV")) != -1)
		switch (ch) {
		case 'l':
			show_logo = 0;
			break;
		case 'n':
			color = 0;
			break;
		case 'V':
			printf("derfetch %s\n", DERFETCH_VERSION);
			return 0;
		default:
			usage();
		}
	if (!isatty(STDOUT_FILENO) || getenv("NO_COLOR"))
		color = 0;

	if (uname(&u) == -1) {
		perror("derfetch: uname");
		return 1;
	}
	if (gethostname(host, sizeof(host)) == -1)
		snprintf(host, sizeof(host), "%.*s", (int)sizeof(host) - 1, u.nodename);
	host[sizeof(host) - 1] = '\0';
	pw = getpwuid(getuid());
	snprintf(title, sizeof(title), "%s@%s", pw ? pw->pw_name : "?", host);

	do_os(&u);
	do_kernel(&u);
	do_uptime();
	do_packages();
	do_shell();
	do_cpu();
	do_memory();
	do_disk();

	/* title, rule, then the info lines beside the logo */
	rows = nlines + 2 > LOGO_LINES ? nlines + 2 : LOGO_LINES;
	for (i = 0; i < rows; i++) {
		print_logo_line(i, show_logo);
		if (i == 0)
			printf("%s%s%s", ACCENT, title, RESET);
		else if (i == 1)
			for (size_t k = 0; k < strlen(title); k++)
				putchar('-');
		else if (i - 2 < nlines)
			printf("%s%s%s: %s", ACCENT, lines[i - 2].key, RESET,
			    lines[i - 2].val);
		putchar('\n');
	}
	return 0;
}
