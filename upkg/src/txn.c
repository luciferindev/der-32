/*
 * upkg -- locking, filesystem transactions and crash recovery.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Every operation that changes the system runs as one transaction:
 *
 *  1. Stage.  New objects are written next to their final location as
 *     "<path>.upkg-new"; missing directories are created.  Each action is
 *     appended to /var/lib/upkg/journal *before* it is performed.
 *  2. Commit.  The journal is fsync'd, then the database changes and a
 *     marker row txn(id) are committed in a single SQLite transaction.
 *     This commit is the atomic decision point.
 *  3. Apply.  Staged objects are rename(2)d into place (atomic per file),
 *     obsolete files are unlinked and empty directories removed.
 *  4. Finish.  The marker row and the journal are deleted.
 *
 * Recovery (run automatically before every modifying command): if a journal
 * exists and its marker row is in the database, the transaction committed
 * and phase 3 is replayed (roll forward); otherwise phase 1 is undone (roll
 * back).  Both directions are idempotent, so recovery itself may be
 * interrupted.  SIGINT/SIGTERM/SIGHUP abort cleanly during phase 1 and are
 * deferred during phases 2-4.
 */

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "upkg.h"

struct txn {
	struct config	*cfg;
	struct db	*db;
	char		*id;
	char		*jpath;
	int		 jfd;
	struct strlist	 ops;	/* "op path", in journal order */
};

static int lockfd = -1;

void
crashpoint(const char *name)
{
	const char *c = getenv("UPKG_TEST_CRASH");

	if (c && strcmp(c, name) == 0) {
		fprintf(stderr, "upkg: test crash at '%s'\n", name);
		fflush(stderr);
		kill(getpid(), SIGKILL);
	}
}

int
lock_acquire(struct config *cfg)
{
	char *path;
	struct flock fl;

	if (lockfd != -1)
		return 0;
	if (mkdir_p(cfg->dbdir, 0755) == -1)
		return -1;
	path = path_join(cfg->dbdir, "lock");
	lockfd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
	if (lockfd == -1) {
		upkg_err("cannot open %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (fcntl(lockfd, F_SETLK, &fl) == -1) {
		upkg_err("the package database is locked by another upkg "
		    "process (%s)", path);
		close(lockfd);
		lockfd = -1;
		free(path);
		return -1;
	}
	free(path);
	{
		const char *h = getenv("UPKG_TEST_HOLD_LOCK");
		if (h)
			sleep((unsigned)atoi(h));
	}
	return 0;
}

void
lock_release(void)
{
	if (lockfd != -1) {
		close(lockfd);
		lockfd = -1;
	}
}

static char *
abspath(struct txn *t, const char *rel)
{
	return path_join(t->cfg->root, rel);
}

static int
journal(struct txn *t, const char *op, const char *rel)
{
	char *line = xasprintf("%s %s\n", op, rel);
	size_t n = strlen(line);
	ssize_t w;

	w = write(t->jfd, line, n);
	free(line);
	if (w != (ssize_t)n) {
		upkg_err("cannot write journal: %s", strerror(errno));
		return -1;
	}
	sl_addn(&t->ops, xasprintf("%s %s", op, rel));
	return 0;
}

static void
own(int fd, const char *path)
{
	if (geteuid() != 0)
		return;
	if (fd >= 0) {
		if (fchown(fd, 0, 0) == -1)
			upkg_warn("chown %s: %s", path, strerror(errno));
	} else if (lchown(path, 0, 0) == -1)
		upkg_warn("chown %s: %s", path, strerror(errno));
}

/*
 * Make sure every ancestor of `rel' is a real directory below the root,
 * creating missing ones.  Symbolic links are never traversed, so a package
 * cannot write outside the root through a planted link.
 */
static int
ensure_parents(struct txn *t, const char *rel)
{
	char *p = xstrdup(rel), *s;
	struct stat st;
	int rv = 0;

	for (s = p; (s = strchr(s, '/')) != NULL; s++) {
		*s = '\0';
		char *a = abspath(t, p);

		if (lstat(a, &st) == -1) {
			if (errno != ENOENT) {
				upkg_err("%s: %s", a, strerror(errno));
				rv = -1;
			} else if (journal(t, "mkdir", p) == -1) {
				rv = -1;
			} else if (mkdir(a, 0755) == -1) {
				upkg_err("mkdir %s: %s", a, strerror(errno));
				rv = -1;
			} else
				own(-1, a);
		} else if (S_ISLNK(st.st_mode)) {
			upkg_err("refusing to traverse symbolic link %s", a);
			rv = -1;
		} else if (!S_ISDIR(st.st_mode)) {
			upkg_err("%s: not a directory", a);
			rv = -1;
		}
		free(a);
		*s = '/';
		if (rv)
			break;
	}
	free(p);
	return rv;
}

static char *
gen_id(void)
{
	unsigned char r[8] = { 0 };
	char hex[17];
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd != -1) {
		if (read(fd, r, sizeof(r)) != (ssize_t)sizeof(r))
			memset(r, 0, sizeof(r));
		close(fd);
	}
	hexencode(r, sizeof(r), hex);
	return xasprintf("%lld-%ld-%s", (long long)time(NULL), (long)getpid(),
	    hex);
}

struct txn *
txn_begin(struct config *cfg, struct db *db)
{
	struct txn *t = xcalloc(1, sizeof(*t));
	char *hdr;

	t->cfg = cfg;
	t->db = db;
	t->id = gen_id();
	t->jpath = path_join(cfg->dbdir, "journal");
	sl_init(&t->ops);
	t->jfd = open(t->jpath, O_WRONLY | O_CREAT | O_EXCL | O_APPEND |
	    O_NOFOLLOW, 0644);
	if (t->jfd == -1) {
		upkg_err("cannot create journal %s: %s", t->jpath,
		    strerror(errno));
		free(t->id);
		free(t->jpath);
		free(t);
		return NULL;
	}
	hdr = xasprintf("upkg-journal 1 %s\n", t->id);
	if (write(t->jfd, hdr, strlen(hdr)) != (ssize_t)strlen(hdr) ||
	    fsync(t->jfd) == -1) {
		upkg_err("cannot write journal: %s", strerror(errno));
		free(hdr);
		close(t->jfd);
		unlink(t->jpath);
		free(t->id);
		free(t->jpath);
		free(t);
		return NULL;
	}
	free(hdr);
	return t;
}

int
txn_stage_dir(struct txn *t, const char *rel, mode_t mode)
{
	char *a = abspath(t, rel);
	struct stat st;
	int rv = -1;

	if (ensure_parents(t, rel) == -1)
		goto out;
	if (lstat(a, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			rv = 0;
			goto out;
		}
		upkg_err("%s exists and is not a directory", a);
		goto out;
	}
	if (journal(t, "mkdir", rel) == -1)
		goto out;
	if (mkdir(a, 0700) == -1) {
		upkg_err("mkdir %s: %s", a, strerror(errno));
		goto out;
	}
	own(-1, a);
	if (chmod(a, mode & 01777) == -1) {
		upkg_err("chmod %s: %s", a, strerror(errno));
		goto out;
	}
	rv = 0;
out:
	free(a);
	return rv;
}

int
txn_stage_file_open(struct txn *t, const char *rel, mode_t mode, int *fdp)
{
	char *a = abspath(t, rel);
	char *staged = xasprintf("%s%s", a, UPKG_STAGE_SUFFIX);
	int fd, rv = -1;

	if (ensure_parents(t, rel) == -1 || journal(t, "stage", rel) == -1)
		goto out;
	fd = open(staged, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd == -1) {
		upkg_err("cannot create %s: %s", staged, strerror(errno));
		goto out;
	}
	own(fd, staged);
	if (fchmod(fd, mode & 0777) == -1) {
		upkg_err("chmod %s: %s", staged, strerror(errno));
		close(fd);
		goto out;
	}
	*fdp = fd;
	rv = 0;
out:
	free(a);
	free(staged);
	return rv;
}

int
txn_stage_link(struct txn *t, const char *rel, const char *target)
{
	char *a = abspath(t, rel);
	char *staged = xasprintf("%s%s", a, UPKG_STAGE_SUFFIX);
	int rv = -1;

	if (ensure_parents(t, rel) == -1 || journal(t, "stage", rel) == -1)
		goto out;
	if (symlink(target, staged) == -1) {
		upkg_err("symlink %s: %s", staged, strerror(errno));
		goto out;
	}
	own(-1, staged);
	rv = 0;
out:
	free(a);
	free(staged);
	return rv;
}

int
txn_unlink(struct txn *t, const char *rel)
{
	return journal(t, "unlink", rel);
}

int
txn_rmdir(struct txn *t, const char *rel)
{
	return journal(t, "rmdir", rel);
}

/* Apply (forward) or undo (backward) the operations of a journal. */
static void
replay(struct config *cfg, struct strlist *ops, int forward)
{
	size_t i;

	if (forward) {
		for (i = 0; i < ops->n; i++) {
			char *op = ops->v[i], *rel = strchr(op, ' ');
			char *a, *s;

			if (rel == NULL)
				continue;
			rel++;
			a = path_join(cfg->root, rel);
			if (strncmp(op, "stage ", 6) == 0) {
				s = xasprintf("%s%s", a, UPKG_STAGE_SUFFIX);
				if (rename(s, a) == -1 && errno != ENOENT)
					upkg_err("rename %s: %s", a,
					    strerror(errno));
				free(s);
			} else if (strncmp(op, "unlink ", 7) == 0) {
				if (unlink(a) == -1 && errno != ENOENT)
					upkg_warn("unlink %s: %s", a,
					    strerror(errno));
			} else if (strncmp(op, "rmdir ", 6) == 0) {
				if (rmdir(a) == -1 && errno != ENOENT &&
				    errno != ENOTEMPTY && errno != EEXIST)
					upkg_warn("rmdir %s: %s", a,
					    strerror(errno));
			}
			free(a);
		}
		return;
	}
	for (i = ops->n; i-- > 0; ) {
		char *op = ops->v[i], *rel = strchr(op, ' ');
		char *a, *s;

		if (rel == NULL)
			continue;
		rel++;
		a = path_join(cfg->root, rel);
		if (strncmp(op, "stage ", 6) == 0) {
			s = xasprintf("%s%s", a, UPKG_STAGE_SUFFIX);
			if (unlink(s) == -1 && errno != ENOENT)
				upkg_warn("unlink %s: %s", s, strerror(errno));
			free(s);
		} else if (strncmp(op, "mkdir ", 6) == 0) {
			(void)rmdir(a);
		}
		free(a);
	}
}

static void
block_signals(sigset_t *old)
{
	sigset_t s;

	sigemptyset(&s);
	sigaddset(&s, SIGINT);
	sigaddset(&s, SIGTERM);
	sigaddset(&s, SIGHUP);
	sigaddset(&s, SIGQUIT);
	sigprocmask(SIG_BLOCK, &s, old);
}

static void
txn_free(struct txn *t)
{
	if (t->jfd != -1)
		close(t->jfd);
	sl_free(&t->ops);
	free(t->id);
	free(t->jpath);
	free(t);
}

/*
 * Commit: the caller has made its database changes inside db_begin().
 */
int
txn_commit(struct txn *t)
{
	sigset_t old;

	if (fsync(t->jfd) == -1) {
		upkg_err("cannot sync journal: %s", strerror(errno));
		txn_abort(t);
		return -1;
	}
	crashpoint("before-commit");
	block_signals(&old);
	if (db_txn_mark(t->db, t->id) == -1 || db_commit(t->db) == -1) {
		sigprocmask(SIG_SETMASK, &old, NULL);
		txn_abort(t);
		return -1;
	}
	crashpoint("after-commit");
	replay(t->cfg, &t->ops, 1);
	crashpoint("after-apply");
	db_txn_clear(t->db, t->id);
	close(t->jfd);
	t->jfd = -1;
	unlink(t->jpath);
	sigprocmask(SIG_SETMASK, &old, NULL);
	txn_free(t);
	return 0;
}

void
txn_abort(struct txn *t)
{
	db_rollback(t->db);
	replay(t->cfg, &t->ops, 0);
	close(t->jfd);
	t->jfd = -1;
	unlink(t->jpath);
	txn_free(t);
}

int
txn_recover(struct config *cfg, struct db *db)
{
	char *jpath = path_join(cfg->dbdir, "journal"), *text, *line, *next;
	char id[128];
	struct strlist ops;
	int committed;

	if (read_file(jpath, &text, NULL) == -1) {
		free(jpath);
		return errno == ENOENT ? 0 : -1;
	}
	sl_init(&ops);
	if (sscanf(text, "upkg-journal 1 %127s", id) != 1) {
		upkg_err("%s: unrecognised journal; remove it manually after "
		    "inspection", jpath);
		free(text);
		free(jpath);
		return -1;
	}
	line = strchr(text, '\n');
	for (line = line ? line + 1 : NULL; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next == NULL)
			break;	/* torn final line: never acted upon */
		*next++ = '\0';
		if (*line)
			sl_add(&ops, line);
	}
	committed = db_txn_exists(db, id);
	if (committed == -1) {
		sl_free(&ops);
		free(text);
		free(jpath);
		return -1;
	}
	upkg_warn("recovering interrupted transaction %s: %s", id,
	    committed ? "completing it (roll forward)" :
	    "undoing it (roll back)");
	replay(cfg, &ops, committed);
	if (committed)
		db_txn_clear(db, id);
	unlink(jpath);
	sl_free(&ops);
	free(text);
	free(jpath);
	return 0;
}
