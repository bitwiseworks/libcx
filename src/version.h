/*
 * Version information.
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

#ifndef VERSION_H
#define VERSION_H

/*
 * LIBCx version number is defined here. Note that the major and minor numbers
 * affect names of the global shared memory and global mutex. This is in order
 * to avoid clashes with older LIBCx versions that might be loaded into memory
 * at application runtime and assumes that at least the minor version is bumped
 * each time there is any change in the binary layout of the global LIBCx
 * structures located in shared memory.
 */
#define VERSION_MAJOR 0
#define VERSION_MINOR 5
#define VERSION_BUILD 4

#define VERSION_MAJ_MIN_MACRO(maj, min) #maj "." #min
#define VERSION_MAJ_MIN VERSION_MAJ_MIN_MACRO(VERSION_MAJOR, VERSION_MINOR)

#endif // VERSION_H
