// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz
//           (C) 2026 trustytrojan

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>

#include <nds/exceptions.h>
#include <nds/ndstypes.h>

#include "dsl.h"
#include "dsl_internal.h"
#include "dsl_library_manager.h"

// Global state
thread_local char *g_dl_err_str = nullptr;
thread_local DslHandle *g_dsl_current = nullptr;

// External C function from libc
extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *d);

// Forward declarations
static int dlopen_mode_validate(int mode);

// ============================================================================
// DslHandle destructor - cleanup in reverse order
// ============================================================================

DslHandle::~DslHandle()
{
    // Run fini_array destructors
    void *__fini_array_start = dlsym(this, "__fini_array_start");
    void *__fini_array_end = dlsym(this, "__fini_array_end");
    g_dl_err_str = nullptr; // Ignore errors

    if ((__fini_array_end != nullptr) && (__fini_array_start != nullptr))
    {
        size_t num_dtors = ((uintptr_t)__fini_array_end -
                            (uintptr_t)__fini_array_start) / 4;

        VoidFn *dtor = (VoidFn *)__fini_array_start;

        for (size_t i = 0; i < num_dtors; i++)
            dtor[num_dtors - i - 1]();
    }

    // Run registered destructors in reverse order
    for (int i = dtors_list.size() - 1; i >= 0; i--)
    {
        const DslDtor& dtor = dtors_list[i];
        dtor.fn(dtor.arg);
    }

    // Close dependencies - this decrements their refcounts
    for (auto dep_handle : dep_handles)
        dlclose(dep_handle);

    // RAII members (loaded_mem, sym_table, vectors) clean up automatically
}

// ============================================================================
// Constructor/destructor handling
// ============================================================================

// This is called by global constructors via __aeabi_atexit()
LIBNDS_NOINLINE
int __aeabi_atexit(void *arg, void (*func)(void *), void *dso_handle)
{
    (void)dso_handle;

    if (func == nullptr)
        return -1;

    if (g_dsl_current == nullptr)
    {
        // If no DSL file is being loaded, this is either a global destructor or
        // some function being added with atexit().
        return __cxa_atexit(func, arg, dso_handle);
    }
    else
    {
        // Register destructor to be called when library is unloaded
        g_dsl_current->dtors_list.push_back({func, arg});
    }

    return 0;
}

std::expected<void, std::string_view> dsl_run_ctors(DslHandle *handle)
{
    void *__bothinit_array_start = dlsym(handle, "__bothinit_array_start");
    void *__bothinit_array_end = dlsym(handle, "__bothinit_array_end");
    g_dl_err_str = nullptr; // Ignore errors

    if ((__bothinit_array_end != nullptr) && (__bothinit_array_start != nullptr))
    {
        size_t num_ctors = ((uintptr_t)__bothinit_array_end -
                            (uintptr_t)__bothinit_array_start) / 4;

        // Reserve space for destructors
        handle->dtors_list.reserve(num_ctors);

        g_dsl_current = handle;

        VoidFn *ctor = (VoidFn *)__bothinit_array_start;
        for (size_t i = 0; i < num_ctors; i++)
            ctor[i]();

        g_dsl_current = nullptr;
    }

    return {};
}

// ============================================================================
// Mode validation
// ============================================================================

static int dlopen_mode_validate(int mode)
{
    int unsupported_mask = RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE
                         | RTLD_NOLOAD | RTLD_DEEPBIND;

    if (mode & unsupported_mask)
    {
        g_dl_err_str = const_cast<char*>("unsupported mode parameter");
        return -1;
    }

    // RTLD_NOW or RTLD_LAZY need to be set, but only RTLD_NOW is supported.
    if ((mode & RTLD_NOW) == 0)
    {
        g_dl_err_str = const_cast<char*>("RTLD_NOW mode required");
        return -1;
    }

    return 0;
}

// ============================================================================
// dlopen implementation
// ============================================================================

void *dlopen_internal(const char *file, int mode)
{
    if ((file == nullptr) || (strlen(file) == 0))
    {
        g_dl_err_str = const_cast<char*>("no file provided");
        return nullptr;
    }

    if (dlopen_mode_validate(mode) != 0)
        return nullptr;

    std::string filename(file);

    // Try to begin loading - this handles already-loaded libraries and cycles
    auto entry_result = g_library_manager.begin_load(filename);
    if (!entry_result)
    {
        g_dl_err_str = const_cast<char*>(entry_result.error().data());
        return nullptr;
    }

    DslLibraryEntry *entry = *entry_result;

    // If library was already loaded (begin_load incremented refcount)
    if (entry->handle != nullptr && !entry->loading)
        return entry->handle;

    // Library is new, need to actually load it
    FILE *f = fopen(file, "rb");
    if (f == nullptr)
    {
        g_library_manager.abort_load(filename);
        g_dl_err_str = const_cast<char*>("file can't be opened");
        return nullptr;
    }

    // Load the DSL file content
    void *handle = dlopen_FILE_internal(f, mode);

    fclose(f);

    if (handle == nullptr)
    {
        // Error string already set by dlopen_FILE_internal
        g_library_manager.abort_load(filename);
        return nullptr;
    }

    // Complete the load
    auto complete_result = g_library_manager.complete_load(filename, static_cast<DslHandle*>(handle));
    if (!complete_result)
    {
        g_dl_err_str = const_cast<char*>(complete_result.error().data());
        delete static_cast<DslHandle*>(handle);
        g_library_manager.abort_load(filename);
        return nullptr;
    }

    return handle;
}

void *dlopen(const char *file, int mode)
{
    // Clear error string
    g_dl_err_str = nullptr;

    void *result = dlopen_internal(file, mode);

    // Ensure __aeabi_atexit is linked (hack to force linker inclusion)
    if (result == nullptr)
        __aeabi_atexit(nullptr, nullptr, nullptr);

    return result;
}

void *dlopen_FILE(FILE *f, int mode)
{
    // Clear error string
    g_dl_err_str = nullptr;

    if (dlopen_mode_validate(mode) != 0)
        return nullptr;

    // NOTE: dlopen_FILE bypasses the library registry since we don't have
    // a filename to track. The caller is responsible for managing the handle.
    return dlopen_FILE_internal(f, mode);
}

// ============================================================================
// dlclose implementation
// ============================================================================

int dlclose(void *handle)
{
    // Clear error string
    g_dl_err_str = nullptr;

    if (handle == nullptr)
    {
        g_dl_err_str = const_cast<char*>("invalid handle");
        return -1;
    }

    DslHandle *h = static_cast<DslHandle*>(handle);

    // Release the reference and check if we should actually unload
    auto release_result = g_library_manager.release_reference(h);
    if (!release_result)
    {
        g_dl_err_str = const_cast<char*>(release_result.error().data());
        return -1;
    }

    bool should_unload = *release_result;

    // Only delete if refcount reached zero
    if (should_unload)
        delete h;

    return 0;
}

// ============================================================================
// dlerror implementation
// ============================================================================

char *dlerror(void)
{
    // Return the current error string, but clear it so that the next call to
    // dlerror() returns NULL.

    char *curr_str = g_dl_err_str;
    g_dl_err_str = nullptr;

    return curr_str;
}

// ============================================================================
// dlsym implementation
// ============================================================================

void *dlsym(void *handle, const char *name)
{
    // Clear error string
    g_dl_err_str = nullptr;

    if ((handle == RTLD_NEXT) || (handle == RTLD_DEFAULT))
    {
        g_dl_err_str = const_cast<char*>("invalid handle");
        return nullptr;
    }

    if ((name == nullptr) || (strlen(name) == 0))
    {
        g_dl_err_str = const_cast<char*>("invalid symbol name");
        return nullptr;
    }

    DslHandle *h = static_cast<DslHandle*>(handle);
    uint8_t *loaded_mem = h->loaded_mem.get();
    dsl_symbol_table *sym_table = h->sym_table.get();

    if (sym_table == nullptr)
    {
        g_dl_err_str = const_cast<char*>("invalid symbol table");
        return nullptr;
    }

    for (unsigned int i = 0; i < sym_table->num_symbols; i++)
    {
        dsl_symbol *sym = &(sym_table->symbol[i]);

        // Only return public symbols
        if ((sym->attributes & DSL_SYMBOL_PUBLIC) == 0)
            continue;

        const char *sym_name = sym->name_str_offset + (const char *)sym_table;

        if (strcmp(sym_name, name) == 0)
        {
            if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                return (void *)sym->value;

            return (void *)(sym->value + loaded_mem);
        }
    }

    g_dl_err_str = const_cast<char*>("symbol not found");
    return nullptr;
}

// ============================================================================
// dlmembase implementation (extension)
// ============================================================================

void *dlmembase(void *handle)
{
    // Clear error string
    g_dl_err_str = nullptr;

    if (handle == nullptr)
    {
        g_dl_err_str = const_cast<char*>("invalid handle");
        return nullptr;
    }

    DslHandle *h = static_cast<DslHandle*>(handle);

    return h->loaded_mem.get();
}
