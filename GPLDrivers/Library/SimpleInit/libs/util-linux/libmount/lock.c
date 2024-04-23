/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * This file is part of libmount from util-linux project.
 *
 * Copyright (C) 2009-2018 Karel Zak <kzak@redhat.com>
 *
 * libmount is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

/**
 * SECTION: lock
 * @title: Locking
 * @short_description: locking methods for /etc/mtab or another libmount files
 *
 * The mtab lock is backwards compatible with the standard linux /etc/mtab
 * locking.  Note, it's necessary to use the same locking schema in all
 * applications that access the file.
 */

#define _GNU_SOURCE
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include "strutils.h"
#include "mountP.h"
#include "monotonic.h"

/*
 * lock handler
 */
struct libmnt_lock {
	char	*lockfile;	/* path to lock file (e.g. /etc/mtab~) */
	char	*linkfile;	/* path to link file (e.g. /etc/mtab~.<id>) */
	int	lockfile_fd;	/* lock file descriptor */

	unsigned int	locked :1,	/* do we own the lock? */
			sigblock :1,	/* block signals when locked */
			simplelock :1;	/* use flock rather than normal mtab lock */

	sigset_t oldsigmask;
};


/**
 * mnt_new_lock:
 * @datafile: the file that should be covered by the lock
 * @id: unique linkfile identifier or 0 (default is getpid())
 *
 * Returns: newly allocated lock handler or NULL on case of error.
 */
struct libmnt_lock *mnt_new_lock(const char *datafile, pid_t id)
{
	struct libmnt_lock *ml = NULL;
	char *lo = NULL, *ln = NULL;
	size_t losz;

	if (!datafile)
		return NULL;

	/* for flock we use "foo.lock, for mtab "foo~"
	 */
	losz = strlen(datafile) + sizeof(".lock");
	lo = malloc(losz);
	if (!lo)
		goto err;

	/* default is mtab~ lock */
	snprintf(lo, losz, "%s~", datafile);

	if (asprintf(&ln, "%s~.%d", datafile, id ? : getpid()) == -1) {
		ln = NULL;
		goto err;
	}
	ml = calloc(1, sizeof(*ml) );
	if (!ml)
		goto err;

	ml->lockfile_fd = -1;
	ml->linkfile = ln;
	ml->lockfile = lo;

	return ml;
err:
	free(lo);
	free(ln);
	free(ml);
	return NULL;
}


/**
 * mnt_free_lock:
 * @ml: struct libmnt_lock handler
 *
 * Deallocates mnt_lock.
 */
void mnt_free_lock(struct libmnt_lock *ml)
{
	if (!ml)
		return;
	free(ml->lockfile);
	free(ml->linkfile);
	free(ml);
}

/**
 * mnt_lock_block_signals:
 * @ml: struct libmnt_lock handler
 * @enable: TRUE/FALSE
 *
 * Block/unblock signals when the lock is locked, the signals are not blocked
 * by default.
 *
 * Returns: <0 on error, 0 on success.
 */
int mnt_lock_block_signals(struct libmnt_lock *ml, int enable)
{
	if (!ml)
		return -EINVAL;
	ml->sigblock = enable ? 1 : 0;
	return 0;
}

/* don't export this to API
 */
int mnt_lock_use_simplelock(struct libmnt_lock *ml, int enable)
{
	size_t sz;

	if (!ml)
		return -EINVAL;


	ml->simplelock = enable ? 1 : 0;

	sz = strlen(ml->lockfile);

	/* Change lock name:
	 *
	 *	flock:     "<name>.lock"
	 *	mtab lock: "<name>~"
	 */
	if (ml->simplelock && endswith(ml->lockfile, "~"))
		memcpy(ml->lockfile + sz - 1, ".lock", 6);

	else if (!ml->simplelock && endswith(ml->lockfile, ".lock"))
		 memcpy(ml->lockfile + sz - 5, "~", 2);

	return 0;
}

/*
 * Returns path to lockfile.
 */
static const char *mnt_lock_get_lockfile(struct libmnt_lock *ml)
{
	return ml ? ml->lockfile : NULL;
}

/*
 * Note that the filename is generated by mnt_new_lock() and depends on
 * getpid() or 'id' argument of the mnt_new_lock() function.
 *
 * Returns: unique (per process/thread) path to linkfile.
 */
static const char *mnt_lock_get_linkfile(struct libmnt_lock *ml)
{
	return ml ? ml->linkfile : NULL;
}

/*
 * Simple flocking
 */
static void unlock_simplelock(struct libmnt_lock *ml)
{

	if (ml->lockfile_fd >= 0) {
		close(ml->lockfile_fd);
	}
}

static int lock_simplelock(struct libmnt_lock *ml)
{
	const char *lfile;
	int rc;
	struct stat sb;
	const mode_t lock_mask = S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;


	lfile = mnt_lock_get_lockfile(ml);


	if (ml->sigblock) {
		sigset_t sigs;
		sigemptyset(&ml->oldsigmask);
		sigfillset(&sigs);
		sigprocmask(SIG_BLOCK, &sigs, &ml->oldsigmask);
	}

	ml->lockfile_fd = open(lfile, O_RDONLY|O_CREAT|O_CLOEXEC,
				      S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);
	if (ml->lockfile_fd < 0) {
		rc = -errno;
		goto err;
	}

	rc = fstat(ml->lockfile_fd, &sb);
	if (rc < 0) {
		rc = -errno;
		goto err;
	}

	if ((sb.st_mode & lock_mask) != lock_mask) {
		rc = fchmod(ml->lockfile_fd, lock_mask);
		if (rc < 0) {
			rc = -errno;
			goto err;
		}
	}

	while (flock(ml->lockfile_fd, LOCK_EX) < 0) {
		int errsv;
		if ((errno == EAGAIN) || (errno == EINTR))
			continue;
		errsv = errno;
		close(ml->lockfile_fd);
		ml->lockfile_fd = -1;
		rc = -errsv;
		goto err;
	}
	ml->locked = 1;
	return 0;
err:
	if (ml->sigblock)
		sigprocmask(SIG_SETMASK, &ml->oldsigmask, NULL);
	return rc;
}

/*
 * traditional mtab locking
 */

static void mnt_lockalrm_handler(int sig __attribute__((__unused__)))
{
	/* do nothing, say nothing, be nothing */
}

/*
 * Waits for F_SETLKW, unfortunately we have to use SIGALRM here to interrupt
 * fcntl() to avoid neverending waiting.
 *
 * Returns: 0 on success, 1 on timeout, -errno on error.
 */
static int mnt_wait_mtab_lock(struct libmnt_lock *ml, struct flock *fl, time_t maxtime)
{
	struct timeval now = { 0 };
	struct sigaction sa, osa;
	int ret = 0;

	gettime_monotonic(&now);

	if (now.tv_sec >= maxtime)
		return 1;		/* timeout */

	/* setup ALARM handler -- we don't want to wait forever */
	sa.sa_flags = 0;
	sa.sa_handler = mnt_lockalrm_handler;
	sigfillset (&sa.sa_mask);

	sigaction(SIGALRM, &sa, &osa);


	alarm(maxtime - now.tv_sec);
	if (fcntl(ml->lockfile_fd, F_SETLKW, fl) == -1)
		ret = errno == EINTR ? 1 : -errno;
	alarm(0);

	/* restore old sigaction */
	sigaction(SIGALRM, &osa, NULL);

	return ret;
}

/*
 * Create the mtab lock file.
 *
 * The old code here used flock on a lock file /etc/mtab~ and deleted
 * this lock file afterwards. However, as rgooch remarks, that has a
 * race: a second mount may be waiting on the lock and proceed as
 * soon as the lock file is deleted by the first mount, and immediately
 * afterwards a third mount comes, creates a new /etc/mtab~, applies
 * flock to that, and also proceeds, so that the second and third mount
 * are now both scribbling in /etc/mtab.
 *
 * The new code uses a link() instead of a creat(), where we proceed
 * only if it was us that created the lock, and hence we always have
 * to delete the lock afterwards. Now the use of flock() is in principle
 * superfluous, but avoids an arbitrary sleep().
 *
 * Where does the link point to? Obvious choices are mtab and mtab~~.
 * HJLu points out that the latter leads to races. Right now we use
 * mtab~.pid instead.
 *
 *
 * The original mount locking code has used sleep(1) between attempts and
 * maximal number of attempts has been 5.
 *
 * There was a very small number of attempts and extremely long waiting (1s)
 * that is useless on machines with large number of mount processes.
 *
 * Now we wait for a few thousand microseconds between attempts and we have a global
 * time limit (30s) rather than a limit for the number of attempts. The advantage
 * is that this method also counts time which we spend in fcntl(F_SETLKW) and
 * the number of attempts is not restricted.
 * -- kzak@redhat.com [Mar-2007]
 *
 *
 * This mtab locking code has been refactored and moved to libmount. The mtab
 * locking is really not perfect (e.g. SIGALRM), but it's stable, reliable and
 * backwardly compatible code.
 *
 * Don't forget that this code has to be compatible with 3rd party mounts
 * (/sbin/mount.foo) and has to work with NFS.
 * -- kzak@redhat.com [May-2009]
 */

/* maximum seconds between the first and the last attempt */
#define MOUNTLOCK_MAXTIME		30

/* sleep time (in microseconds, max=999999) between attempts */
#define MOUNTLOCK_WAITTIME		5000

static void unlock_mtab(struct libmnt_lock *ml)
{
	if (!ml)
		return;

	if (!ml->locked && ml->lockfile && ml->linkfile)
	{
		/* We (probably) have all the files, but we don't own the lock,
		 * Really? Check it! Maybe ml->locked wasn't set properly
		 * because the code was interrupted by a signal. Paranoia? Yes.
		 *
		 * We own the lock when linkfile == lockfile.
		 */
		struct stat lo, li;

		if (!stat(ml->lockfile, &lo) && !stat(ml->linkfile, &li) &&
		    lo.st_dev == li.st_dev && lo.st_ino == li.st_ino)
			ml->locked = 1;
	}

	if (ml->linkfile)
		unlink(ml->linkfile);
	if (ml->lockfile_fd >= 0)
		close(ml->lockfile_fd);
	if (ml->locked && ml->lockfile) {
		unlink(ml->lockfile);
	}
}

static int lock_mtab(struct libmnt_lock *ml)
{
	int i, rc = -1;
	struct timespec waittime = { 0 };;
	struct timeval maxtime = { 0 };
	const char *lockfile, *linkfile;

	if (!ml)
		return -EINVAL;
	if (ml->locked)
		return 0;

	lockfile = mnt_lock_get_lockfile(ml);
	if (!lockfile)
		return -EINVAL;
	linkfile = mnt_lock_get_linkfile(ml);
	if (!linkfile)
		return -EINVAL;

	if (ml->sigblock) {
		/*
		 * Block all signals when locked, mnt_unlock_file() will
		 * restore the old mask.
		 */
		sigset_t sigs;

		sigemptyset(&ml->oldsigmask);
		sigfillset(&sigs);
		sigdelset(&sigs, SIGTRAP);
		sigdelset(&sigs, SIGALRM);
		sigprocmask(SIG_BLOCK, &sigs, &ml->oldsigmask);
	}

	i = open(linkfile, O_WRONLY|O_CREAT|O_CLOEXEC, S_IRUSR|S_IWUSR);
	if (i < 0) {
		/* linkfile does not exist (as a file) and we cannot create it.
		 * Read-only or full filesystem? Too many files open in the system?
		 */
		if (errno > 0)
			rc = -errno;
		goto failed;
	}
	close(i);

	gettime_monotonic(&maxtime);
	maxtime.tv_sec += MOUNTLOCK_MAXTIME;

	waittime.tv_sec = 0;
	waittime.tv_nsec = (1000 * MOUNTLOCK_WAITTIME);

	/* Repeat until it was us who made the link */
	while (!ml->locked) {
		struct timeval now = { 0 };
		struct flock flock;
		int j;

		j = link(linkfile, lockfile);
		if (j == 0)
			ml->locked = 1;

		if (j < 0 && errno != EEXIST) {
			if (errno > 0)
				rc = -errno;
			goto failed;
		}
		ml->lockfile_fd = open(lockfile, O_WRONLY|O_CLOEXEC);

		if (ml->lockfile_fd < 0) {
			/* Strange... Maybe the file was just deleted? */
			int errsv = errno;
			gettime_monotonic(&now);
			if (errsv == ENOENT && now.tv_sec < maxtime.tv_sec) {
				ml->locked = 0;
				continue;
			}
			if (errsv > 0)
				rc = -errsv;
			goto failed;
		}

		flock.l_type = F_WRLCK;
		flock.l_whence = SEEK_SET;
		flock.l_start = 0;
		flock.l_len = 0;

		if (ml->locked) {
			/* We made the link. Now claim the lock. */
			fcntl (ml->lockfile_fd, F_SETLK, &flock);
			break;
		}

		/* Someone else made the link. Wait. */
		int err = mnt_wait_mtab_lock(ml, &flock, maxtime.tv_sec);

		if (err == 1) {
			rc = -ETIMEDOUT;
			goto failed;

		} else if (err < 0) {
			rc = err;
			goto failed;
		}
		nanosleep(&waittime, NULL);
		close(ml->lockfile_fd);
		ml->lockfile_fd = -1;
	}
	unlink(linkfile);
	return 0;

failed:
	mnt_unlock_file(ml);
	return rc;
}


/**
 * mnt_lock_file
 * @ml: pointer to struct libmnt_lock instance
 *
 * Creates a lock file (e.g. /etc/mtab~). Note that this function may
 * use alarm().
 *
 * Your application always has to call mnt_unlock_file() before exit.
 *
 * Traditional mtab locking scheme:
 *
 *   1. create linkfile (e.g. /etc/mtab~.$PID)
 *   2. link linkfile --> lockfile (e.g. /etc/mtab~.$PID --> /etc/mtab~)
 *   3. a) link() success: setups F_SETLK lock (see fcntl(2))
 *      b) link() failed:  wait (max 30s) on F_SETLKW lock, goto 2.
 *
 * Note that when the lock is used by mnt_update_table() interface then libmount
 * uses flock() for private library file /run/mount/utab. The fcntl(2) is used only
 * for backwardly compatible stuff like /etc/mtab.
 *
 * Returns: 0 on success or negative number in case of error (-ETIMEOUT is case
 * of stale lock file).
 */
int mnt_lock_file(struct libmnt_lock *ml)
{
	if (!ml)
		return -EINVAL;

	if (ml->simplelock)
		return lock_simplelock(ml);

	return lock_mtab(ml);
}

/**
 * mnt_unlock_file:
 * @ml: lock struct
 *
 * Unlocks the file. The function could be called independently of the
 * lock status (for example from exit(3)).
 */
void mnt_unlock_file(struct libmnt_lock *ml)
{
	if (!ml)
		return;

	if (ml->simplelock)
		unlock_simplelock(ml);
	else
		unlock_mtab(ml);

	ml->locked = 0;
	ml->lockfile_fd = -1;

	if (ml->sigblock) {
		sigprocmask(SIG_SETMASK, &ml->oldsigmask, NULL);
	}
}