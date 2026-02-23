// SPDX-License-Identifier: Zlib
// SPDX-FileNotice: Modified from the original version by the BlocksDS project.
//
// Copyright (C) 2021-2023 agbabi contributors

// Context switching definitions

#ifndef UCONTEXT_H__
#define UCONTEXT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ucontext.h>

#include <nds/ndstypes.h>

/// Copies the current machine context into ucp
///
/// @param ucp
///     Pointer to context structure
/// @return
///     0
LIBNDS_NONNULL(1)
int getcontext(ucontext_t *ucp);

/// Sets the current context to ucp
///
/// @param ucp
///     Pointer to context structure
///
/// @return
///     Does not return
LIBNDS_NORETURN LIBNDS_NONNULL(1)
int setcontext(const ucontext_t *ucp);

/// Writes current context into oucp, and switches to ucp
///
/// @param oucp
///     Output address for current context
/// @param ucp
///     Context to swap to
///
/// @return
///     Although technically this does not return, it will appear to return 0
///     when switching to oucp
LIBNDS_NONNULL(1, 2)
int swapcontext(ucontext_t *__restrict__ oucp, const ucontext_t *__restrict__ ucp);

/// Modifies context ucp to invoke func with setcontext.
///
/// Before invoking, the caller must allocate a new stack for this context and
/// assign its address to ucp->uc_stack, and set a successor context to
/// ucp->uc_link.
///
/// @param ucp
///     Pointer to context structure
/// @param func
///     Function to invoke with __agbabi_setcontext
/// @param argc
///     Number of arguments passed to func
/// @param ...
///     List of arguments to be passed to func
LIBNDS_NONNULL(1, 2)
void makecontext(ucontext_t *ucp, void(*func)(void), int argc, ...);

#ifdef __cplusplus
}
#endif

#endif // UCONTEXT_H__
