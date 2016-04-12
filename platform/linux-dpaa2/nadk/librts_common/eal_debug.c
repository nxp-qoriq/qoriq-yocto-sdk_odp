/*
 * Copyright (c) 2014 Freescale Semiconductor, Inc. All rights reserved.
 */

/*   Derived from DPDK's eal_debug.h
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <execinfo.h>
#include <stdarg.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <nadk_log.h>
#include <nadk_debug.h>
#include <nadk_common.h>
#include <nadk_internal.h>

#define BACKTRACE_SIZE 256

/* dump the stack of the calling core */
void nadk_dump_stack(void)
{
	void *func[BACKTRACE_SIZE];
	char **symb = NULL;
	int size;

	size = backtrace(func, BACKTRACE_SIZE);
	symb = backtrace_symbols(func, size);
	while (size > 0) {
		NADK_LOG(ERR, EAL,
			"%d: [%s]\n", size, symb[size - 1]);
		size--;
	}
}

/* not implemented in this environment */
void nadk_dump_registers(void)
{
	return;
}

/* call abort(), it will generate a coredump if enabled */
void __nadk_panic(const char *funcname, const char *format, ...)
{
	/* disable history */
	nadk_log_set_history(0);

	NADK_LOG(CRIT, EAL, "PANIC in %s()-%s:\n", funcname, format);
	printf("\n PANIC in %s():\n", funcname);
#ifndef NADK_LOGLIB_DISABLE
	if (nadk_logs.level) {
		va_list ap;
		va_start(ap, format);
		nadk_vlog(NADK_LOG_CRIT, NADK_LOGTYPE_EAL, format, ap);
		va_end(ap);
	}
#endif
	nadk_dump_stack();
	nadk_dump_registers();
	abort();
}

/*
 * Like nadk_panic this terminates the application. However, no traceback is
 * provided and no core-dump is generated.
 */
void
nadk_exit(int exit_code, __attribute__((unused)) const char *format, ...)
{
	/* disable history */
	nadk_log_set_history(0);

	if (exit_code != 0)
		NADK_LOG(CRIT, EAL, "Error - exiting with code: %d\n"
				"  Cause: ", exit_code);

	printf("\n EXIT in %d:\n", exit_code);
#ifndef NADK_LOGLIB_DISABLE
	if (nadk_logs.level) {
		va_list ap;
		va_start(ap, format);
		nadk_vlog(NADK_LOG_CRIT, NADK_LOGTYPE_EAL, format, ap);
		va_end(ap);
	}
#endif

#ifndef NADK_EAL_ALWAYS_PANIC_ON_ERROR
	exit(exit_code);
#else
	nadk_dump_stack();
	nadk_dump_registers();
	abort();
#endif
}
