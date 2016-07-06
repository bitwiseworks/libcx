# Makefile for poll(2) emulator
#
# $Id$
# ---------------------------------------------------------------------------

#############################################################################
# Definitions that you need to edit
#############################################################################

# ---------------------------------------------------------------------------
# Installation prefix

PREFIX		= /usr/local
LIBDIR		= $(PREFIX)/lib
INCDIR		= $(PREFIX)/include/sys

# ---------------------------------------------------------------------------
# Platform-specific bits
#

########
# *BSD #
########

# For GNU CC on *BSD. Should work for FreeBSD, NetBSD, OpenBSD and BSD/OS
LINK_SHARED	= $(CC) -shared
SHLIB_EXT	= so
SHLIB_NOVER	= $(LIB_NAME).$(SHLIB_EXT)
SHLIB		= $(LIB_NAME).$(SHLIB_EXT).$(VERSION)
SHLIB_INSTALLED = $(LIBDIR)/$(LIB_NAME).$(SHLIB_EXT).$(MAJOR)

############
# Mac OS X #
############

# Benjamin Reed <ranger@befunk.com>:
# On Mac OS X, comment out the above lines, and uncomment these instead.
#LINK_SHARED	= $(CC) -install_name $(PREFIX)/lib/$(SHLIB) \
#			-compatibility_version $(COMPAT_VERSION) \
#			-current_version $(VERSION) -dynamiclib
#SHLIB_EXT	= dylib
#SHLIB_NOVER	= $(LIB_NAME).$(SHLIB_EXT)
#SHLIB		= $(LIB_NAME).$(VERSION).$(SHLIB_EXT)
#SHLIB_INSTALLED= $(LIBDIR)/$(LIB_NAME).$(MAJOR).$(SHLIB_EXT)

# NOTE: If you have linkage problems on the Mac (see the POSSIBLE ISSUES
# section in the INSTALL file), uncomment the following definition. Be sure
# to use the same -D option when compiling source code that uses this
# library.

#POLL_RENAME = -Dpoll=pollemu

# ---------------------------------------------------------------------------

# If you have a BSD-compatible install(1), use:
INSTALL		= install -c

# If you do not have a BSD-compatible install(1), use:
#INSTALL	= ./install.sh -c

# ---------------------------------------------------------------------------
# Compilation and Linkage

MAJOR		= 1
MINOR		= 3
VERSION		= $(MAJOR).$(MINOR)
COMPAT_VERSION	= $(MAJOR)
CC		= cc
LIB_NAME	= libpoll
LIB		= $(LIB_NAME).a
COMPILE_STATIC	= $(CC) -c 
COMPILE_SHARED	= $(CC) -c -fPIC 
RANLIB		= ranlib
CPPFLAGS        = $(POLL_RENAME)

#############################################################################
# There should be no need to edit past this point
#############################################################################

.SUFFIXES: .po

.c.po:
	$(COMPILE_SHARED) $(CPP_FLAGS) $< -o $*.po
.c.o:
	$(COMPILE_STATIC) $(CPPFLAGS) $<

all:		libs
libs:		$(SHLIB) $(LIB)
test:		polltest

dirs:
		@echo "creating directories..."
		$(INSTALL) -m 755 -d $(LIBDIR)
		$(INSTALL) -m 755 -d $(INCDIR)

install:	all dirs
		$(INSTALL) -m 755 $(SHLIB) $(LIB) $(LIBDIR)
		ln -sf $(SHLIB) $(SHLIB_INSTALLED)
		ln -sf $(SHLIB) $(LIBDIR)/$(SHLIB_NOVER)
		$(INSTALL) -m 644 poll.h $(INCDIR)

clean:
		rm -f poll.po *.o $(LIB) $(SHLIB) $(SHLIB_NOVER) polltest

$(SHLIB):	poll.po
		$(LINK_SHARED) -o $(SHLIB) poll.po
$(LIB):		poll.o
		ar rv $(LIB) poll.o
polltest:	polltest.o
		$(CC) -o polltest polltest.o $(LIB)
