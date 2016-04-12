/*
 * Copyright (c) 2014 Freescale Semiconductor, Inc. All rights reserved.
 */
/*   Derived from DPDK's rte_debug.h
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

#ifndef _NADK_DEBUG_H_
#define _NADK_DEBUG_H_

/*!
 * @file nadk_debug.h
 *
 * @brief Debug Functions in RTE
 * This file defines a generic API for debug operations. Part of
 * the implementation is architecture-specific.
 * @addtogroup NADK_LOG
 * @ingroup NADK_RTS
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Dump the stack of the calling core to the console.
 */
void nadk_dump_stack(void);

/*!
 * Dump the registers of the calling core to the console.
 *
 * Note: Not implemented in a userapp environment; use gdb instead.
 */
void nadk_dump_registers(void);

/*!
 * Provides notification of a critical non-recoverable error and terminate
 * execution abnormally.
 *
 * Displays the format string and its expanded arguments (printf-like).
 *
 * In a linuxapp environment, this function dumps the stack and calls
 * abort() resulting in a core dump if enabled.
 *
 * The function never returns.
 *
 * @param format
 *   The format string
 * @param args
 *   The variable list of arguments.
 */
#define nadk_panic(...) nadk_panic_(__func__, __VA_ARGS__, "dummy")
/*! Intenally used. See/use 'nadk_panic(...)' */
#define nadk_panic_(func, format, ...) __nadk_panic(func, format "%.0s", __VA_ARGS__)

/*!< Panic if 'exp' is zero/NULL*/
#define	NADK_VERIFY(exp)	do {                                                  \
	if (!(exp))                                                           \
		nadk_panic("line %d\tassert \"" #exp "\" failed\n", __LINE__); \
} while (0)

/*!
 * Provide notification of a critical non-recoverable error and stop.
 *
 * This function should not be called directly. Refer to nadk_panic() macro
 * documentation.
 */
void __nadk_panic(const char *funcname, const char *format, ...)
#ifdef __GNUC__
#if (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 2))
	__attribute__((cold))
#endif
#endif
	__attribute__((noreturn))
	__attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

/*! @} */
#endif /* _NADK_DEBUG_H_ */
