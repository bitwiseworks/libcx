#ifndef __SHARED_H__
#define __SHARED_H__

#include <errno.h>
#include <sys/param.h>

#ifdef DEBUG
#define LOG(a) fprintf a
#else
#define LOG(a)
#endif

/* defines */
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
int uropAdd(const char *pszPathOld, const char *pszPathNew);
int uropPending(void);

#endif // __SHARED_H__
