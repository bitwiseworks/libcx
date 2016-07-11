#define INCL_DOS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#define INCL_DOS
#include <os2.h>

int main( int argc, char* argv[])
{
   printf( "uropdump pid %d\n", getpid());
   if (argc == 2 && !strcmp( argv[1], "-D")) {
      printf( "uropdump daemon mode\n");
      while(1) {
         DosSleep( 5*60*1000);
      }
   }
   if (argc == 2 && !strcmp( argv[1], "-R")) {
      printf( "uropdump reset data\n");
      urpoReset();
   }
   urpoDump();
}
