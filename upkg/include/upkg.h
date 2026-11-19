/*
 * upkg -- the DER/32 Unix Package Manager.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UPKG_H
#define UPKG_H

#include <sys/types.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>

#define UPKG_VERSION		"0.1"
#define UPKG_PKG_FORMAT		1	/* +PKGINFO / +MANIFEST schema version */
#define UPKG_INDEX_FORMAT	1	/* repository INDEX schema version */
#define UPKG_DB_SCHEMA		1	/* installed package database schema */

#define UPKG_PKGINFO		"+PKGINFO"
#define UPKG_MANIFEST		"+MANIFEST"
#define UPKG_STAGE_SUFFIX	".upkg-new"

#ifndef UPKG_DEFAULT_PREFIX
#define UPKG_DEFAULT_PREFIX	"usr/local"
#endif

/* ---------------------------------------------------------------- util.c */

struct strlist {
	char	**v;
	size_t	  n;
	size_t	  cap;
};

extern int upkg_verbose;
extern volatile sig_atomic_t upkg_interrupted;

void	 upkg_err(const char *, ...) __attribute__((format(printf, 1, 2)));
void	 upkg_warn(const char *, ...) __attribute__((format(printf, 1, 2)));
void	 upkg_msg(const char *, ...) __attribute__((format(printf, 1, 2)));
void	 upkg_debug(const char *, ...) __attribute__((format(printf, 1, 2)));
void	 upkg_die(const char *, ...) __attribute__((format(printf, 1, 2), noreturn));

void	*xmalloc(size_t);
void	*xcalloc(size_t, size_t);
void	*xrealloc(void *, size_t);
char	*xstrdup(const char *);
char	*xstrndup(const char *, size_t);
char	*xasprintf(const char *, ...) __attribute__((format(printf, 1, 2)));

void	 sl_init(struct strlist *);
void	 sl_add(struct strlist *, const char *);
void	 sl_addn(struct strlist *, char *);	/* takes ownership */
int	 sl_contains(const struct strlist *, const char *);
void	 sl_free(struct strlist *);

char	*trim(char *);
char	*path_join(const char *, const char *);
int	 mkdir_p(const char *, mode_t);
int	 read_file(const char *, char **, size_t *);
int	 write_file_atomic(const char *, const char *, size_t, mode_t);
int	 copy_file(const char *, const char *);
int	 is_hex(const char *, size_t);
void	 hexencode(const unsigned char *, size_t, char *);
int	 hexdecode(const char *, unsigned char *, size_t);

/* ---------------------------------------------------------------- hash */

#define SHA256_HEX_LEN	64

struct sha256_ctx;
struct sha256_ctx *sha256_new(void);
void	 sha256_update(struct sha256_ctx *, const void *, size_t);
void	 sha256_final_hex(struct sha256_ctx *, char out[SHA256_HEX_LEN + 1]);
int	 sha256_file(const char *, char out[SHA256_HEX_LEN + 1], off_t *);

/* ---------------------------------------------------------------- sig.c */

int	 sig_keygen(const char *prefix);
int	 sig_sign(const char *keyfile, const char *data, size_t len, char **sighex);
int	 sig_verify_dir(const char *keydir, const char *keyname,
	    const char *data, size_t len, const char *sighex, char **signer);

/* ---------------------------------------------------------------- version.c */

enum dep_op { DEP_ANY = 0, DEP_LT, DEP_LE, DEP_EQ, DEP_GE, DEP_GT };

struct depspec {
	char		*repo;		/* optional explicit repository */
	char		*name;
	enum dep_op	 op;
	char		*version;	/* "1.2" or "1.2-3" */
};

int	 vercmp(const char *, const char *);
int	 fullvercmp(const char *v1, unsigned r1, const char *v2, unsigned r2);
int	 depspec_parse(const char *, struct depspec *, int allow_repo);
void	 depspec_free(struct depspec *);
int	 depspec_match(const struct depspec *, const char *version, unsigned release);
const char *dep_op_str(enum dep_op);
int	 valid_pkgname(const char *);
int	 valid_version(const char *);

/* ---------------------------------------------------------------- meta.c */

struct repo;

struct pkgmeta {
	char		*name;
	char		*version;
	unsigned	 release;
	char		*arch;
	char		*description;
	char		*license;
	char		*maintainer;
	char		*origin;
	struct strlist	 depends;
	struct strlist	 conflicts;
	/* repository index only */
	char		*filename;
	char		*sha256;
	long long	 size;
	struct repo	*repo;
};

enum mtype { M_FILE = 'f', M_DIR = 'd', M_LINK = 'l' };

struct mentry {
	enum mtype	 type;
	char		*path;
	mode_t		 mode;
	long long	 size;
	char		*sha256;
	char		*target;
	int		 seen;
};

struct manifest {
	struct mentry	*e;
	size_t		 n;
	size_t		 cap;
};

void	 pkgmeta_free(struct pkgmeta *);
int	 pkgmeta_parse(const char *text, struct pkgmeta *, const char *what);
char	*pkgmeta_format(const struct pkgmeta *, int with_index_fields);
int	 pkgmeta_validate(const struct pkgmeta *, const char *what);
char	*pkgmeta_fullver(const struct pkgmeta *);
char	*pkgmeta_filename(const struct pkgmeta *);

void	 manifest_free(struct manifest *);
int	 manifest_parse(const char *text, struct manifest *);
char	*manifest_format(const struct manifest *);
struct mentry *manifest_find(struct manifest *, const char *);
void	 manifest_sort(struct manifest *);
int	 manifest_add(struct manifest *, enum mtype, const char *path, mode_t,
	    long long size, const char *sha256, const char *target);

int	 safe_relpath(const char *);
int	 path_in_prefixes(const char *, const struct strlist *);
int	 link_target_ok(const char *linkpath, const char *target,
	    const struct strlist *prefixes);

/* ---------------------------------------------------------------- config.c */

struct repo {
	char		*name;
	char		*url;
	int		 priority;
	int		 enabled;
	char		*key;		/* restrict to this key name, or NULL */
	struct pkgmeta	*pkgs;
	size_t		 npkgs;
	long long	 timestamp;
	int		 loaded;
};

struct config {
	char		*root;		/* "/" or test/image root */
	char		*confdir;	/* <root>/etc/upkg */
	char		*keydir;	/* <confdir>/keys */
	char		*dbdir;		/* <root>/var/lib/upkg */
	char		*cachedir;	/* <root>/var/cache/upkg */
	char		*arch;
	char		*portsdir;
	char		*builddir;
	char		*build_user;
	char		*ca_file;
	int		 allow_http;
	struct strlist	 prefixes;
	struct repo	*repos;
	size_t		 nrepos;
};

int	 config_load(struct config *, const char *root);
void	 config_free(struct config *);
int	 config_save_repos(const struct config *);
struct repo *config_repo(struct config *, const char *);
int	 ini_parse(const char *text, const char *fname,
	    int (*cb)(void *, const char *section, const char *key,
	    const char *value, int line), void *arg);
int	 parse_bool(const char *, int *);

/* ---------------------------------------------------------------- fetch.c */

int	 fetch_url(const struct config *, const char *url, const char *dest);

/* ---------------------------------------------------------------- repo.c */

int	 repo_sync(struct config *, struct repo *);
int	 repo_load(struct config *, struct repo *);
int	 repo_load_all(struct config *);
int	 index_parse(const char *text, struct repo *);
int	 repo_build_index(const char *dir, const char *reponame,
	    const char *keyfile);
int	 repo_fetch_pkg(struct config *, struct pkgmeta *, char **path);

/* ---------------------------------------------------------------- pkgfile.c */

struct pkgfile {
	struct pkgmeta	 meta;
	struct manifest	 manifest;
	char		*path;
};

int	 pkgfile_read_meta(const char *path, struct pkgfile *);
void	 pkgfile_free(struct pkgfile *);
int	 pkgfile_create(const char *pkginfo_text, const char *stagedir,
	    const char *outpath);

struct txn;
int	 pkgfile_extract(const char *path, struct pkgfile *, struct txn *);

/* ---------------------------------------------------------------- db.c */

struct db;

struct instpkg {
	char		*name;
	char		*version;
	unsigned	 release;
	char		*arch;
	char		*repo;
	char		*reason;
	char		*description;
	long long	 installed;
	struct strlist	 depends;
	struct strlist	 conflicts;
};

int	 db_open(struct config *, struct db **, int create);
void	 db_close(struct db *);
int	 db_begin(struct db *);
int	 db_commit(struct db *);
int	 db_rollback(struct db *);
int	 db_get_pkg(struct db *, const char *name, struct instpkg *);
void	 instpkg_free(struct instpkg *);
int	 db_list_pkgs(struct db *, struct instpkg **, size_t *);
int	 db_file_owner(struct db *, const char *path, char **owner);
int	 db_pkg_files(struct db *, const char *pkg, struct manifest *);
int	 db_pkg_dirs(struct db *, const char *pkg, struct strlist *);
int	 db_dir_owners(struct db *, const char *path, struct strlist *);
int	 db_add_pkg(struct db *, const struct pkgmeta *, const struct manifest *,
	    const char *repo, const char *reason);
int	 db_del_pkg(struct db *, const char *name);
int	 db_set_reason(struct db *, const char *name, const char *reason);
int	 db_txn_mark(struct db *, const char *txid);
int	 db_txn_exists(struct db *, const char *txid);
int	 db_txn_clear(struct db *, const char *txid);

/* ---------------------------------------------------------------- txn.c */

int	 lock_acquire(struct config *);
void	 lock_release(void);
int	 txn_recover(struct config *, struct db *);

struct txn *txn_begin(struct config *, struct db *);
int	 txn_stage_dir(struct txn *, const char *relpath, mode_t);
int	 txn_stage_file_open(struct txn *, const char *relpath, mode_t, int *fd);
int	 txn_stage_link(struct txn *, const char *relpath, const char *target);
int	 txn_unlink(struct txn *, const char *relpath);
int	 txn_rmdir(struct txn *, const char *relpath);
int	 txn_commit(struct txn *);
void	 txn_abort(struct txn *);
void	 crashpoint(const char *);

/* ---------------------------------------------------------------- ops.c */

struct opts {
	int		 cascade;
	int		 allow_unsigned;
	int		 dry_run;
	int		 as_deps;
	int		 nodeps;
	char		*output;
	char		*keyfile;
};

int	 op_install(struct config *, char **targets, int n, struct opts *);
int	 op_remove(struct config *, char **targets, int n, struct opts *);
int	 op_autoremove(struct config *, struct opts *);
int	 op_upgrade(struct config *, struct opts *);

/* ---------------------------------------------------------------- build.c */

int	 op_build(struct config *, const char *target, struct opts *);

#endif /* UPKG_H */
