#ifndef __SHARED_H__
#define __SHARED_H__

#define INCL_FSMACROS
#define INCL_ERRORS
#define INCL_DOS
#define INCL_EXAPIS
#include <os2emx.h>

#include <errno.h>
#include <sys/param.h>

#ifdef DEBUG
#define LOG(a) fprintf a
#else
#define LOG(a)
#endif

/* defines */
#define SHMEM_UROP	"\\SHAREMEM\\UROP1"
#define SEM_UROP	"\\SEM32\\UROP1"
#define MAX_UROP		500


/* typedefs */
typedef struct _UROP
{
    char 			szPathOld[PATH_MAX];
    char			szPathNew[PATH_MAX];
} UROP;

typedef struct _MUROP
{
    int			cUROP;
    UROP			urop[MAX_UROP];
} MUROP;


/* prototypes */
int uropInit(void);
int uropAdd( const char *pszPathOld, const char *pszPathNew);

#endif // __SHARED_H__
