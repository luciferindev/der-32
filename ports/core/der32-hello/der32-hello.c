/*
 * der32-hello -- print the system identity as reported by the kernel.
 * Copyright (c) 2026 The DER/32 Project.  SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/param.h>
#include <sys/utsname.h>
#ifdef __NetBSD__
#include <sys/sysctl.h>
#endif
#include <stdio.h>

#ifndef PKGVERSION
#define PKGVERSION "1.0"
#endif

int
main(void)
{
	struct utsname u;

	if (uname(&u) == -1) {
		perror("uname");
		return 1;
	}
	printf("der32-hello %s: %s %s %s\n", PKGVERSION, u.sysname, u.release,
	    u.machine);
#ifdef __NetBSD__
	{
		char rel[32], code[32];
		size_t rl = sizeof(rel), cl = sizeof(code);

		if (sysctlbyname("kern.der32.release", rel, &rl, NULL, 0) == 0 &&
		    sysctlbyname("kern.der32.codename", code, &cl, NULL, 0) == 0)
			printf("kern.der32: release %s \"%s\"\n", rel, code);
		else
			printf("kern.der32: not available (not a DER/32 kernel)\n");
	}
#endif
	return 0;
}
