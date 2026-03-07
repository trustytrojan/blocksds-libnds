// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2025 Antonio Niño Díaz

#include "dsl_library_manager.hpp"
#include "dsl_internal.hpp"

#include <algorithm>

// Global instance
DslLibraryManager g_library_manager;

std::expected<DslLibraryEntry*, std::string_view> 
DslLibraryManager::begin_load(const std::string& name)
{
    // Check if already loaded or being loaded
    auto it = libraries.find(name);
    if (it != libraries.end())
    {
        if (it->second.loading)
            return std::unexpected("cyclic dependency detected");
        
        // Already loaded - just increment refcount
        it->second.refcount++;
        return &it->second;
    }
    
    // Create new entry in loading state
    auto [new_it, inserted] = libraries.try_emplace(name);
    if (!inserted)
        return std::unexpected("failed to create library entry");
    
    return &new_it->second;
}

std::expected<void, std::string_view> 
DslLibraryManager::complete_load(const std::string& name, DslHandle *handle)
{
    auto it = libraries.find(name);
    if (it == libraries.end())
        return std::unexpected("library not in registry");
    
    it->second.handle = handle;
    it->second.loading = false;
    
    return {};
}

std::expected<DslHandle*, std::string_view> 
DslLibraryManager::add_reference(const std::string& name)
{
    auto it = libraries.find(name);
    if (it == libraries.end())
        return std::unexpected("library not found");
    
    if (it->second.loading)
        return std::unexpected("library still loading");
    
    it->second.refcount++;
    return it->second.handle;
}

std::expected<bool, std::string_view> 
DslLibraryManager::release_reference(DslHandle *handle)
{
    // Find the library by handle
    auto it = std::find_if(libraries.begin(), libraries.end(),
        [handle](const auto& pair) { return pair.second.handle == handle; });
    
    if (it == libraries.end())
        return std::unexpected("library not found in registry");
    
    if (it->second.refcount <= 0)
        return std::unexpected("invalid refcount");
    
    it->second.refcount--;
    
    bool should_unload = (it->second.refcount == 0);
    if (should_unload)
        libraries.erase(it);
    
    return should_unload;
}

void DslLibraryManager::abort_load(const std::string& name)
{
    libraries.erase(name);
}

bool DslLibraryManager::is_loading(const std::string& name) const
{
    auto it = libraries.find(name);
    if (it == libraries.end())
        return false;
    
    return it->second.loading;
}

std::string_view DslLibraryManager::find_name(DslHandle *handle) const
{
    auto it = std::find_if(libraries.begin(), libraries.end(),
        [handle](const auto& pair) { return pair.second.handle == handle; });
    
    if (it == libraries.end())
        return "";
    
    return it->first;
}
