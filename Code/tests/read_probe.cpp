// Real-memory fixtures for the ReadProcessMemory probe: these run against the operating system, not a model
// of it, because the whole question is what the primitive does on a guard page, a reserved page and a span
// that crosses two regions.
#include <catch2/catch.hpp>
#include "../client/Services/ReadProbe.h"
#include "../client/Services/ItemNameReader.h"
#include <array>
#include <cstring>
#include <string>

namespace
{
constexpr size_t kPage = 4096;

struct Reservation
{
    uint8_t* Base{};
    size_t Bytes{};
    explicit Reservation(size_t bytes) : Bytes(bytes)
    {
        Base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
        REQUIRE(Base != nullptr);
    }
    ~Reservation() { if (Base) VirtualFree(Base, 0, MEM_RELEASE); }
    uint8_t* Commit(size_t offset, size_t bytes, DWORD protect)
    {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(Base + offset, bytes, MEM_COMMIT, protect));
        REQUIRE(p == Base + offset);
        return p;
    }
};

DWORD ProtectOf(const void* p)
{
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(p, &info, sizeof(info)) != 0);
    return info.Protect;
}

size_t RegionSizeOf(const void* p)
{
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(p, &info, sizeof(info)) != 0);
    return info.RegionSize;
}
}

TEST_CASE("Read probe copies committed memory and fails closed on null, reserved and decommitted pages", "[read-probe]")
{
    Reservation memory(4 * kPage);
    uint8_t* page = memory.Commit(0, kPage, PAGE_READWRITE);
    std::memset(page, 0x5A, kPage);

    std::array<uint8_t, 272> out{};
    REQUIRE(ReadProbe::Copy(page, out.data(), out.size()));
    REQUIRE(out[0] == 0x5A);
    REQUIRE(out[271] == 0x5A);

    REQUIRE_FALSE(ReadProbe::Copy(nullptr, out.data(), 1));
    REQUIRE_FALSE(ReadProbe::Copy(page, nullptr, 1));
    REQUIRE_FALSE(ReadProbe::Copy(page, out.data(), 0));

    // Reserved but never committed: the byte walk refused this and so does the probe.
    REQUIRE_FALSE(ReadProbe::Copy(memory.Base + kPage, out.data(), 1));

    // A span that runs off the committed page into the reserved one fails as a whole, and reports no partial
    // success: that is the property the page-chunked name reader relies on. The output must be untouched, so
    // a primitive that copied the readable half and then failed would be caught here.
    out.fill(0xEE);
    REQUIRE_FALSE(ReadProbe::Copy(page + kPage - 8, out.data(), 16));
    for (size_t i = 0; i < 16; ++i)
        REQUIRE(out[i] == 0xEE);

    // Decommitted after use: fails closed again.
    REQUIRE(VirtualFree(page, kPage, MEM_DECOMMIT));
    REQUIRE_FALSE(ReadProbe::Copy(page, out.data(), 1));
}

TEST_CASE("Read probe refuses NOACCESS and GUARD pages and leaves the guard armed", "[read-probe]")
{
    Reservation memory(2 * kPage);
    uint8_t* noAccess = memory.Commit(0, kPage, PAGE_NOACCESS);
    uint8_t* guarded = memory.Commit(kPage, kPage, PAGE_READWRITE | PAGE_GUARD);

    std::array<uint8_t, 16> out{};
    REQUIRE_FALSE(ReadProbe::Copy(noAccess, out.data(), out.size()));

    REQUIRE(ProtectOf(guarded) == (PAGE_READWRITE | PAGE_GUARD));
    REQUIRE_FALSE(ReadProbe::Copy(guarded, out.data(), out.size()));
    // Measured 2026-09-15 and asserted here so a Windows change would show up in the test run, not in the game:
    // the failed probe must not consume the guard, or the next legitimate touch (a thread's stack growth) would
    // fault instead of extending.
    REQUIRE(ProtectOf(guarded) == (PAGE_READWRITE | PAGE_GUARD));
}

TEST_CASE("Read probe accepts a span across two adjacent readable regions, which VirtualQuery refused", "[read-probe]")
{
    // Two pages of one reservation committed with different protections are two regions to VirtualQuery. The
    // old IsReadable refused any span that crossed the first region's end; the probe accepts it because every
    // byte is readable. This is the one documented widening of the readability contract.
    Reservation memory(2 * kPage);
    uint8_t* first = memory.Commit(0, kPage, PAGE_READWRITE);
    uint8_t* second = memory.Commit(kPage, kPage, PAGE_READONLY);
    std::memset(first, 1, kPage);

    REQUIRE(RegionSizeOf(first) == kPage);
    REQUIRE(RegionSizeOf(second) == kPage);

    std::array<uint8_t, 32> out{};
    REQUIRE(ReadProbe::Copy(first + kPage - 16, out.data(), 32));
    REQUIRE(out[0] == 1);
    REQUIRE(out[16] == 0);

    // The old rule, reproduced exactly, on the same span.
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(first + kPage - 16, &info, sizeof(info)) != 0);
    const auto cRegionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    REQUIRE_FALSE(reinterpret_cast<uintptr_t>(first + kPage - 16) + 32 <= cRegionEnd);
}

TEST_CASE("Paged item-name reader on real pages: NUL before an unreadable page succeeds, none fails", "[read-probe][item-reader]")
{
    Reservation memory(2 * kPage);
    char* page = reinterpret_cast<char*>(memory.Commit(0, kPage, PAGE_READWRITE));
    std::memset(page, 'x', kPage);

    auto readChunk = [](uintptr_t address, char* chunk, size_t bytes) { return ReadProbe::Copy(reinterpret_cast<const void*>(address), chunk, bytes); };

    // Name ends 3 bytes before the reserved page: success, exact text, nothing written past the NUL.
    const std::string name = "Weapon  (000139B7)";
    char* start = page + kPage - name.size() - 1;
    std::memcpy(start, name.c_str(), name.size() + 1);
    std::array<char, 128> out{}; out.fill('?');
    REQUIRE(ItemNameReader::CopyPaged(reinterpret_cast<uintptr_t>(start), out.data(), out.size(), readChunk));
    REQUIRE(std::string(out.data()) == name);
    REQUIRE(out[name.size() + 1] == '?');

    // Same bytes with the NUL removed: the name runs into the reserved page and the reader fails, with the
    // readable bytes copied first, exactly as the byte walk left them.
    start[name.size()] = 'x';
    out.fill('?');
    REQUIRE_FALSE(ItemNameReader::CopyPaged(reinterpret_cast<uintptr_t>(start), out.data(), out.size(), readChunk));
    REQUIRE(std::string(out.data(), name.size() + 1) == name + "x");
    REQUIRE(out[name.size() + 1] == '?');

    // 127 readable bytes with no NUL: capacity exhausted, false, as before.
    std::memset(page, 'y', kPage);
    out.fill('?');
    REQUIRE_FALSE(ItemNameReader::CopyPaged(reinterpret_cast<uintptr_t>(page), out.data(), out.size(), readChunk));
    REQUIRE(out[126] == 'y');
    REQUIRE(out[127] == '?');
}
