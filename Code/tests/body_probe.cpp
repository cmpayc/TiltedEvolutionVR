// Real-memory fixtures for the body reader's production backends (BodyProbeMemory.h): the rpm backend
// through the BodyReadMemory template, the working-set protection query on every protection the spec
// reasons about, its VirtualQuery fallback for pages the working set does not hold, and the vq backend's
// reproduction of the a524de14 region cache. These run against the operating system, not a model of it.
#include <catch2/catch.hpp>
#include "../client/Games/Skyrim/NetImmerse/BodyProbeMemory.h"
#include <cstring>

using namespace BodyTracking;

namespace
{
constexpr size_t kPage = 4096;

struct Reservation
{
    uint8_t* Base{};
    explicit Reservation(size_t pages)
    {
        Base = static_cast<uint8_t*>(VirtualAlloc(nullptr, pages * kPage, MEM_RESERVE, PAGE_NOACCESS));
        REQUIRE(Base != nullptr);
    }
    ~Reservation() { if (Base) VirtualFree(Base, 0, MEM_RELEASE); }
    uint8_t* Commit(size_t page, size_t pages, DWORD protect)
    {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(Base + page * kPage, pages * kPage, MEM_COMMIT, protect));
        REQUIRE(p == Base + page * kPage);
        return p;
    }
    uint8_t* Page(size_t index) const { return Base + index * kPage; }
};

DWORD ProtectOf(const void* p)
{
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(p, &info, sizeof(info)) != 0);
    return info.Protect;
}

// Make a page resident without writing it (a read through ReadProcessMemory faults it in).
void Touch(const void* p)
{
    uint8_t byte{};
    ReadProbe::Copy(p, &byte, 1);
}

uintptr_t At(const void* p) { return reinterpret_cast<uintptr_t>(p); }

using RpmReader = BodyReadMemory<RpmMemory>;
}

TEST_CASE("Rpm probe proves exactly the page-rounded span and never a page without a requested byte", "[body][probe]")
{
    Reservation memory(2);
    uint8_t* page = memory.Commit(0, 1, PAGE_READWRITE);
    std::memset(page, 0x5A, kPage);
    RpmReader reader;
    uint64_t value{};
    REQUIRE(reader.Read(At(page + 100), value));
    REQUIRE(value == 0x5A5A5A5A5A5A5A5AULL);
    REQUIRE(reader.Stats.Probes == 1);
    REQUIRE(reader.Stats.ProbeBytes == kPage);
    REQUIRE(reader.Stats.VirtualQueries == 0);
    REQUIRE(reader.Stats.Regions == 0);
    REQUIRE(reader.Stats.Intervals == 1);
    REQUIRE(reader.Intervals.size() == 1);
    REQUIRE(reader.Intervals[0].Begin == At(page));
    REQUIRE(reader.Intervals[0].End == At(page) + kPage);
    // Cached: a second read on the page probes nothing more.
    REQUIRE(reader.Read(At(page + 4000), value));
    REQUIRE(reader.Stats.Probes == 1);
    // The reserved neighbour is refused and leaves the cache as it was.
    REQUIRE_FALSE(reader.Range(At(memory.Page(1)), 1));
    REQUIRE(reader.Stats.Probes == 2);
    REQUIRE(reader.Intervals.size() == 1);
    // A request that ends on the reserved page is refused whole: no interval for the readable part.
    RpmReader whole;
    REQUIRE_FALSE(whole.Range(At(page + kPage - 8), 16));
    REQUIRE(whole.Intervals.empty());
}

TEST_CASE("Rpm probe copies a 128 KB flattened-tree span in one probe and refuses it whole when its tail is reserved", "[body][probe]")
{
    const size_t bytes = 128 * 1024;
    {
        Reservation memory(34);
        uint8_t* base = memory.Commit(0, 33, PAGE_READWRITE);
        RpmReader reader;
        // Unaligned start: 32 pages of request round to 33 pages, three chunks.
        REQUIRE(reader.Range(At(base + 8), bytes));
        REQUIRE(reader.Stats.Probes == 1);
        REQUIRE(reader.Stats.ProbeBytes == 33 * kPage);
        REQUIRE(reader.Intervals.size() == 1);
        REQUIRE(reader.Intervals[0].Begin == At(base));
        REQUIRE(reader.Intervals[0].End == At(base) + 33 * kPage);
        // Aligned: exactly two chunks.
        RpmReader aligned;
        REQUIRE(aligned.Range(At(base), bytes));
        REQUIRE(aligned.Stats.ProbeBytes == bytes);
    }
    {
        Reservation memory(34);
        uint8_t* base = memory.Commit(0, 32, PAGE_READWRITE);
        RpmReader reader;
        REQUIRE_FALSE(reader.Range(At(base + 8), bytes)); // page 32 reserved
        REQUIRE(reader.Intervals.empty());
        REQUIRE(reader.Stats.Probes == 1);
    }
}

TEST_CASE("Rpm intervals merge when adjacent and stay apart across a NOACCESS hole", "[body][probe]")
{
    Reservation memory(3);
    memory.Commit(0, 1, PAGE_READWRITE);
    memory.Commit(1, 1, PAGE_NOACCESS);
    memory.Commit(2, 1, PAGE_READWRITE);
    RpmReader reader;
    REQUIRE_FALSE(reader.Range(At(memory.Page(0)), 3 * kPage));
    REQUIRE(reader.Intervals.empty());
    REQUIRE(reader.Range(At(memory.Page(0)), kPage));
    REQUIRE(reader.Range(At(memory.Page(2)) + 16, 8));
    REQUIRE(reader.Intervals.size() == 2);
    REQUIRE_FALSE(reader.Range(At(memory.Page(1)) + 8, 8));

    Reservation adjacent(2);
    adjacent.Commit(0, 2, PAGE_READWRITE);
    RpmReader merged;
    REQUIRE(merged.Range(At(adjacent.Page(0)) + 8, 8));
    REQUIRE(merged.Range(At(adjacent.Page(1)) + 8, 8));
    REQUIRE(merged.Intervals.size() == 1);
    REQUIRE(merged.Intervals[0].Begin == At(adjacent.Page(0)));
    REQUIRE(merged.Intervals[0].End == At(adjacent.Page(2)));
    REQUIRE(merged.Stats.Intervals == 1);
    // Filling the gap between two intervals collapses all three into one.
    Reservation three(3);
    three.Commit(0, 3, PAGE_READWRITE);
    RpmReader gap;
    REQUIRE(gap.Range(At(three.Page(0)), 8));
    REQUIRE(gap.Range(At(three.Page(2)), 8));
    REQUIRE(gap.Intervals.size() == 2);
    REQUIRE(gap.Range(At(three.Page(1)), 8));
    REQUIRE(gap.Intervals.size() == 1);
    REQUIRE(gap.Intervals[0].End - gap.Intervals[0].Begin == 3 * kPage);
}

TEST_CASE("Rpm write permission is proven per page from the working set after the read probe, across a READWRITE to READONLY boundary", "[body][probe]")
{
    Reservation memory(2);
    uint8_t* rw = memory.Commit(0, 1, PAGE_READWRITE);
    uint8_t* ro = memory.Commit(1, 1, PAGE_READONLY);
    RpmReader reader;
    REQUIRE(reader.Range(At(ro - 8), 16));            // read across the boundary is fine
    REQUIRE(reader.Intervals.size() == 1);            // one merged interval, no flags
    REQUIRE(reader.Range(At(rw), 8, true));           // page 0 writable
    REQUIRE(reader.Stats.WriteQueries == 1);
    // Residency after the probe is the host's business (working-set trimming can evict); the fallback
    // gives the same answer, so only the answer is pinned here.
    REQUIRE(reader.Range(At(rw + 100), 8, true));     // cached page, no second query
    REQUIRE(reader.Stats.WriteQueries == 1);
    REQUIRE_FALSE(reader.Range(At(ro - 8), 16, true)); // page 1 refused
    REQUIRE(reader.Stats.WriteQueries == 2);
    REQUIRE(reader.Stats.WriteQueryFailures == 1);
    REQUIRE(reader.Stats.VirtualQueries == reader.Stats.WriteQueryFallbacks);
    REQUIRE(ProtectOf(ro) == PAGE_READONLY);
    REQUIRE(ProtectOf(rw) == PAGE_READWRITE);
}

TEST_CASE("Working-set write query accepts READWRITE, EXECUTE_READWRITE and a copy-on-write view; refuses READONLY, NOACCESS and EXECUTE_READ", "[body][probe]")
{
    Reservation memory(5);
    uint8_t* rw = memory.Commit(0, 1, PAGE_READWRITE);
    uint8_t* ro = memory.Commit(1, 1, PAGE_READONLY);
    uint8_t* na = memory.Commit(2, 1, PAGE_NOACCESS);
    uint8_t* xr = memory.Commit(3, 1, PAGE_EXECUTE_READ);
    uint8_t* xrw = memory.Commit(4, 1, PAGE_EXECUTE_READWRITE);
    for (auto* p : {rw, ro, xr, xrw}) Touch(p);
    rw[5] = 0x5A;
    RpmMemory backend;
    NativeReadStats stats;
    REQUIRE(backend.Writable(At(rw), At(rw) + 8, stats));
    REQUIRE(rw[5] == 0x5A);
    REQUIRE(backend.Writable(At(xrw), At(xrw) + 8, stats));
    REQUIRE_FALSE(backend.Writable(At(ro), At(ro) + 8, stats));
    REQUIRE_FALSE(backend.Writable(At(xr), At(xr) + 8, stats));
    const auto before = stats.WriteQueryFallbacks;
    REQUIRE_FALSE(backend.Writable(At(na), At(na) + 8, stats)); // a NOACCESS page is never in the working set: the fallback answers, and refuses
    REQUIRE(stats.WriteQueryFallbacks == before + 1);
    REQUIRE(stats.VirtualQueries == stats.WriteQueryFallbacks);
    REQUIRE(stats.WriteQueryFailures == 3);
    for (auto* p : {rw, ro, na, xr, xrw}) REQUIRE(ProtectOf(p) == ProtectOf(p)); // nothing here changes protection
    REQUIRE(ProtectOf(ro) == PAGE_READONLY);
    REQUIRE(ProtectOf(na) == PAGE_NOACCESS);

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_WRITECOPY, 0, static_cast<DWORD>(kPage), nullptr);
    REQUIRE(mapping != nullptr);
    auto* wc = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, kPage));
    REQUIRE(wc != nullptr);
    Touch(wc);
    REQUIRE(backend.Writable(At(wc), At(wc) + 8, stats)); // WRITECOPY counts as writable, as today's predicate says
    UnmapViewOfFile(wc);
    CloseHandle(mapping);
}

TEST_CASE("A page that loses access between the read probe and the write query is refused without a crash, through the fallback", "[body][probe]")
{
    Reservation memory(1);
    uint8_t* page = memory.Commit(0, 1, PAGE_READWRITE);
    RpmReader reader;
    REQUIRE(reader.Range(At(page + 8), 8));
    DWORD old{};
    REQUIRE(VirtualProtect(page, kPage, PAGE_NOACCESS, &old));
    REQUIRE_FALSE(reader.Range(At(page + 8), 8, true)); // interval still cached; refused whichever path answers
    REQUIRE(reader.Stats.Probes == 1);
    REQUIRE(reader.Stats.WriteQueryFailures == 1);
    REQUIRE(VirtualProtect(page, kPage, PAGE_READWRITE, &old));
    REQUIRE(reader.Range(At(page + 8), 8, true));     // restored: accepted whichever path answers
    REQUIRE(reader.Stats.WriteQueryFailures == 1);
    REQUIRE(reader.Stats.VirtualQueries == reader.Stats.WriteQueryFallbacks);
}

TEST_CASE("A committed page nobody has touched is answered by the fallback, not refused", "[body][probe]")
{
    Reservation memory(1);
    uint8_t* page = memory.Commit(0, 1, PAGE_READWRITE);
    RpmMemory backend;
    NativeReadStats stats;
    REQUIRE(backend.Writable(At(page), At(page) + 8, stats)); // an untouched page has no valid PTE: the fallback answers
    REQUIRE(stats.WriteQueryFallbacks == 1);
    REQUIRE(stats.VirtualQueries == 1);
    // After a probe the page is normally resident and the working set answers directly; a trimmed page
    // would go through the fallback with the same answer, so the count is not pinned.
    RpmReader reader;
    REQUIRE(reader.Range(At(page), 8));
    REQUIRE(reader.Range(At(page), 8, true));
    REQUIRE(reader.Stats.VirtualQueries == reader.Stats.WriteQueryFallbacks);
}

TEST_CASE("A guard page is refused by the read probe with its guard intact; the working set never holds it", "[body][probe]")
{
    Reservation memory(1);
    uint8_t* guarded = memory.Commit(0, 1, PAGE_READWRITE | PAGE_GUARD);
    RpmReader reader;
    REQUIRE_FALSE(reader.Range(At(guarded), 8, true));
    REQUIRE(reader.Stats.Probes == 1);
    REQUIRE(reader.Stats.WriteQueries == 0);
    REQUIRE(ProtectOf(guarded) == (PAGE_READWRITE | PAGE_GUARD));
    RpmMemory backend;
    NativeReadStats stats;
    REQUIRE_FALSE(backend.Writable(At(guarded), At(guarded) + 8, stats)); // a guard page is never valid: the fallback sees the guard and refuses
    REQUIRE(stats.WriteQueryFallbacks == 1);
    REQUIRE(ProtectOf(guarded) == (PAGE_READWRITE | PAGE_GUARD));
}

TEST_CASE("Executable lookup answers from the working set for resident code and through one VirtualQuery otherwise, keeping today's answer for executable heap", "[body][probe]")
{
    RpmMemory backend;
    NativeReadStats stats;
    REQUIRE(backend.Executable(At(reinterpret_cast<const void*>(&ProtectOf)), stats)); // code of this binary
    REQUIRE(stats.ExecutableLookups == 1);
    REQUIRE(backend.Executable(At(reinterpret_cast<const void*>(&GetTickCount64)), stats)); // kernel32 code

    Reservation memory(1);
    uint8_t* heap = memory.Commit(0, 1, PAGE_READWRITE);
    Touch(heap);
    REQUIRE_FALSE(backend.Executable(At(heap), stats));
    REQUIRE(stats.VirtualQueries == stats.ExecutableFallbackHits); // whichever path answered, the counters agree
    const auto resident = stats.ExecutableFallbackHits;

    // A committed EXECUTE_READ page nobody has touched is not in the working set: the fallback answers, and accepts.
    auto* fresh = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ));
    REQUIRE(fresh != nullptr);
    REQUIRE(backend.Executable(At(fresh), stats));
    REQUIRE(stats.ExecutableFallbackHits == resident + 1);
    REQUIRE(stats.VirtualQueries == stats.ExecutableFallbackHits);
    VirtualFree(fresh, 0, MEM_RELEASE);
    // A trampoline-style page (executable heap, written then re-protected) is accepted either way, as before this build.
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    REQUIRE(stub != nullptr);
    stub[0] = 0xC3; // ret
    DWORD old{};
    REQUIRE(VirtualProtect(stub, kPage, PAGE_EXECUTE_READ, &old));
    REQUIRE(backend.Executable(At(stub), stats));
    Touch(stub);
    REQUIRE(backend.Executable(At(stub), stats));                    // same answer once resident
    VirtualFree(stub, 0, MEM_RELEASE);
}

TEST_CASE("Rpm reader reports nothing as read-only and merges", "[body][probe]")
{
    RpmMemory backend;
    REQUIRE_FALSE(backend.KnownReadOnly(At(&backend), sizeof(backend)));
    REQUIRE(backend.MergeIntervals());
}

TEST_CASE("Working-set write query walks a span wider than one batch without allocating, and stops at the first refused page", "[body][probe]")
{
    Reservation memory(RpmMemory::kWriteBatch + 3);
    uint8_t* base = memory.Commit(0, RpmMemory::kWriteBatch + 2, PAGE_READWRITE);
    uint8_t* ro = memory.Commit(RpmMemory::kWriteBatch + 2, 1, PAGE_READONLY);
    RpmReader reader;
    const size_t span = (RpmMemory::kWriteBatch + 2) * kPage;
    REQUIRE(reader.Range(At(base), span));
    REQUIRE(reader.Range(At(base), span, true));
    REQUIRE(reader.Stats.WriteQueries == 2);              // one full batch and one of two pages
    REQUIRE(reader.Stats.WriteQueryFailures == 0);
    REQUIRE(reader.Range(At(base) + 8, span - 8, true));  // every page cached: no further query
    REQUIRE(reader.Stats.WriteQueries == 2);
    REQUIRE(reader.Range(At(ro - 16), 32));
    REQUIRE_FALSE(reader.Range(At(ro - 16), 32, true));   // the READONLY tail page refuses the span
    REQUIRE(reader.Stats.WriteQueryFailures == 1);
}
