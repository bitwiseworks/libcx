/* test whether fcntl locking works on this system */

/* Borrowed from Samba4 configure tests and aligned to build with LIBCx. */

#define HAVE_UNISTD_H
#define HAVE_FCNTL_H
#define HAVE_SYS_FCNTL_H
#define HAVE_SYS_WAIT_H
#define HAVE_WAITPID
#define LIBCX_BUILD

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include <errno.h>

static int sys_waitpid(pid_t pid,int *status,int options)
{
#ifdef HAVE_WAITPID
  return waitpid(pid,status,options);
#else /* USE_WAITPID */
  return wait4(pid, status, options, NULL);
#endif /* USE_WAITPID */
}

#ifdef LIBCX_BUILD
#define DATA fname
#else
#define DATA "conftest.fcntl"
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

/* lock a byte range in a open file */
static int do_test(void)
{
	struct flock lock;
	int fd, ret, status=1;
	pid_t pid;
#ifdef LIBCX_BUILD
	char *fname;

	fd = create_temp_file("tst-flock3-", &fname);
	if (fd == -1)
    return -1;
	close(fd);
#else
	char *testdir = NULL;

	testdir = getenv("TESTDIR");
	if (testdir) chdir(testdir);
#endif

	alarm(10);

	if (!(pid=fork())) {
		sleep(2);
		fd = open(DATA, O_RDONLY);

		if (fd == -1) {
			fprintf(stderr,"ERROR: failed to open %s (errno=%d)\n", 
				DATA, (int)errno);
			exit(1);
		}

		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = 0x100000000LL;
		lock.l_len = 4;
		lock.l_pid = getpid();
		
		lock.l_type = F_WRLCK;
		
		/* check if a lock applies */
		ret = fcntl(fd,F_GETLK,&lock);

		if ((ret == -1) ||
		    (lock.l_type == F_UNLCK)) {
			fprintf(stderr,"ERROR: lock test failed (ret=%d errno=%d)\n", ret, (int)errno);
			exit(1);
		} else {
			exit(0);
		}
	}

	unlink(DATA);
	fd = open(DATA, O_RDWR|O_CREAT|O_EXCL, 0600);

	if (fd == -1) {
		fprintf(stderr,"ERROR: failed to open %s (errno=%d)\n", 
			DATA, (int)errno);
		exit(1);
	}

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0x100000004LL;
	lock.l_pid = getpid();

	/* set a 100000004 byte write lock, should conflict with the above */
	ret = fcntl(fd,F_SETLK,&lock);

	sys_waitpid(pid, &status, 0);

#ifndef LIBCX_BUILD
	unlink(DATA);
#endif

	if (ret != 0) {
		fprintf(stderr,"ERROR: failed to lock %s (errno=%d)\n", 
			DATA, (int)errno);
		exit(1);
	}

	if (lock.l_len < 0x100000004LL) {
		fprintf(stderr,"ERROR: settign lock overflowed\n");
		exit(1);
	}

#if defined(WIFEXITED) && defined(WEXITSTATUS)
    if(WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    } else {
        status = 1;
    }
#else /* defined(WIFEXITED) && defined(WEXITSTATUS) */
	status = (status == 0) ? 0 : 1;
#endif /* defined(WIFEXITED) && defined(WEXITSTATUS) */

	if (status) {
		fprintf(stderr,"ERROR: lock test failed with status=%d\n", 
			status);
	}

	exit(status);
}
