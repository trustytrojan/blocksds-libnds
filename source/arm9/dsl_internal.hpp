// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef LIBNDS_DSL_INTERNAL_H__
#define LIBNDS_DSL_INTERNAL_H__

#include <cstdio>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dsl.h"

// Forward declarations
struct DslHandle;
struct DslLoadedLib;

// Destructor entry for global objects
struct DslDtor {
    void (*fn)(void *);
    void *arg;
};

// RAII wrapper for symbol table
struct DslSymbolTable {
    std::unique_ptr<uint8_t[]> data;
    dsl_symbol_table *table;
    
    DslSymbolTable() : table(nullptr) {}
    
    explicit DslSymbolTable(size_t size) {
        data = std::make_unique<uint8_t[]>(size);
        table = reinterpret_cast<dsl_symbol_table*>(data.get());
    }
    
    dsl_symbol_table* get() { return table; }
    const dsl_symbol_table* get() const { return table; }
    
    operator bool() const { return table != nullptr; }
};

// Main handle structure with RAII
struct DslHandle {
    std::unique_ptr<uint8_t[]> loaded_mem;
    size_t loaded_mem_size;
    DslSymbolTable sym_table;
    std::vector<DslHandle*> dep_handles;
    std::vector<DslDtor> dtors_list;
    
    DslHandle() : loaded_mem_size(0) {}
    
    ~DslHandle(); // Defined in dlfcn.cpp
    
    // Prevent copying
    DslHandle(const DslHandle&) = delete;
    DslHandle& operator=(const DslHandle&) = delete;
};

// Global state
extern thread_local char *g_dl_err_str;
extern thread_local DslHandle *g_dsl_current;

// Dependency loading - now returns std::expected
std::expected<void, std::string_view> dsl_read_dep_names(FILE *f, uint8_t num_deps, std::vector<std::string>& dep_names);
std::expected<void, std::string_view> dsl_load_dependencies(DslHandle *handle, const std::vector<std::string>& dep_names, int mode);
std::expected<void, std::string_view> dsl_resolve_external_symbols(DslHandle *handle);

// Relocation functions (in dsl_relocations.cpp)
std::expected<void, std::string_view> dsl_apply_relocations(FILE *f, dsl_section_header *section,
                         uint8_t num_sections, uint8_t *loaded_mem,
                         size_t addr_space_size, size_t loaded_mem_size,
                         dsl_symbol_table *sym_table);

// Constructor/destructor handling
std::expected<void, std::string_view> dsl_run_ctors(DslHandle *handle);

// Internal dlopen
void *dlopen_internal(const char *file, int mode);
void *dlopen_FILE_internal(FILE *f, int mode);

// Public API functions (from dlfcn.h, but we need them internally)
extern "C" {
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
}

#endif // LIBNDS_DSL_INTERNAL_H__
