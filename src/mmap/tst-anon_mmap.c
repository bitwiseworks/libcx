/*
 * Testcase for anonymous shared mmap.
 * Copyright (C) 2016 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2016.
 *
 * The kLIBC Extension Library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The kLIBC Extension Library is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>.
 */
/*************************************************************************\
*                  Copyright (C) Michael Kerrisk, 2014.                   *
*                                                                         *
* This program is free software. You may use, modify, and redistribute it *
* under the terms of the GNU Affero General Public License as published   *
* by the Free Software Foundation, either version 3 or (at your option)   *
* any later version. This program is distributed without any warranty.    *
* See the file COPYING.agpl-v3 for details.                               *
\*************************************************************************/

/* anon_mmap.c

   Demonstrate how to share a region of mapped memory between a parent and
   child process without having to create a mapped file, either through the
   creation of an anonymous memory mapping or through the mapping of /dev/zero.
*/
#if defined(__KLIBC__) || defined(__APPLE__)
#define USE_MAP_ANON
#endif
#ifdef USE_MAP_ANON
#define _BSD_SOURCE             /* Get MAP_ANONYMOUS definition */
#endif
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>  /* Type definitions used by many programs */
#include <stdio.h>      /* Standard I/O functions */
#include <stdlib.h>     /* Prototypes of commonly used library functions,
                           plus EXIT_SUCCESS and EXIT_FAILURE constants */
#include <unistd.h>     /* Prototypes for many system calls */
#include <errno.h>      /* Declares errno and defines error constants */
#include <string.h>     /* Commonly used string-handling functions */

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
    int *addr;                  /* Pointer to shared memory region */
    int status;
    /* Parent creates mapped region prior to calling fork() */

#ifdef USE_MAP_ANON             /* Use MAP_ANONYMOUS */
    addr = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED){
        fprintf(stderr,"mmap failed - errno = %d\n",errno);
        exit(-1);
    }

#else                           /* Map /dev/zero */
    int fd;

    fd = open("/dev/zero", O_RDWR);
    if (fd == -1)
        errExit("open");

    addr = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
        errExit("mmap");

    if (close(fd) == -1)        /* No longer needed */
        errExit("close");
#endif

    /*
     * Part 1. Test successful parent-chlid-grandchild access.
     */

    *addr = 1;                  /* Initialize integer in mapped region */

    switch (fork()) {           /* Parent and child share mapping */
    case -1:
        fprintf(stderr,"fork failed - errno = %d\n",errno);
	exit(-1);
    case 0:                     /* Child: increment shared integer and exit */
        printf("Child started, ");
        printf("value = %d\n", *addr);
        (*addr)++;
        if (*addr != 2) {
            fprintf(stderr,"value is %d, must be 2\n",*addr);
            exit(-1);
        }
        switch (fork()) {       /* Parent and child share mapping */
        case -1:
            fprintf(stderr,"fork failed - errno = %d\n",errno);
            exit(-1);
        case 0:                 /* Child: increment shared integer and exit */
            printf("Grand child started, ");
            printf("value = %d\n", *addr);
            (*addr)++;
            if (*addr != 3) {
                fprintf(stderr,"value is %d, must be 3\n",*addr);
                exit(-1);
            }
            if (munmap(addr, sizeof(int)) == -1){
                fprintf(stderr,"munmap failed - errno = %d\n",errno);
                exit(-1);
            }
            exit(EXIT_SUCCESS);
        default:                /* Parent: wait for child to terminate */
            if (wait(&status) == -1){
                fprintf(stderr,"wait failed - errno = %d\n",errno);
                exit(-1);
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                fprintf(stderr,"child crashed or returned non-zero (status %x)\n", status);
                exit(-1);
            }
            printf("In child, value = %d\n", *addr);
            if (*addr != 3) {
                fprintf(stderr,"value is %d, must be 3\n",*addr);
                exit(-1);
            }
            if (munmap(addr, sizeof(int)) == -1){
                fprintf(stderr,"munmap failed - errno = %d\n",errno);
                exit(-1);
            }
            exit(EXIT_SUCCESS);
        }

    default:                    /* Parent: wait for child to terminate */
        if (wait(&status) == -1){
	    fprintf(stderr,"wait failed - errno = %d\n",errno);
	    exit(-1);
	}
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"child crashed or returned non-zero (status %x)\n", status);
            exit(-1);
	}
        printf("In parent, value = %d\n", *addr);
        if (*addr != 3) {
            fprintf(stderr,"value is %d, must be 3\n",*addr);
            exit(-1);
        }
    }

    /*
     * Part 2. Test unsuccessful parent-chlid access.
     */

    switch (fork()) {           /* Parent and child share mapping */
    case -1:
        fprintf(stderr,"fork failed - errno = %d\n",errno);
	exit(-1);
    case 0:                     /* Child: increment shared integer and exit */
        printf("Child started, ");
        printf("value = %d\n", *addr);
        (*addr)++;
        if (*addr != 4) {
            fprintf(stderr,"value is %d, must be 4\n",*addr);
            exit(-1);
        }
        if (munmap(addr, sizeof(int)) == -1){
                fprintf(stderr,"munmap failed - errno = %d\n",errno);
                exit(-1);
        }
        /* This should crash the child with SIGSEGV */
        (*addr)++;
        exit(EXIT_SUCCESS);
    default:                    /* Parent: wait for child to terminate */
        if (wait(&status) == -1){
            fprintf(stderr,"wait failed - errno = %d\n",errno);
            exit(-1);
        }
        if (WIFEXITED(status) || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
            fprintf(stderr,"child ended sucessfully or crashed not with SIGSEGV (status %x)\n", status);
            exit(-1);
        }
        printf("In parent, value = %d\n", *addr);
        if (*addr != 4) {
            fprintf(stderr,"value is %d, must be 4\n",*addr);
            exit(-1);
        }
        if (munmap(addr, sizeof(int)) == -1){
            fprintf(stderr,"munmap failed - errno = %d\n",errno);
            exit(-1);
        }
        exit(EXIT_SUCCESS);
    }
}
