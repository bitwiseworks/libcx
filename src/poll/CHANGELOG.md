
Version 1.5.1 (2011-08-29)

* Updated out-of-date license clauses in poll.h and poll.c to be consistent
  with the license in the LICENSE file.

Version 1.5 (2009-08-14)

* Added nfds_t typedef to poll.h and changed poll() function to use that
  type. This change enhances portability for applications that are ported
  from systems using newer versions of poll(2).

* Added instructions (in README) and optional -D parameter (in Makefile)
  for working around linkage problems on some Mac OS X systems.

* Updated license to an OSI-approved BSD license, which is more compatible
  with applications using version 3 of the GNU Public License (GPL).

Thanks to John Gilmore (*gnu@toad.com*) for noting these problems and
suggesting the necessary fixes.

Version 1.4 (2003-01-01)

* Now handles and ignores bad file descriptors (i.e., descriptors with
  negative values) in poll array. Change is based on a patch submitted by
  Dave Smith (*dizzyd@jabber.org*)

Version 1.3 (2002-09-30)

* Incorporated changes to Makefile for building on Mac OS/X. (Thanks to
  Benjamin Reed (*ranger@befunk.com*)

Version 1.2 (2002-05-11)

* Changed WillsCreek.com references to clapper.org.
* Added license info to source code comments

