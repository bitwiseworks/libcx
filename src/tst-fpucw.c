/*
 * Testcase for FPU control word recovery.
 * Copyright (C) 2018 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2018.
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

#include <stdio.h>
#include <float.h>
#include <math.h>

#include "shared.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "test-skeleton.c"

void screw_fpucw()
{
	// 0x362 is what some Gpi/Win APIs leave in the FPU CW after themselves.

	__asm__(
		"pushl $0x362;"
		"fldcw 0(%esp);"
		"popl %eax"
	);
}

static int do_test(void)
{
	float a = 1.;
	float b = 0.;

	printf("before screw: fpu_cw=%x\n",_control87(0, 0));

	screw_fpucw();

	printf("after screw: fpu_cw=%x\n", _control87(0, 0));

	volatile float c = a / b;

	if (c != INFINITY)
	{
		printf("c is %f, not %f (fpu_cw=%x)\n", c, INFINITY, _control87(0, 0));
		return 1;
	}

  printf("after fix: fpu_cw=%x\n", _control87(0, 0));

  volatile float c2 = 2. / 0.;

	if (c2 != INFINITY)
	{
		printf("c2 is %f, not %f (fpu_cw=%x)\n", c2, INFINITY, _control87(0, 0));
		return 1;
	}

	return 0;
}
