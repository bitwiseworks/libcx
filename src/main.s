/*
 * main() hook implementation for kLIBC.
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

/*
 * Designed after kLIBC sys/i386/appinit.s and must be kept in sync with it
 * and with startup/i386/crt0.s. Also depends on a LIBC import in libcx.def
 * (_libc___init_app).
 */

  .globl  ___init_app
  .globl  ___main_hook_return

  .data

L_ret: .long 0

  .text

___init_app:
  popl L_ret
  /* do original kLIBC work */
  call __libc__init_app
  /* esp points to main() call frame. */
  push %esp
  call ___main_hook
  /* Theoretically we never should get here. Force a SIGSEGV. */
  hlt

___main_hook_return:
  movl 4(%esp), %esp
  jmp *L_ret
