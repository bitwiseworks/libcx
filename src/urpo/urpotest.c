#define INCL_DOS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <process.h>


int main( int argc, char argv[])
{
   FILE* fp;
   int   rc, pid, i;
   char  path[_MAX_PATH];
   char  pathnew[_MAX_PATH];

   if (argc==1)
      for( i=0; i<10; i++) {
         //spawnl( P_NOWAIT, "urpotest.exe", "urpotest.exe", "dummy", NULL);
      }

   printf( "pid %d\n", getpid());

   sprintf( path, "rename_test.%d", getpid());
   sprintf( pathnew, "renamed_test.%d", getpid());

   fp = fopen( path, "w");
   fprintf( fp, "Hello world!\n");
   rc = unlink( path);
   printf( "pid %d, unlink rc=%d, errno=%d\n", getpid(), rc, errno);
   if (rc == -1)
      printf( "pid %d FATAL ERROR!\n", getpid());

   rc = rename( path, pathnew);
   printf( "pid %d, rename rc=%d, errno=%d\n", getpid(), rc, errno);
   if (rc == -1)
      printf( "pid %d FATAL ERROR!\n", getpid());

	for( i=0; i<2; i++) {
		pid = fork();
		if (pid == 0) {
			printf( "pid %d, child\n", getpid());
			sprintf( path, "unlink_child_test.%d", getpid());
			fp = fopen( path, "w");
			rc = unlink( path);
			if (rc == -1)
				printf( "pid %d FATAL ERROR!\n", getpid());
			fclose( fp);
			exit(0);
		}
		if (pid == -1) {
			printf( "pid %d, fork failed errno=%d\n", getpid(), errno);
		}
		//waitpid(pid, NULL, 0);
	}

   //puts( "Hit a key to go on...");
   //_getchar();

   fclose( fp);
   printf( "pid %d, done\n", getpid());
}

