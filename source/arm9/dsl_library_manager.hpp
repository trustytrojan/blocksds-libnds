// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef LIBNDS_DSL_LIBRARY_MANAGER_H__
#define LIBNDS_DSL_LIBRARY_MANAGER_H__

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

// Forward declaration
struct DslHandle;

// Library entry in the registry
struct DslLibraryEntry {
    DslHandle *handle;
    int refcount;
    bool loading;  // True while constructors are running (prevents cycles)
    
    DslLibraryEntry() : handle(nullptr), refcount(1), loading(true) {}
};

// Library manager - handles reference counting and prevents cycles
class DslLibraryManager {
private:
    std::unordered_map<std::string, DslLibraryEntry> libraries;
    
public:
    // Mark a library as being loaded (prevents cyclic dependencies)
    std::expected<DslLibraryEntry*, std::string_view> begin_load(const std::string& name);
    
    // Complete loading a library (store handle, clear loading flag)
    std::expected<void, std::string_view> complete_load(const std::string& name, DslHandle *handle);
    
    // Increment reference count (when library is already loaded)
    std::expected<DslHandle*, std::string_view> add_reference(const std::string& name);
    
    // Decrement reference count and return whether to actually unload
    std::expected<bool, std::string_view> release_reference(DslHandle *handle);
    
    // Abort a load that failed (remove from registry)
    void abort_load(const std::string& name);
    
    // Check if a library is currently being loaded
    bool is_loading(const std::string& name) const;
    
    // Find library name by handle (for error messages)
    std::string_view find_name(DslHandle *handle) const;
};

// Global library manager instance
extern DslLibraryManager g_library_manager;

#endif // LIBNDS_DSL_LIBRARY_MANAGER_H__
