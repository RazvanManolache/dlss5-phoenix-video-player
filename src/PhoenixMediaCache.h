#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>

namespace phoenix_media_cache {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){close();}
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
struct EntryGuard {
    Handle mutex;bool held=false;
    explicit EntryGuard(const std::filesystem::path& root);
    ~EntryGuard(){if(held)ReleaseMutex(mutex.value);}
};
// Shared readers prevent cleanup; this is independent of the producer lock.
struct Lease {
    Handle handle;
    static std::shared_ptr<Lease> Acquire(const std::filesystem::path& directory);
};
struct Maintenance {size_t removed=0,deferred=0;uintmax_t freed=0;bool floorMet=false;};
Maintenance Maintain(const std::filesystem::path& root,uintmax_t freeFloor,bool clear);
} // namespace phoenix_media_cache
