#pragma once

// The production backend for BodyReadMemory.
//
// Readability is proven by ReadProcessMemory over the page-rounded request (one copy per contiguous span,
// ~1 µs per call regardless of the region the pointer sits in); write permission and executability are read
// per page from the working set (QueryWorkingSetEx: the page's Win32 protection in ~1 µs, never a fault, cost
// independent of region size), with one VirtualQuery for a page that is not resident so the answer never
// depends on residency. Nothing here raises an exception, so a vectored handler (a crash logger, Catch2) never
// sees a first-chance fault from this code. A VirtualQuery region cache was the original backend; its cost
// grows with the size of the region a pointer sits in (10 ms at the base of a 1 GB pool), which is why it went.
//
// Header-only Win32 so TPTests exercises the production code against real pages.

#include "BodyReadMemory.h"
#include "../../../Services/ReadProbe.h"

#include <psapi.h>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BodyTracking
{
constexpr uintptr_t kProbePage = 4096; // x64 Windows pages are 4 KB
constexpr uintptr_t kProbePageMask = ~static_cast<uintptr_t>(kProbePage - 1);

// The VirtualQuery predicate the working-set lookups fall back to for a page that is not resident.
inline bool QueryRegion(uintptr_t p, BodyMemoryRegion& region) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto access = info.Protect & 255;
    const bool write = access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    const bool execute = access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    if (!write && access != PAGE_READONLY && access != PAGE_EXECUTE_READ) return false;
    const auto begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (info.RegionSize > UINTPTR_MAX - begin) return false;
    region = {begin, begin + info.RegionSize, write, execute};
    return true;
}

// The same write/execute classification as QueryRegion, applied to a page's Win32 protection.
inline bool ProtectionWritable(uint32_t protection) noexcept
{
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto access = protection & 255;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}
inline bool ProtectionExecutable(uint32_t protection) noexcept
{
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto access = protection & 255;
    return access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

inline uintptr_t DispatchRtti(uintptr_t p, uintptr_t function) noexcept
{
    // Dispatch the slot that was just validated, rather than fetching it
    // again through a potentially changed vtable between check and call.
    return reinterpret_cast<uintptr_t (*)(void*)>(function)(reinterpret_cast<void*>(p));
}

struct RpmMemory
{
    static constexpr size_t kChunk = 64 * 1024;
    static constexpr size_t kWriteBatch = 64; // pages per working-set query; no allocation per call
    std::unordered_set<uintptr_t> WritablePages; // proven this invocation

    bool MergeIntervals() const noexcept { return true; }

    // Exactly the page-rounded span is copied; a page holding no requested byte is never touched.
    // A span whose last page is the top page of the address space is refused (its rounded end would wrap);
    // no user-mode object lives there on x64 Windows, so this is documented rather than special-cased.
    bool Probe(uintptr_t p, size_t bytes, BodyMemoryInterval& out, NativeReadStats& stats)
    {
        if (!p || !bytes || bytes > UINTPTR_MAX - p) return false;
        const auto begin = p & kProbePageMask;
        const auto last = (p + bytes - 1) & kProbePageMask;
        if (last > UINTPTR_MAX - kProbePage) return false;
        const auto end = last + kProbePage;
        // The copy target is never read: one buffer per thread, allocated once, never cleared.
        thread_local std::unique_ptr<uint8_t[]> scratch(new uint8_t[kChunk]);
        uint32_t copied{};
        for (auto chunk = begin; chunk < end; chunk += kChunk)
        {
            const auto size = static_cast<size_t>(std::min<uintptr_t>(kChunk, end - chunk));
            if (!ReadProbe::Copy(reinterpret_cast<const void*>(chunk), scratch.get(), size)) return false;
            copied += static_cast<uint32_t>(size);
        }
        stats.ProbeBytes += copied; // only spans that were proven count
        out = {begin, end};
        return true;
    }

    // Every page of [begin, end), each proven once per invocation, all of them in one working-set query.
    // Callers pass only spans that Probe accepted, so the pages are resident unless trimmed since; a
    // non-resident page is answered by one VirtualQuery of today's predicate and counted as a fallback.
    bool Writable(uintptr_t begin, uintptr_t end, NativeReadStats& stats)
    {
        if (!begin || end < begin) return false;
        PSAPI_WORKING_SET_EX_INFORMATION batch[kWriteBatch]{};
        size_t count{};
        auto page = begin & kProbePageMask;
        while (true)
        {
            const bool more = page < end;
            if (more && !WritablePages.contains(page)) batch[count++] = {reinterpret_cast<void*>(page), {}};
            if (count == kWriteBatch || (!more && count))
            {
                if (!QueryWritable(batch, count, stats)) return false;
                count = 0;
            }
            if (!more || page > UINTPTR_MAX - kProbePage) break;
            page += kProbePage;
        }
        return true;
    }
    bool QueryWritable(PSAPI_WORKING_SET_EX_INFORMATION* pages, size_t count, NativeReadStats& stats)
    {
        ++stats.WriteQueries;
        const bool queried = K32QueryWorkingSetEx(GetCurrentProcess(), pages, static_cast<DWORD>(count * sizeof(pages[0]))) != 0;
        for (size_t i = 0; i < count; ++i)
        {
            const auto address = reinterpret_cast<uintptr_t>(pages[i].VirtualAddress);
            bool writable{};
            if (queried && pages[i].VirtualAttributes.Valid)
                writable = ProtectionWritable(static_cast<uint32_t>(pages[i].VirtualAttributes.Win32Protection));
            else
            {
                ++stats.WriteQueryFallbacks;
                ++stats.VirtualQueries;
                BodyMemoryRegion region;
                writable = QueryRegion(address, region) && region.Write;
            }
            if (!writable) { ++stats.WriteQueryFailures; return false; }
            WritablePages.insert(address);
        }
        return true;
    }

    // The page's protection from the working set; a non-resident page falls back to one VirtualQuery.
    bool Executable(uintptr_t p, NativeReadStats& stats)
    {
        ++stats.ExecutableLookups;
        PSAPI_WORKING_SET_EX_INFORMATION page{reinterpret_cast<void*>(p & kProbePageMask), {}};
        if (K32QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) && page.VirtualAttributes.Valid)
            return ProtectionExecutable(static_cast<uint32_t>(page.VirtualAttributes.Win32Protection));
        ++stats.ExecutableFallbackHits;
        ++stats.VirtualQueries;
        BodyMemoryRegion region;
        return QueryRegion(p, region) && region.Execute;
    }
    bool KnownReadOnly(uintptr_t, size_t) const noexcept { return false; }
    uintptr_t Rtti(uintptr_t p, uintptr_t function) const noexcept { return DispatchRtti(p, function); }
};

// The backend BodyNative reads through.
using RuntimeMemory = RpmMemory;
}
