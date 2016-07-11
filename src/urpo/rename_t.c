#define INCL_DOS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <process.h>


int main( void)
{
   FILE* fp;
   int   rc;
   char  pathnew[_MAX_PATH];

   printf( "pid %d\n", getpid());
   fp = fopen( "rename_t.old", "w");
   fprintf( fp, "Hello world!\n");
   sprintf( pathnew, "rename_t.%d", getpid());
   rc = unlink( "rename_t.old");
   printf( "pid %d, unlink rc=%d, errno=%d\n", getpid(), rc, errno);
   rc = rename( "rename_t.old", pathnew);
   printf( "pid %d, rename rc=%d, errno=%d\n", getpid(), rc, errno);

fork();

   puts( "Hit a key to go on...");
   _getchar();

   fclose( fp);
   printf( "pid %d, done\n", getpid());
}
