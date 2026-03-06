// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <nds/arm9/cache.h>
#include <nds/exceptions.h>
#include <nds/ndstypes.h>

#include "dsl.h"

// This holds a pointer to the last error caused by the functions in this file.
// All threads have their own pointer.
static __thread char *dl_err_str = NULL;

typedef struct {
    void (*fn) (void *);
    void *arg;
} dsl_dtor;

// This is the internal structure of a handle returned by dlopen().
typedef struct {
    void *loaded_mem;
    dsl_symbol_table *sym_table;

    void **dep_handles;
    int dep_count;

    dsl_dtor *dtors_list;
    int dtors_num;
    int dtors_max;
} dsl_handle;

typedef struct dsl_loaded_lib {
    char *name;
    dsl_handle *handle;
    int refcount;
    bool loading;
    struct dsl_loaded_lib *next;
} dsl_loaded_lib;

static dsl_loaded_lib *dsl_loaded_libs = NULL;

static int dlopen_mode_validate(int mode);
static void dsl_dep_names_free(char **dep_names, int dep_count);
static void dsl_close_handle(dsl_handle *h);
static void *dlopen_internal(const char *file, int mode);
static void *dlopen_FILE_internal(FILE *f, int mode, const char *origin_path);

// Some ELF-related definitions

// Check the following link for information about the relocations:
// https://github.com/ARM-software/abi-aa/blob/9498b4eef7b3616fafeab15bf6891ab365a071be/aaelf32/aaelf32.rst

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Word;

typedef struct {
    Elf32_Addr r_offset; // Location (virtual address)
    Elf32_Word r_info;   // (symbol table index << 8) | (type of relocation)
} Elf32_Rel;

#define R_ARM_NONE          0
#define R_ARM_ABS32         2
#define R_ARM_REL32         3
#define R_ARM_THM_CALL      10
#define R_ARM_BASE_PREL     25
#define R_ARM_GOT_BREL      26
#define R_ARM_CALL          28
#define R_ARM_JUMP24        29
#define R_ARM_THM_JUMP24    30
#define R_ARM_TARGET1       38
#define R_ARM_TLS_IE32      107
#define R_ARM_TLS_LE32      108

// Size of a thread control block. TLS relocations are generated relative to a
// location before tdata and tbss.
#define TCB_SIZE 8

// While the constructors of a library are being called, this pointer holds the
// address of the handle being loaded.
static __thread dsl_handle *dsl_current = NULL;

// fini_array isn't really used by global destructors. Instead, global
// constructors call __aeabi_atexit() so that the destructors are called in the
// opposite order of the constructors. Also, in case a global constructor isn't
// called, the destructor won't be called either. More information here:
// https://etherealwake.com/2021/09/crt-startup/#c-abi-extensions
LIBNDS_NOINLINE
int __aeabi_atexit(void *arg, void (*func) (void *), void *dso_handle)
{
    (void)dso_handle;

    if (func == NULL)
        return -1;

    if (dsl_current == NULL)
    {
        // If no DSL file is being loaded, this is either a global destructor or
        // some function being added with atexit().

        // This function is in libc:
        int __cxa_atexit(void (*fn) (void *), void *arg, void *d);

        return __cxa_atexit(func, arg, dso_handle);
    }
    else
    {
        dsl_handle *handle = dsl_current;

        if (handle->dtors_num == handle->dtors_max)
            libndsCrash("Too many dtors in dynamic lib");

        handle->dtors_list[handle->dtors_num].fn = func;
        handle->dtors_list[handle->dtors_num].arg = arg;

        handle->dtors_num++;
    }

    return 0;
}

static int dlopen_mode_validate(int mode)
{
    int unsupported_mask = RTLD_LAZY | RTLD_GLOBAL | RTLD_NODELETE
                         | RTLD_NOLOAD | RTLD_DEEPBIND;

    if (mode & unsupported_mask)
    {
        dl_err_str = "unsupported mode parameter";
        return -1;
    }

    // RTLD_NOW or RTLD_LAZY need to be set, but only RTLD_NOW is supported.
    if ((mode & RTLD_NOW) == 0)
    {
        dl_err_str = "RTLD_NOW mode required";
        return -1;
    }

    return 0;
}

static void dsl_dep_names_free(char **dep_names, int dep_count)
{
    if (dep_names == NULL)
        return;

    for (int i = 0; i < dep_count; i++)
        free(dep_names[i]);

    free(dep_names);
}

static int dsl_read_dep_names(FILE *f, uint8_t num_deps, char ***dep_names_out)
{
    *dep_names_out = NULL;

    if (num_deps == 0)
        return 0;

    char **dep_names = calloc(num_deps, sizeof(char *));
    if (dep_names == NULL)
    {
        dl_err_str = "no memory for dependency names";
        return -1;
    }

    for (uint8_t i = 0; i < num_deps; i++)
    {
        size_t capacity = 32;
        size_t length = 0;
        char *name = malloc(capacity);

        if (name == NULL)
        {
            dl_err_str = "no memory for dependency name";
            dsl_dep_names_free(dep_names, i);
            return -1;
        }

        while (1)
        {
            int value = fgetc(f);
            if (value == EOF)
            {
                free(name);
                dl_err_str = "can't read dependency names";
                dsl_dep_names_free(dep_names, i);
                return -1;
            }

            if (length == capacity)
            {
                size_t new_capacity = capacity * 2;
                char *new_name = realloc(name, new_capacity);
                if (new_name == NULL)
                {
                    free(name);
                    dl_err_str = "no memory for dependency name";
                    dsl_dep_names_free(dep_names, i);
                    return -1;
                }

                name = new_name;
                capacity = new_capacity;
            }

            name[length++] = value;

            if (value == '\0')
                break;
        }

        dep_names[i] = name;
    }

    *dep_names_out = dep_names;
    return 0;
}

static dsl_loaded_lib *dsl_loaded_lib_find_by_name(const char *name)
{
    for (dsl_loaded_lib *lib = dsl_loaded_libs; lib != NULL; lib = lib->next)
    {
        if (strcmp(lib->name, name) == 0)
            return lib;
    }

    return NULL;
}

static dsl_loaded_lib *dsl_loaded_lib_find_by_handle(dsl_handle *handle)
{
    for (dsl_loaded_lib *lib = dsl_loaded_libs; lib != NULL; lib = lib->next)
    {
        if (lib->handle == handle)
            return lib;
    }

    return NULL;
}

static dsl_loaded_lib *dsl_loaded_lib_add_loading(const char *name)
{
    dsl_loaded_lib *lib = calloc(1, sizeof(dsl_loaded_lib));
    if (lib == NULL)
        return NULL;

    lib->name = strdup(name);
    if (lib->name == NULL)
    {
        free(lib);
        return NULL;
    }

    lib->refcount = 1;
    lib->loading = true;
    lib->next = dsl_loaded_libs;
    dsl_loaded_libs = lib;

    return lib;
}

static void dsl_loaded_lib_remove(dsl_loaded_lib *lib)
{
    dsl_loaded_lib **it = &dsl_loaded_libs;

    while (*it != NULL)
    {
        if (*it == lib)
        {
            *it = lib->next;
            free(lib->name);
            free(lib);
            return;
        }

        it = &((*it)->next);
    }
}

static char *dsl_make_relative_path(const char *origin_path, const char *dep_name)
{
    if ((origin_path == NULL) || (dep_name == NULL))
        return NULL;

    if (dep_name[0] == '/')
        return NULL;

    const char *slash = strrchr(origin_path, '/');
    if (slash == NULL)
        return NULL;

    size_t dir_len = slash - origin_path + 1;
    size_t dep_len = strlen(dep_name);

    char *full_path = malloc(dir_len + dep_len + 1);
    if (full_path == NULL)
        return NULL;

    memcpy(full_path, origin_path, dir_len);
    memcpy(full_path + dir_len, dep_name, dep_len + 1);

    return full_path;
}

char dsl_load_deps_errbuf[50];

static int dsl_load_dependencies(dsl_handle *handle, char **dep_names, int dep_count,
                                 int mode, const char *origin_path)
{
    if (dep_count == 0)
        return 0;

    handle->dep_handles = calloc(dep_count, sizeof(void *));
    if (handle->dep_handles == NULL)
    {
        dl_err_str = "no memory for dependency handles";
        return -1;
    }
    handle->dep_count = dep_count;

    for (int i = 0; i < dep_count; i++)
    {
        char *resolved_path = dsl_make_relative_path(origin_path, dep_names[i]);
        const char *load_name = (resolved_path != NULL) ? resolved_path : dep_names[i];

        void *dep_handle = dlopen_internal(load_name, mode);

        free(resolved_path);

        if (dep_handle == NULL)
        {
            // Override dl_err_str set by dlopen_internal, as it is a less specific error than this:
            strcpy(dsl_load_deps_errbuf, "failed to load dependency: ");
            strcat(dsl_load_deps_errbuf, load_name);
            dl_err_str = dsl_load_deps_errbuf;
            return -1;
        }

        handle->dep_handles[i] = dep_handle;
    }

    return 0;
}

static int dsl_resolve_external_symbols(dsl_handle *handle)
{
    dsl_symbol_table *sym_table = handle->sym_table;

    for (unsigned int i = 0; i < sym_table->num_symbols; i++)
    {
        dsl_symbol *sym = &(sym_table->symbol[i]);

        if ((sym->attributes & DSL_SYMBOL_EXTERNAL) == 0)
            continue;

        const char *sym_name = sym->name_str_offset + (const char *)sym_table;
        uintptr_t fallback_value = sym->value;

        for (int d = 0; d < handle->dep_count; d++)
        {
            void *addr = dlsym(handle->dep_handles[d], sym_name);
            if (addr != NULL)
            {
                sym->value = (uintptr_t)addr;
                break;
            }
        }

        dl_err_str = NULL;

        if ((sym->value == fallback_value) && (fallback_value == 0))
        {
            dl_err_str = "unresolved external symbol";
            return -1;
        }
    }

    return 0;
}

static int dsl_apply_relocations(FILE *f, dsl_section_header *section,
                                 uint8_t num_sections, uint8_t *loaded_mem,
                                 dsl_symbol_table *sym_table)
{
    for (unsigned int i = 0; i < num_sections; i++)
    {
        int type = section[i].type;

        if (type != DSL_SEGMENT_RELOCATIONS)
            continue;

        size_t size = section[i].size;
        size_t data_offset = section[i].data_offset;

        if (fseek(f, data_offset, SEEK_SET) != 0)
        {
            dl_err_str = "can't seek relocations";
            return -1;
        }

        size_t num_relocs = size / sizeof(Elf32_Rel);

        for (size_t r = 0; r < num_relocs; r++)
        {
            Elf32_Rel rel;
            if (fread(&rel, sizeof(Elf32_Rel), 1, f) != 1)
            {
                dl_err_str = "can't read relocation";
                return -1;
            }

            int rel_type = rel.r_info & 0xFF;
            int rel_symbol = rel.r_info >> 8;

            if ((rel_type == R_ARM_ABS32) || (rel_type == R_ARM_TARGET1))
            {
                // R_ARM_TARGET1 behaves as R_ARM_ABS32 due to the linker option
                // -Wl,--target1-abs.
                uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                    *ptr = sym->value;
                else
                    *ptr += (uintptr_t)loaded_mem;
            }
            else if (rel_type == R_ARM_THM_CALL)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t bl_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;
#if 0
                    // Note: In ARM7 it isn't possible to do interworking calls
                    // (from Thumb to ARM) because BLX doesn't exist. This check
                    // will be required if we want to enable dynamic libraries
                    // on the ARM7.
                    if ((sym_addr & 1) == 0)
                    {
                        dl_err_str = "R_ARM_THM_CALL can't switch to ARM in ARMv4";
                        return -1;
                    }
#endif
                    bool to_arm = false;
                    if ((sym_addr & 1) == 0)
                        to_arm = true;

                    int32_t jump_value = sym_addr - bl_addr;

                    if (to_arm)
                        jump_value -= 2;
                    else
                        jump_value -= 4;

                    if ((jump_value > 0x3FFFFF) | (jump_value <= -0x3FFFFF))
                    {
                        dl_err_str = "R_ARM_THM_CALL outside of range";
                        return -1;
                    }

                    uint16_t *ptr = (uint16_t *)(loaded_mem + rel.r_offset);

                    ptr[0] = 0xF000 | (0x07FF & (jump_value >> 12));

                    if (to_arm)
                        ptr[1] = 0xE800 | (0x07FE & (jump_value >> 1));
                    else
                        ptr[1] = 0xF800 | (0x07FF & (jump_value >> 1));
                }
            }
            else if (rel_type == R_ARM_JUMP24)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t b_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;

                    if ((sym_addr & 1) == 1)
                    {
                        dl_err_str = "R_ARM_JUMP24 jump to Thumb";
                        return -1;
                    }

                    int32_t jump_value = sym_addr - b_addr;
                    jump_value -= 6;

                    if ((jump_value > 0x7FFFFF) | (jump_value <= -0x7FFFFF))
                    {
                        dl_err_str = "R_ARM_JUMP24 outside of range";
                        return -1;
                    }

                    uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                    *ptr = (*ptr & 0xFF000000)
                         | ((jump_value >> 2) & 0x00FFFFFF);
                }
            }
            else if (rel_type == R_ARM_CALL)
            {
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                {
                    uint32_t bl_addr = (uint32_t)(loaded_mem + rel.r_offset);
                    uint32_t sym_addr = sym->value;
#if 0
                    // Note: In ARM7 it isn't possible to do interworking calls
                    // (from ARM to Thumb) because BLX doesn't exist. This check
                    // will be required if we want to enable dynamic libraries
                    // on the ARM7.
                    if ((sym_addr & 1) == 0)
                    {
                        dl_err_str = "R_ARM_CALL can't switch to Thumb in ARMv4";
                        return -1;
                    }
#endif
                    bool to_arm = false;
                    if ((sym_addr & 1) == 0)
                        to_arm = true;

                    int32_t jump_value = sym_addr - bl_addr;

                    if (to_arm)
                        jump_value -= 6;
                    else
                        jump_value -= 8;

                    if ((jump_value > 0x7FFFFF) | (jump_value <= -0x7FFFFF))
                    {
                        dl_err_str = "R_ARM_CALL outside of range";
                        return -1;
                    }

                    uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);

                    if (!to_arm)
                    {
                        *ptr = 0xFA000000
                             | ((jump_value >> 2) & 0x00FFFFFF)
                             | ((jump_value & BIT(1)) << 23);
                    }
                    else
                    {
                        *ptr = (*ptr & 0xFF000000)
                             | ((jump_value >> 2) & 0x00FFFFFF);
                    }
                }
            }
            else if (rel_type == R_ARM_TLS_LE32)
            {
                uint32_t *ptr = (uint32_t *)(loaded_mem + rel.r_offset);
                dsl_symbol *sym = &(sym_table->symbol[rel_symbol]);

                *ptr = sym->value + TCB_SIZE;
            }
            else
            {
                dl_err_str = "unknown relocation";
                return -1;
            }
        }

        break;
    }

    return 0;
}

static int dsl_run_ctors(dsl_handle *handle)
{
    void *__bothinit_array_start = dlsym(handle, "__bothinit_array_start");
    void *__bothinit_array_end = dlsym(handle, "__bothinit_array_end");
    dl_err_str = NULL; // Ignore errors

    handle->dtors_list = NULL;
    handle->dtors_num = 0;
    handle->dtors_max = 0;

    if ((__bothinit_array_end != NULL) && (__bothinit_array_start != NULL))
    {
        size_t num_ctors = ((uintptr_t)__bothinit_array_end -
                            (uintptr_t)__bothinit_array_start) / 4;

        handle->dtors_list = calloc(num_ctors, sizeof(dsl_dtor));
        if (handle->dtors_list == NULL)
        {
            dl_err_str = "no memory for destructors";
            return -1;
        }
        handle->dtors_max = num_ctors;

        dsl_current = handle;

        VoidFn *ctor = __bothinit_array_start;
        for (size_t i = 0; i < num_ctors; i++)
            ctor[i]();

        dsl_current = NULL;
    }

    return 0;
}

static void dsl_close_handle(dsl_handle *h)
{
    if (h == NULL)
        return;

    void *__fini_array_start = dlsym(h, "__fini_array_start");
    void *__fini_array_end = dlsym(h, "__fini_array_end");
    dl_err_str = NULL; // Ignore errors

    if ((__fini_array_end != NULL) && (__fini_array_start != NULL))
    {
        size_t num_dtors = ((uintptr_t)__fini_array_end -
                            (uintptr_t)__fini_array_start) / 4;

        VoidFn *dtor = __fini_array_start;

        for (size_t i = 0; i < num_dtors; i++)
            dtor[num_dtors - i - 1]();
    }

    for (int i = 0; i < h->dtors_num; i++)
    {
        dsl_dtor *dtor = &(h->dtors_list[h->dtors_num - i - 1]);
        dtor->fn(dtor->arg);
    }

    for (int i = 0; i < h->dep_count; i++)
        dlclose(h->dep_handles[i]);

    free(h->loaded_mem);
    free(h->sym_table);
    free(h->dep_handles);
    free(h->dtors_list);
    free(h);
}

static void *dlopen_FILE_internal(FILE *f, int mode, const char *origin_path)
{
    // Clear error string
    dl_err_str = NULL;

    uint8_t *loaded_mem = NULL;
    dsl_handle *handle = NULL;
    dsl_symbol_table *sym_table = NULL;

    char **dep_names = NULL;
    int dep_count = 0;

    if (dlopen_mode_validate(mode) != 0)
        return NULL;

    // RTLD_LOCAL is the default setting, but it doesn't need to be set
    // manually.

    if (f == NULL)
    {
        dl_err_str = "no FILE handle provided";
        return NULL;
    }

    uint8_t num_sections;
    uint8_t num_deps;
    uint32_t addr_space_size;

    {
        dsl_header header;
        if (fread(&header, sizeof(header), 1, f) != 1)
        {
            dl_err_str = "can't read DSL header";
            goto cleanup;
        }

        if ((header.magic != DSL_MAGIC) || (header.version != 0))
        {
            dl_err_str = "invalid DSL magic or version";
            goto cleanup;
        }

        num_sections = header.num_sections;
        num_deps = header.num_deps;
        addr_space_size = header.addr_space_size;
    }

    loaded_mem = malloc(addr_space_size);
    if (loaded_mem == NULL)
    {
        dl_err_str = "no memory to load sections";
        goto cleanup;
    }

    dsl_section_header section[10];
    if (num_sections > 10)
    {
        dl_err_str = "too many sections";
        goto cleanup;
    }

    if (fread(&section, sizeof(dsl_section_header), num_sections, f) != num_sections)
    {
        dl_err_str = "can't read DSL sections";
        goto cleanup;
    }

    // Load progbits sections and clear nobits sections. Skip relocations.
    for (unsigned int i = 0; i < num_sections; i++)
    {
        uintptr_t address = section[i].address;
        size_t size = section[i].size;
        int type = section[i].type;
        size_t data_offset = section[i].data_offset;

        if (type == DSL_SEGMENT_NOBITS)
        {
            memset(loaded_mem + address, 0, size);
            continue;
        }

        // Check this only for sections with data
        long int cursor = ftell(f);
        if ((size_t)cursor != data_offset)
        {
            dl_err_str = "sections not in order";
            goto cleanup;
        }

        if (type == DSL_SEGMENT_PROGBITS)
        {
            if (fread(loaded_mem + address, 1, size, f) != size)
            {
                dl_err_str = "section data can't be read";
                goto cleanup;
            }
        }
        else if (type == DSL_SEGMENT_RELOCATIONS)
        {
            // Skip section
            if (fseek(f, size, SEEK_CUR) != 0)
            {
                dl_err_str = "section data can't be skipped";
                goto cleanup;
            }
        }
    }

    dep_count = num_deps;

    if (dsl_read_dep_names(f, num_deps, &dep_names) != 0)
        goto cleanup;

    // Load symbol table
    // -----------------

    long int symbol_table_start = ftell(f);

    // Calculate size of symbol table by checking the size of the file
    if (fseek(f, 0, SEEK_END) != 0)
    {
        dl_err_str = "can't seek end of file";
        goto cleanup;
    }

    long int file_size = ftell(f);

    if (fseek(f, symbol_table_start, SEEK_SET) != 0)
    {
        dl_err_str = "can't seek symbol table";
        goto cleanup;
    }

    size_t symbol_table_size = file_size - symbol_table_start;

    sym_table = malloc(symbol_table_size);
    if (sym_table == NULL)
    {
        dl_err_str = "no memory to load symbol table";
        goto cleanup;
    }

    if (fread(sym_table, symbol_table_size, 1, f) != 1)
    {
        dl_err_str = "can't read symbol table";
        goto cleanup;
    }

    // Start preparing the handle
    // --------------------------

    handle = calloc(1, sizeof(dsl_handle));
    if (handle == NULL)
    {
        dl_err_str = "no memory to create handle";
        goto cleanup;
    }

    handle->loaded_mem = loaded_mem;
    handle->sym_table = sym_table;

    if (dsl_load_dependencies(handle, dep_names, dep_count, mode, origin_path) != 0)
        goto cleanup;

    if (dsl_resolve_external_symbols(handle) != 0)
        goto cleanup;

    // Apply relocations
    // -----------------

    if (dsl_apply_relocations(f, section, num_sections, loaded_mem, sym_table) != 0)
        goto cleanup;

    // Now that we have finished loading and handling relocations we need to
    // flush the data cache. If not, the instruction cache won't see the updated
    // code in main RAM! Also, we need to clear the instruction cache in case
    // this range was already in cache because of a previous library, for
    // example.

    DC_FlushRange(loaded_mem, addr_space_size);
    IC_InvalidateRange(loaded_mem, addr_space_size);

    // After all the code is loaded check if there are any global constructors
    // and call them.

    if (dsl_run_ctors(handle) != 0)
        goto cleanup;

    dsl_dep_names_free(dep_names, dep_count);

    return handle;

cleanup:
    dsl_dep_names_free(dep_names, dep_count);

    if (handle != NULL)
    {
        // Preserve the error string from the function that failed.
        // dsl_close_handle() failing is not a big deal
        char *const prev_err = dl_err_str;
        dsl_close_handle(handle);
        dl_err_str = prev_err;
    }
    else
    {
        free(loaded_mem);
        free(sym_table);
    }

    // This is a hack to make sure that __aeabi_atexit() is always included in
    // the final binary if dlopen() is used. __aeabi_atexit() is marked as
    // "noinline", so this will force the linker to include it.
    __aeabi_atexit(NULL, NULL, NULL);

    return NULL;
}

void *dlopen_FILE(FILE *f, int mode)
{
    return dlopen_FILE_internal(f, mode, NULL);
}

static void *dlopen_internal(const char *file, int mode)
{
    if ((file == NULL) || (strlen(file) == 0))
    {
        dl_err_str = "no file provided";
        return NULL;
    }

    if (dlopen_mode_validate(mode) != 0)
        return NULL;

    dsl_loaded_lib *lib = dsl_loaded_lib_find_by_name(file);
    if (lib != NULL)
    {
        if (lib->loading)
        {
            dl_err_str = "cyclic dependency detected";
            return NULL;
        }

        lib->refcount++;
        return lib->handle;
    }

    lib = dsl_loaded_lib_add_loading(file);
    if (lib == NULL)
    {
        dl_err_str = "no memory to create library record";
        return NULL;
    }

    FILE *f = fopen(file, "rb");
    if (f == NULL)
    {
        dsl_loaded_lib_remove(lib);
        dl_err_str = "file can't be opened";
        return NULL;
    }

    void *p = dlopen_FILE_internal(f, mode, file);

    fclose(f);

    if (p == NULL)
    {
        dsl_loaded_lib_remove(lib);
        return NULL;
    }

    lib->handle = p;
    lib->loading = false;

    return p;
}

void *dlopen(const char *file, int mode)
{
    // Clear error string
    dl_err_str = NULL;

    return dlopen_internal(file, mode);
}

int dlclose(void *handle)
{
    // Clear error string
    dl_err_str = NULL;

    if (handle == NULL)
    {
        dl_err_str = "invalid handle";
        return -1;
    }

    dsl_handle *h = handle;
    dsl_loaded_lib *lib = dsl_loaded_lib_find_by_handle(h);

    if (lib != NULL)
    {
        if (lib->refcount <= 0)
        {
            dl_err_str = "invalid library state";
            return -1;
        }

        lib->refcount--;
        if (lib->refcount > 0)
            return 0;

        dsl_loaded_lib_remove(lib);
    }

    dsl_close_handle(h);

    return 0;
}

char *dlerror(void)
{
    // Return the current error string, but clear it so that the  next call to
    // dlerror() returns NULL.

    char *curr_str = dl_err_str;

    dl_err_str = NULL;

    return curr_str;
}

void *dlsym(void *handle, const char *name)
{
    // Clear error string
    dl_err_str = NULL;

    if ((handle == RTLD_NEXT) || (handle == RTLD_DEFAULT))
    {
        dl_err_str = "invalid handle";
        return NULL;
    }

    if ((name == NULL) || (strlen(name) == 0))
    {
        dl_err_str = "invalid symbol name";
        return NULL;
    }

    char *loaded_mem = ((dsl_handle *)handle)->loaded_mem;
    dsl_symbol_table *sym_table = ((dsl_handle *)handle)->sym_table;

    for (unsigned int i = 0; i < sym_table->num_symbols; i++)
    {
        dsl_symbol *sym = &(sym_table->symbol[i]);

        const char *sym_name = sym->name_str_offset + (const char *)sym_table;

        // Only return public symbols
        if ((sym->attributes & DSL_SYMBOL_PUBLIC) == 0)
            continue;

        if (strcmp(sym_name, name) == 0)
        {
            if (sym->attributes & DSL_SYMBOL_EXTERNAL)
                return (void *)sym->value;

            return sym->value + loaded_mem;
        }
    }

    dl_err_str = "symbol not found";
    return NULL;
}

void *dlmembase(void *handle)
{
    // Clear error string
    dl_err_str = NULL;

    if (handle == NULL)
    {
        dl_err_str = "invalid handle";
        return NULL;
    }

    dsl_handle *h = handle;

    return h->loaded_mem;
}
