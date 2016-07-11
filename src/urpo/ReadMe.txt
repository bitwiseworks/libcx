urpo: unlink rename pending operation
============================================
Copyright (C) 2008-16 Yuri Dario <yd@os2power.com>


Contents
--------
URPO is a static library created to allow deleting or renaming files still in
use by a program. 
The scope of this library is to allow log rotation for daemons, so it does not
cover all possible situations.


System requirements:
--------------------
- gcc 4.9.2 runtime: ftp://ftp.netlabs.org/pub/gcc/
- libc 0.6.6 runtime: ftp://ftp.netlabs.org/pub/gcc/


Usage
-----
The library is shipped in source form and in OMF .lib file format under the LGPL
license. It means you can use it with your LGPL/GPL code.
If you build it as dll, probably you can use it also with other licensed programs.
The library contains the following functions

    unlink/remove/rename/close/fclose

When unlink/rename operations fails and the error code is correct, the operation
is stored into a shared memory area for later usage.
The shared memory area storage is inspected when files are closed, and pending 
operations will be executed.
Since only the above functions are actually supported, other combinations could
lead to unpredictable results.
To use the library, put it into your /usr/lib and add -lurpo in your link command.

This library supports only gcc, but I think it could be easily adapted to
other OS/2 compilers. Please resend contributed code.
If files are locked by non-gcc executables, the required operation cannot be
executed at file close time, but since the list is always fully scanned, the
pending operation will be executed at the first close() call in libc: in this
case, there could be a severe delay (and more...).

For compatibility with applications, rename() supports only EBUSY and ETXTBSY
return error codes. If you need to force rename on close, you can modify your
source code and use

   #include <urpo.h>
   int renameForce( const char *pszPathOld, const char *pszPathNew);

to store the name in the pending operations list.


Support
-------
Since this is a OpenSource freeware product there are no
formal support options available.


Donations
---------
Since this software is ported for free, donations are welcome! you can use PayPal
to donate me and support OS/2 developement. See my homepage for details, or ask :-)
At least, a post card is welcome!


===============================================================================
Yuri Dario <yd@os2power.com>
http://web.os2power.com/yuri
