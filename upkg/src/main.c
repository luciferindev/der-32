/*
 * upkg -- the DER/32 Unix Package Manager: command line interface.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/stat.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "upkg.h"

static void
usage(FILE *f)
{
	fprintf(f,
"usage: upkg [-v] [-r root] command [arguments]\n"
"\n"
"Packages:\n"
"  sync                         download and verify repository indexes\n"
"  install [-n] [--asdeps] [--nodeps] [--allow-unsigned] pkg ...\n"
"                               install packages (pkg, repo/pkg, pkg>=ver,\n"
"                               or a local .upkg file)\n"
"  remove [-n] [-c] pkg ...     remove packages (-c: also their dependents)\n"
"  autoremove [-n]              remove orphaned dependencies\n"
"  upgrade [-n]                 upgrade all installed packages\n"
"  search query                 search repository packages\n"
"  info pkg                     show package details\n"
"  list [-e|-d] [-f pkg]        list installed packages (or a package's files)\n"
"  check [pkg ...]              verify installed files against the database\n"
"  build [-o out] port          build a package from a source recipe\n"
"\n"
"Repositories:\n"
"  repo list\n"
"  repo add name url [-p priority] [-k key] [--disabled]\n"
"  repo remove name\n"
"  repo enable|disable name\n"
"  repo priority name n\n"
"\n"
"Publishing:\n"
"  keygen prefix                create an Ed25519 signing key pair\n"
"  create -i PKGINFO -d stagedir [-o file.upkg]\n"
"  index -n repo -k key.key dir sign and write dir/INDEX\n"
"\n"
"  recover                      complete or undo an interrupted transaction\n"
"  vercmp a b                   compare two versions (prints -1, 0 or 1)\n"
"  --version                    print the upkg version\n");
}

static void
on_signal(int sig)
{
	(void)sig;
	upkg_interrupted = 1;
}

static void
setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* Split command arguments into options (as listed) and operands. */
static int
take_opt(int *argc, char **argv, const char *name)
{
	int i, j;

	for (i = 1; i < *argc; i++)
		if (strcmp(argv[i], name) == 0) {
			for (j = i; j < *argc; j++)
				argv[j] = argv[j + 1];
			(*argc)--;
			return 1;
		}
	return 0;
}

static char *
take_optarg(int *argc, char **argv, const char *name)
{
	int i, j;
	char *v;

	for (i = 1; i < *argc - 1; i++)
		if (strcmp(argv[i], name) == 0) {
			v = argv[i + 1];
			for (j = i; j + 2 <= *argc; j++)
				argv[j] = argv[j + 2];
			*argc -= 2;
			return v;
		}
	return NULL;
}

static int
no_unknown_opts(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (argv[i][0] == '-' && argv[i][1] != '\0') {
			upkg_err("unknown option '%s' for '%s'", argv[i], argv[0]);
			return -1;
		}
	return 0;
}

static int
cmd_sync(struct config *cfg)
{
	size_t i;
	int bad = 0, n = 0;

	if (lock_acquire(cfg) == -1)
		return 1;
	if (cfg->nrepos == 0) {
		upkg_err("no repositories configured (see 'upkg repo add')");
		return 1;
	}
	upkg_msg(":: synchronising repository indexes");
	for (i = 0; i < cfg->nrepos; i++) {
		if (!cfg->repos[i].enabled) {
			upkg_msg(":: %-12s disabled", cfg->repos[i].name);
			continue;
		}
		n++;
		if (repo_sync(cfg, &cfg->repos[i]) == -1)
			bad = 1;
		if (upkg_interrupted)
			return 1;
	}
	return bad ? 1 : 0;
}

static void
print_meta(const struct pkgmeta *m, const char *where)
{
	size_t i;

	printf("Name         : %s\n", m->name);
	printf("Version      : %s-%u\n", m->version, m->release);
	printf("Architecture : %s\n", m->arch);
	printf("Repository   : %s\n", where);
	printf("Description  : %s\n", m->description ? m->description : "");
	if (m->license)
		printf("License      : %s\n", m->license);
	if (m->maintainer)
		printf("Maintainer   : %s\n", m->maintainer);
	printf("Depends on   :");
	for (i = 0; i < m->depends.n; i++)
		printf(" %s", m->depends.v[i]);
	printf("%s\n", m->depends.n ? "" : " (none)");
	printf("Conflicts    :");
	for (i = 0; i < m->conflicts.n; i++)
		printf(" %s", m->conflicts.v[i]);
	printf("%s\n", m->conflicts.n ? "" : " (none)");
	if (m->filename) {
		printf("Download     : %s (%lld bytes)\n", m->filename, m->size);
		printf("SHA-256      : %s\n", m->sha256);
	}
}

static int
cmd_info(struct config *cfg, const char *spec)
{
	struct db *db = NULL;
	struct instpkg ip;
	struct depspec d;
	size_t i, j;
	int shown = 0, f = 0;

	if (depspec_parse(spec, &d, 1) == -1) {
		upkg_err("invalid package name '%s'", spec);
		return 1;
	}
	if (db_open(cfg, &db, 0) == 0 && !d.repo &&
	    (f = db_get_pkg(db, d.name, &ip)) == 1) {
		struct tm tm;
		char tbuf[64];
		time_t t = (time_t)ip.installed;

		localtime_r(&t, &tm);
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
		printf("Name         : %s\n", ip.name);
		printf("Version      : %s-%u\n", ip.version, ip.release);
		printf("Architecture : %s\n", ip.arch);
		printf("Repository   : %s\n", ip.repo ? ip.repo : "-");
		printf("Description  : %s\n", ip.description ? ip.description : "");
		printf("Install date : %s\n", tbuf);
		printf("Install reason: %s\n", ip.reason);
		printf("Depends on   :");
		for (i = 0; i < ip.depends.n; i++)
			printf(" %s", ip.depends.v[i]);
		printf("%s\n\n", ip.depends.n ? "" : " (none)");
		instpkg_free(&ip);
		shown = 1;
	}
	repo_load_all(cfg);
	for (i = 0; i < cfg->nrepos; i++) {
		struct repo *r = &cfg->repos[i];

		if (!r->loaded || (d.repo && strcmp(d.repo, r->name)))
			continue;
		for (j = 0; j < r->npkgs; j++)
			if (strcmp(r->pkgs[j].name, d.name) == 0) {
				print_meta(&r->pkgs[j], r->name);
				printf("\n");
				shown = 1;
			}
	}
	db_close(db);
	depspec_free(&d);
	if (!shown) {
		upkg_err("package '%s' not found", spec);
		return 1;
	}
	(void)f;
	return 0;
}

static int
cmd_search(struct config *cfg, const char *q)
{
	struct db *db = NULL;
	size_t i, j;
	int hits = 0;

	db_open(cfg, &db, 0);
	repo_load_all(cfg);
	for (i = 0; i < cfg->nrepos; i++) {
		struct repo *r = &cfg->repos[i];

		if (!r->loaded)
			continue;
		for (j = 0; j < r->npkgs; j++) {
			struct pkgmeta *m = &r->pkgs[j];
			struct instpkg ip;
			const char *tag = "";

			if (strstr(m->name, q) == NULL &&
			    (m->description == NULL || strstr(m->description, q) == NULL))
				continue;
			if (db && db_get_pkg(db, m->name, &ip) == 1) {
				tag = (strcmp(ip.version, m->version) == 0 &&
				    ip.release == m->release) ? " [installed]" :
				    " [installed: other version]";
				instpkg_free(&ip);
			}
			printf("%s/%s %s-%u%s\n    %s\n", r->name, m->name,
			    m->version, m->release, tag,
			    m->description ? m->description : "");
			hits++;
		}
	}
	db_close(db);
	return hits ? 0 : 1;
}

static int
cmd_list(struct config *cfg, int argc, char **argv)
{
	struct db *db;
	struct instpkg *v;
	size_t n, i;
	char *files = take_optarg(&argc, argv, "-f");
	int only_e = take_opt(&argc, argv, "-e");
	int only_d = take_opt(&argc, argv, "-d");

	if (no_unknown_opts(argc, argv) == -1 || argc != 1)
		return 2;
	if (db_open(cfg, &db, 0) == -1) {
		/* no database yet: nothing installed */
		return files ? 1 : 0;
	}
	if (files) {
		struct manifest mf;
		struct strlist dirs;
		struct instpkg ip;

		if (db_get_pkg(db, files, &ip) != 1) {
			upkg_err("%s is not installed", files);
			db_close(db);
			return 1;
		}
		instpkg_free(&ip);
		db_pkg_files(db, files, &mf);
		db_pkg_dirs(db, files, &dirs);
		for (i = dirs.n; i-- > 0; )
			printf("/%s/\n", dirs.v[i]);
		for (i = 0; i < mf.n; i++)
			printf("/%s%s%s\n", mf.e[i].path,
			    mf.e[i].type == M_LINK ? " -> " : "",
			    mf.e[i].type == M_LINK ? mf.e[i].target : "");
		manifest_free(&mf);
		sl_free(&dirs);
		db_close(db);
		return 0;
	}
	if (db_list_pkgs(db, &v, &n) == -1) {
		db_close(db);
		return 1;
	}
	for (i = 0; i < n; i++) {
		int e = strcmp(v[i].reason, "explicit") == 0;

		if ((only_e && !e) || (only_d && e)) {
			instpkg_free(&v[i]);
			continue;
		}
		printf("%-24s %s-%u%s\n", v[i].name, v[i].version, v[i].release,
		    e ? "" : "  (dependency)");
		instpkg_free(&v[i]);
	}
	free(v);
	db_close(db);
	return 0;
}

static int
cmd_check(struct config *cfg, int argc, char **argv)
{
	struct db *db;
	struct instpkg *v;
	size_t n, i, j;
	int bad = 0, k;

	if (db_open(cfg, &db, 0) == -1)
		return 1;
	if (db_list_pkgs(db, &v, &n) == -1)
		return 1;
	for (i = 0; i < n; i++) {
		struct manifest mf;
		int sel = argc <= 1;

		for (k = 1; k < argc; k++)
			if (strcmp(argv[k], v[i].name) == 0)
				sel = 1;
		if (sel) {
			db_pkg_files(db, v[i].name, &mf);
			for (j = 0; j < mf.n; j++) {
				char *a = path_join(cfg->root, mf.e[j].path);
				char sum[SHA256_HEX_LEN + 1], tg[1025];
				struct stat st;

				if (lstat(a, &st) == -1) {
					printf("%s: missing /%s\n", v[i].name,
					    mf.e[j].path);
					bad = 1;
				} else if (mf.e[j].type == M_FILE) {
					if (!S_ISREG(st.st_mode) ||
					    sha256_file(a, sum, NULL) == -1 ||
					    strcmp(sum, mf.e[j].sha256) != 0) {
						printf("%s: modified /%s\n",
						    v[i].name, mf.e[j].path);
						bad = 1;
					}
				} else {
					ssize_t l = readlink(a, tg, sizeof(tg) - 1);
					if (l < 0 || (tg[l] = '\0',
					    strcmp(tg, mf.e[j].target) != 0)) {
						printf("%s: modified link /%s\n",
						    v[i].name, mf.e[j].path);
						bad = 1;
					}
				}
				free(a);
			}
			manifest_free(&mf);
		}
		instpkg_free(&v[i]);
	}
	free(v);
	db_close(db);
	if (!bad)
		upkg_msg(":: all installed files verified");
	return bad;
}

static int
cmd_repo(struct config *cfg, int argc, char **argv)
{
	struct repo *r;
	size_t i;

	if (argc < 2 || strcmp(argv[1], "list") == 0) {
		if (cfg->nrepos == 0)
			upkg_msg("no repositories configured");
		for (i = 0; i < cfg->nrepos; i++) {
			r = &cfg->repos[i];
			printf("%-12s prio %-5d %-8s %s%s%s\n", r->name,
			    r->priority, r->enabled ? "enabled" : "disabled",
			    r->url, r->key ? "  key=" : "", r->key ? r->key : "");
		}
		return 0;
	}
	if (lock_acquire(cfg) == -1)
		return 1;
	if (strcmp(argv[1], "add") == 0) {
		char *prio = take_optarg(&argc, argv, "-p");
		char *key = take_optarg(&argc, argv, "-k");
		int disabled = take_opt(&argc, argv, "--disabled");

		if (no_unknown_opts(argc, argv) == -1 || argc != 4) {
			usage(stderr);
			return 2;
		}
		if (!valid_pkgname(argv[2])) {
			upkg_err("invalid repository name '%s'", argv[2]);
			return 1;
		}
		if (config_repo(cfg, argv[2])) {
			upkg_err("repository '%s' already exists", argv[2]);
			return 1;
		}
		if (strncmp(argv[3], "https://", 8) && strncmp(argv[3], "file://", 7) &&
		    argv[3][0] != '/' && !(cfg->allow_http &&
		    strncmp(argv[3], "http://", 7) == 0)) {
			upkg_err("unsupported repository URL '%s'", argv[3]);
			return 1;
		}
		if (key && !valid_pkgname(key)) {
			upkg_err("invalid key name '%s'", key);
			return 1;
		}
		cfg->repos = xrealloc(cfg->repos, (cfg->nrepos + 1) *
		    sizeof(*cfg->repos));
		r = &cfg->repos[cfg->nrepos++];
		memset(r, 0, sizeof(*r));
		r->name = xstrdup(argv[2]);
		r->url = xstrdup(argv[3]);
		r->priority = prio ? atoi(prio) : 0;
		r->enabled = !disabled;
		r->key = key ? xstrdup(key) : NULL;
		if (config_save_repos(cfg) == -1)
			return 1;
		upkg_msg("added repository '%s'; run 'upkg sync'", argv[2]);
		return 0;
	}
	if (argc < 3) {
		usage(stderr);
		return 2;
	}
	if ((r = config_repo(cfg, argv[2])) == NULL) {
		upkg_err("no repository named '%s'", argv[2]);
		return 1;
	}
	if (strcmp(argv[1], "remove") == 0) {
		char *p = xasprintf("%s/sync/%s.INDEX", cfg->dbdir, r->name);
		char *s = xasprintf("%s.sig", p);

		unlink(p);
		unlink(s);
		free(p);
		free(s);
		free(r->name);
		free(r->url);
		free(r->key);
		for (i = 0; i < r->npkgs; i++)
			pkgmeta_free(&r->pkgs[i]);
		free(r->pkgs);
		i = (size_t)(r - cfg->repos);
		memmove(r, r + 1, (cfg->nrepos - i - 1) * sizeof(*r));
		cfg->nrepos--;
		if (config_save_repos(cfg) == -1)
			return 1;
		upkg_msg("removed repository '%s'", argv[2]);
		return 0;
	}
	if (strcmp(argv[1], "enable") == 0 || strcmp(argv[1], "disable") == 0) {
		r->enabled = argv[1][0] == 'e';
		return config_save_repos(cfg) == -1;
	}
	if (strcmp(argv[1], "priority") == 0 && argc == 4) {
		r->priority = atoi(argv[3]);
		return config_save_repos(cfg) == -1;
	}
	usage(stderr);
	return 2;
}

int
main(int argc, char **argv)
{
	struct config cfg;
	struct opts o;
	const char *root = getenv("UPKG_ROOT");
	int rv;

	memset(&o, 0, sizeof(o));
	argv++, argc--;
	while (argc > 0 && argv[0][0] == '-') {
		if (strcmp(argv[0], "--version") == 0 || strcmp(argv[0], "-V") == 0) {
			printf("upkg %s\n", UPKG_VERSION);
			return 0;
		} else if (strcmp(argv[0], "-h") == 0 ||
		    strcmp(argv[0], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (strcmp(argv[0], "-v") == 0) {
			upkg_verbose = 1;
		} else if (strcmp(argv[0], "-r") == 0 && argc > 1) {
			root = argv[1];
			argv++, argc--;
		} else {
			upkg_err("unknown option '%s'", argv[0]);
			usage(stderr);
			return 2;
		}
		argv++, argc--;
	}
	if (argc == 0) {
		usage(stderr);
		return 2;
	}
	setup_signals();
	umask(022);

	const char *cmd = argv[0];

	/* commands that need no configuration */
	if (strcmp(cmd, "help") == 0) {
		usage(stdout);
		return 0;
	}
	if (strcmp(cmd, "version") == 0) {
		printf("upkg %s\n", UPKG_VERSION);
		return 0;
	}
	if (strcmp(cmd, "vercmp") == 0) {
		int c;

		if (argc != 3) {
			usage(stderr);
			return 2;
		}
		c = vercmp(argv[1], argv[2]);
		printf("%d\n", c < 0 ? -1 : c > 0);
		return 0;
	}
	if (strcmp(cmd, "keygen") == 0) {
		if (argc != 2) {
			usage(stderr);
			return 2;
		}
		return sig_keygen(argv[1]) == 0 ? 0 : 1;
	}
	if (strcmp(cmd, "create") == 0) {
		char *info = take_optarg(&argc, argv, "-i");
		char *dir = take_optarg(&argc, argv, "-d");
		char *out = take_optarg(&argc, argv, "-o");
		char *text, *def = NULL;
		struct pkgmeta m;

		if (!info || !dir || argc != 1 || no_unknown_opts(argc, argv)) {
			usage(stderr);
			return 2;
		}
		if (read_file(info, &text, NULL) == -1) {
			upkg_err("%s: %s", info, strerror(errno));
			return 1;
		}
		if (out == NULL) {
			if (pkgmeta_parse(text, &m, info) == -1 ||
			    pkgmeta_validate(&m, info) == -1) {
				free(text);
				return 1;
			}
			out = def = pkgmeta_filename(&m);
			pkgmeta_free(&m);
		}
		rv = pkgfile_create(text, dir, out);
		free(text);
		free(def);
		return rv == 0 ? 0 : 1;
	}
	if (strcmp(cmd, "index") == 0) {
		char *name = take_optarg(&argc, argv, "-n");
		char *key = take_optarg(&argc, argv, "-k");

		if (!name || !key || argc != 2 || no_unknown_opts(argc, argv)) {
			usage(stderr);
			return 2;
		}
		return repo_build_index(argv[1], name, key) == 0 ? 0 : 1;
	}

	if (config_load(&cfg, root) == -1)
		return 1;
	rv = 2;
	if (strcmp(cmd, "sync") == 0) {
		rv = cmd_sync(&cfg);
	} else if (strcmp(cmd, "install") == 0 || strcmp(cmd, "remove") == 0 ||
	    strcmp(cmd, "autoremove") == 0 || strcmp(cmd, "upgrade") == 0) {
		o.dry_run = take_opt(&argc, argv, "-n");
		o.cascade = take_opt(&argc, argv, "-c");
		o.allow_unsigned = take_opt(&argc, argv, "--allow-unsigned");
		o.as_deps = take_opt(&argc, argv, "--asdeps");
		o.nodeps = take_opt(&argc, argv, "--nodeps");
		if (no_unknown_opts(argc, argv) == -1) {
			rv = 2;
		} else if (cmd[0] == 'i') {
			rv = argc < 2 ? 2 :
			    op_install(&cfg, argv + 1, argc - 1, &o) ? 1 : 0;
		} else if (cmd[0] == 'r') {
			rv = argc < 2 ? 2 :
			    op_remove(&cfg, argv + 1, argc - 1, &o) ? 1 : 0;
		} else if (cmd[0] == 'a') {
			rv = op_autoremove(&cfg, &o) ? 1 : 0;
		} else {
			rv = op_upgrade(&cfg, &o) ? 1 : 0;
		}
		if (rv == 2)
			usage(stderr);
	} else if (strcmp(cmd, "search") == 0 && argc == 2) {
		rv = cmd_search(&cfg, argv[1]);
	} else if (strcmp(cmd, "info") == 0 && argc == 2) {
		rv = cmd_info(&cfg, argv[1]);
	} else if (strcmp(cmd, "list") == 0) {
		rv = cmd_list(&cfg, argc, argv);
	} else if (strcmp(cmd, "check") == 0) {
		rv = cmd_check(&cfg, argc, argv);
	} else if (strcmp(cmd, "repo") == 0) {
		rv = cmd_repo(&cfg, argc, argv);
	} else if (strcmp(cmd, "build") == 0) {
		o.output = take_optarg(&argc, argv, "-o");
		if (argc != 2 || no_unknown_opts(argc, argv)) {
			usage(stderr);
			rv = 2;
		} else
			rv = op_build(&cfg, argv[1], &o) ? 1 : 0;
	} else if (strcmp(cmd, "recover") == 0) {
		struct db *db;

		rv = 1;
		if (lock_acquire(&cfg) == 0 && db_open(&cfg, &db, 1) == 0) {
			rv = txn_recover(&cfg, db) ? 1 : 0;
			db_close(db);
			if (rv == 0)
				upkg_msg(":: database consistent");
		}
	} else {
		upkg_err("unknown command or wrong arguments: '%s'", cmd);
		usage(stderr);
	}
	lock_release();
	config_free(&cfg);
	return rv;
}
