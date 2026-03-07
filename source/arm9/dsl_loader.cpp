// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

#include <nds/arm9/cache.h>

#include "dsl.h"
#include "dsl_internal.hpp"
#include "dsl_library_manager.hpp"

// Buffer for error messages related to dependency loading
static char dsl_load_deps_errbuf[256];

std::expected<void, std::string_view> dsl_read_dep_names(FILE *f, uint8_t num_deps, std::vector<std::string>& dep_names)
{
    dep_names.clear();

    if (num_deps == 0)
        return {};

    dep_names.reserve(num_deps);

    for (uint8_t i = 0; i < num_deps; i++)
    {
        std::string name;
        name.reserve(32);

        while (true)
        {
            int value = fgetc(f);
            if (value == EOF)
                return std::unexpected("can't read dependency names");

            if (value == '\0')
                break;

            name.push_back(static_cast<char>(value));
        }

        dep_names.push_back(std::move(name));
    }

    return {};
}

std::expected<void, std::string_view> dsl_load_dependencies(DslHandle *handle, const std::vector<std::string>& dep_names, int mode)
{
    if (dep_names.empty())
        return {};

    handle->dep_handles.reserve(dep_names.size());

    for (const auto& dep_name : dep_names)
    {
        std::string load_name = dep_name + ".dsl";

        void *dep_handle = dlopen_internal(load_name.c_str(), mode);

        if (dep_handle == NULL)
        {
            // Use dlopen_internal's error string in our own as a reason
            snprintf(dsl_load_deps_errbuf, sizeof(dsl_load_deps_errbuf), 
                     "failed to load dependency '%s': %s", 
                     dep_name.c_str(), g_dl_err_str ? g_dl_err_str : "unknown error");
            return std::unexpected(std::string_view(dsl_load_deps_errbuf));
        }

        handle->dep_handles.push_back(static_cast<DslHandle*>(dep_handle));
    }

    return {};
}

std::expected<void, std::string_view> dsl_resolve_external_symbols(DslHandle *handle)
{
    dsl_symbol_table *sym_table = handle->sym_table.get();

    for (unsigned int i = 0; i < sym_table->num_symbols; i++)
    {
        dsl_symbol *sym = &(sym_table->symbol[i]);

        if ((sym->attributes & DSL_SYMBOL_EXTERNAL) == 0)
            continue;

        const char *sym_name = sym->name_str_offset + (const char *)sym_table;
        uintptr_t fallback_value = sym->value;

        for (auto dep_handle : handle->dep_handles)
        {
            void *addr = dlsym(dep_handle, sym_name);
            if (addr != NULL)
            {
                sym->value = (uintptr_t)addr;
                break;
            }
        }

        g_dl_err_str = NULL;

        if ((sym->value == fallback_value) && (fallback_value == 0))
            return std::unexpected("unresolved external symbol");
    }

    return {};
}

void *dlopen_FILE_internal(FILE *f, int mode)
{
    // Clear error string
    g_dl_err_str = NULL;

    if (f == NULL)
    {
        g_dl_err_str = const_cast<char*>("no FILE handle provided");
        return NULL;
    }

    // Read DSL header
    dsl_header header;
    if (fread(&header, sizeof(header), 1, f) != 1)
    {
        g_dl_err_str = const_cast<char*>("can't read DSL header");
        return NULL;
    }

    if ((header.magic != DSL_MAGIC) || (header.version != 0))
    {
        g_dl_err_str = const_cast<char*>("invalid DSL magic or version");
        return NULL;
    }

    uint8_t num_sections = header.num_sections;
    uint8_t num_deps = header.num_deps;
    uint32_t addr_space_size = header.addr_space_size;

    if (num_sections > 10)
    {
        g_dl_err_str = const_cast<char*>("too many sections");
        return NULL;
    }

    // Read section headers
    dsl_section_header section[10];
    if (fread(&section, sizeof(dsl_section_header), num_sections, f) != num_sections)
    {
        g_dl_err_str = const_cast<char*>("can't read DSL sections");
        return NULL;
    }

    // Calculate veneer pool size
    size_t total_relocs = 0;
    for (unsigned int s = 0; s < num_sections; s++)
    {
        if (section[s].type == DSL_SEGMENT_RELOCATIONS)
            total_relocs += section[s].size / 8; // sizeof(Elf32_Rel)
    }

    size_t veneer_pool_size = ((addr_space_size + 3) & ~3) - addr_space_size;
    veneer_pool_size += total_relocs * 8;
    size_t loaded_mem_size = (size_t)addr_space_size + veneer_pool_size;

    // Create handle with RAII
    auto handle = std::make_unique<DslHandle>();
    handle->loaded_mem = std::make_unique<uint8_t[]>(loaded_mem_size);
    handle->loaded_mem_size = loaded_mem_size;

    if (!handle->loaded_mem)
    {
        g_dl_err_str = const_cast<char*>("no memory to load sections");
        return NULL;
    }

    uint8_t *loaded_mem = handle->loaded_mem.get();

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
            g_dl_err_str = const_cast<char*>("sections not in order");
            return NULL;
        }

        if (type == DSL_SEGMENT_PROGBITS)
        {
            if (fread(loaded_mem + address, 1, size, f) != size)
            {
                g_dl_err_str = const_cast<char*>("section data can't be read");
                return NULL;
            }
        }
        else if (type == DSL_SEGMENT_RELOCATIONS)
        {
            // Skip section
            if (fseek(f, size, SEEK_CUR) != 0)
            {
                g_dl_err_str = const_cast<char*>("section data can't be skipped");
                return NULL;
            }
        }
    }

    // Read dependency names
    std::vector<std::string> dep_names;
    if (auto result = dsl_read_dep_names(f, num_deps, dep_names); !result)
    {
        g_dl_err_str = const_cast<char*>(result.error().data());
        return NULL;
    }

    // Load symbol table
    long int symbol_table_start = ftell(f);

    // Calculate size of symbol table by checking the size of the file
    if (fseek(f, 0, SEEK_END) != 0)
    {
        g_dl_err_str = const_cast<char*>("can't seek end of file");
        return NULL;
    }

    long int file_size = ftell(f);

    if (fseek(f, symbol_table_start, SEEK_SET) != 0)
    {
        g_dl_err_str = const_cast<char*>("can't seek symbol table");
        return NULL;
    }

    size_t symbol_table_size = file_size - symbol_table_start;

    handle->sym_table = DslSymbolTable(symbol_table_size);
    if (!handle->sym_table)
    {
        g_dl_err_str = const_cast<char*>("no memory to load symbol table");
        return NULL;
    }

    if (fread(handle->sym_table.data.get(), symbol_table_size, 1, f) != 1)
    {
        g_dl_err_str = const_cast<char*>("can't read symbol table");
        return NULL;
    }

    // Load dependencies (using std::expected internally)
    if (auto result = dsl_load_dependencies(handle.get(), dep_names, mode); !result)
    {
        g_dl_err_str = const_cast<char*>(result.error().data());
        return NULL;
    }

    // Resolve external symbols
    if (auto result = dsl_resolve_external_symbols(handle.get()); !result)
    {
        g_dl_err_str = const_cast<char*>(result.error().data());
        return NULL;
    }

    // Apply relocations
    if (auto result = dsl_apply_relocations(f, section, num_sections, loaded_mem,
                              addr_space_size, loaded_mem_size, 
                              handle->sym_table.get()); !result)
    {
        g_dl_err_str = const_cast<char*>(result.error().data());
        return NULL;
    }

    // Flush caches
    DC_FlushRange(loaded_mem, loaded_mem_size);
    IC_InvalidateRange(loaded_mem, loaded_mem_size);

    // Run constructors
    if (auto result = dsl_run_ctors(handle.get()); !result)
    {
        g_dl_err_str = const_cast<char*>(result.error().data());
        return NULL;
    }

    return handle.release();  // Transfer ownership to caller
}
