// SPDX-License-Identifier: Zlib
// SPDX-FileNotice: Modified from the original version by the BlocksDS project.
//
// Copyright (C) 2021-2023 agbabi contributors

#ifndef NDSABI_H__
#define NDSABI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include <nds/ndstypes.h>

/// Copies n bytes from src to dest (forward)
///
/// Assumes dest and src are 2-byte aligned
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy
LIBNDS_NONNULL(1, 2)
void __ndsabi_memcpy2(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Copies n bytes from src to dest (forward)
///
/// This is a slow, unaligned, byte-by-byte copy: ideal for SRAM
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy
LIBNDS_NONNULL(1, 2)
void __ndsabi_memcpy1(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Copies n bytes from src to dest (backwards)
///
/// This is a slow, unaligned, byte-by-byte copy: ideal for SRAM
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy
LIBNDS_NONNULL(1, 2)
void __ndsabi_rmemcpy1(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Copies n bytes from src to dest (backwards)
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy
LIBNDS_NONNULL(1, 2)
void __ndsabi_rmemcpy(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Copies n bytes in multiples of 16 bytes from src to dest (forward) using FIQ mode
///
/// Assumes dest and src are 4-byte aligned
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy, must be a multiple of 16
LIBNDS_NONNULL(1, 2)
void __ndsabi_fiq_memcpy4x4(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Copies n bytes from src to dest (forward) using FIQ mode
///
/// Assumes dest and src are 4-byte aligned
///
/// @param dest
///     Destination address
/// @param src
///     Source address
/// @param n
///     Number of bytes to copy
LIBNDS_NONNULL(1, 2)
void __ndsabi_fiq_memcpy4(void *__restrict__ dest, const void *__restrict__ src, size_t n);

/// Fills dest with n bytes of c
///
/// Assumes dest is 4-byte aligned.
///
/// Trailing copy uses the low word of c, and the low byte of c.
///
/// @param dest
///     Destination address
/// @param n
///     Number of bytes to set
/// @param c
///     Value to set
LIBNDS_NONNULL(1)
void __ndsabi_lwordset4(void *dest, size_t n, long long c);

/// Fills dest with n bytes of c.
///
/// Assumes dest is 4-byte aligned.
///
/// Trailing copy uses the low byte of c.
///
/// @param dest
///     Destination address
/// @param n
///     Number of bytes to set
/// @param c
///     Value to set
LIBNDS_NONNULL(1)
void __ndsabi_wordset4(void *dest, size_t n, int c);

/// Coroutine state
typedef struct
{
    uint32_t arm_sp : 31; ///< Pointer to coroutine stack
    uint32_t joined : 1;  ///< Flag if the coroutine has joined
    uint32_t arg;
} __ndsabi_coro_t;

/// Initializes a coro struct to call a given coroutine
///
/// @param coro
///     Pointer to coro struct to initialize
/// @param sp_top
///     The TOP of the stack for this coroutine (stack grows down!)
/// @param coproc
///     Procedure to call as a coroutine
/// @param arg
///     Initial argument to be passed to the coroutine.
LIBNDS_NONNULL(1, 2, 3)
void __ndsabi_coro_make(__ndsabi_coro_t* __restrict__ coro,
    void* __restrict__ sp_top, int(*coproc)(__ndsabi_coro_t*, void*), void *arg);

/// Initializes a coro struct to call a given coroutine.
///
/// The only difference is that the coroutine won't get the coroutine context as
/// an argument. Because of that, this function can't be used for simple
/// coroutines, it's designed to be used by a multithreading scheduler.
///
/// @param coro
///     Pointer to coro struct to initialize
/// @param sp_top
///     The TOP of the stack for this coroutine (stack grows down!)
/// @param coproc
///     Procedure to call as a coroutine
/// @param arg
///     Initial argument to be passed to the coroutine.
LIBNDS_NONNULL(1, 2, 3)
void __ndsabi_coro_make_noctx(__ndsabi_coro_t* __restrict__ coro,
    void* __restrict__ sp_top, int(*coproc)(void*), void *arg);

/// Starts/resumes a given coroutine.
///
/// @param coro
///     Coroutine to start/resume
///
/// @return
///     Integer value from coroutine
LIBNDS_NONNULL(1)
int __ndsabi_coro_resume(__ndsabi_coro_t* coro);

/// Yields a given value of a coroutine back to the caller.
///
/// @param coro
///     Coroutine that is yielding
/// @param value
///     Returned to caller.
LIBNDS_NONNULL(1)
void __ndsabi_coro_yield(__ndsabi_coro_t* coro, int value);

#ifdef __cplusplus
}
#endif
#endif // NDSABI_H__
