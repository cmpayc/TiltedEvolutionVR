#include <catch2/catch.hpp>
#include "../client/Services/ItemNameReader.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct Memory
{
    static constexpr uintptr_t Base = 0x1000;
    std::string Bytes;
    size_t Boundary = 128;
    bool SecondReadable = true;
    size_t Queries{}, Reads{};
    bool Query(uintptr_t p, uintptr_t& end)
    {
        ++Queries;
        if (p < Base || p >= Base + Bytes.size()) return false;
        const bool second = p >= Base + Boundary;
        if (second && !SecondReadable) return false;
        end = Base + (second ? Bytes.size() : std::min(Boundary, Bytes.size()));
        return end > p;
    }
    char Read(uintptr_t p) { ++Reads; return Bytes.at(p - Base); }
};
bool OldCopy(Memory& m, char* out, size_t capacity)
{
    for (size_t i = 0; i + 1 < capacity; ++i)
    {
        uintptr_t end{};
        if (!m.Query(Memory::Base + i, end)) return false;
        out[i] = m.Read(Memory::Base + i);
        if (out[i] == 0) return true;
        if (out[i] < 0x20 || out[i] > 0x7e) return false;
    }
    return false;
}
// A page model for the chunked reader: readability changes only at one page boundary, which is where real
// pages change it. Base is chosen so that boundary is a 4 KB boundary, exactly the case the reader splits on.
struct PagedMemory
{
    uintptr_t Base;
    std::string Bytes;
    size_t Boundary;
    bool SecondReadable;
    size_t Chunks{};
    // Padded to two full pages: a real page is readable in full or not at all, so a chunk request that runs past
    // the interesting bytes must still succeed, as it does in memory. The byte walk never reaches the padding.
    PagedMemory(std::string bytes, size_t boundary, bool secondReadable)
        : Base(0x10000 - boundary), Bytes(std::move(bytes)), Boundary(boundary), SecondReadable(secondReadable)
    {
        Bytes.resize(8192, 'x');
    }
    bool ReadChunk(uintptr_t p, char* chunk, size_t n)
    {
        ++Chunks;
        if (p < Base || p + n > Base + Bytes.size()) return false;
        const bool crossesIntoSecond = p + n > Base + Boundary;
        const bool startsInSecond = p >= Base + Boundary;
        if ((startsInSecond || crossesIntoSecond) && !SecondReadable) return false;
        std::memcpy(chunk, Bytes.data() + (p - Base), n);
        return true;
    }
    bool OldCopy(char* out, size_t capacity)
    {
        for (size_t i = 0; i + 1 < capacity; ++i)
        {
            const bool second = i >= Boundary;
            if (i >= Bytes.size() || (second && !SecondReadable)) return false;
            out[i] = Bytes[i];
            if (out[i] == 0) return true;
            if (out[i] < 0x20 || out[i] > 0x7e) return false;
        }
        return false;
    }
};
}

TEST_CASE("Paged item name reader matches the byte walk for every terminator and page split", "[item-reader]")
{
    for (size_t nul = 0; nul <= 128; ++nul)
        for (size_t split = 1; split <= 128; ++split)
            for (bool readable : {false, true})
            {
                PagedMemory memory{std::string(129, 'x'), split, readable};
                memory.Bytes[nul] = 0;
                std::array<char, 128> a{}, b{}; a.fill('?'); b.fill('?');
                const bool expected = memory.OldCopy(a.data(), a.size());
                const bool actual = ItemNameReader::CopyPaged(memory.Base, b.data(), b.size(),
                    [&](uintptr_t p, char* chunk, size_t n) { return memory.ReadChunk(p, chunk, n); });
                REQUIRE(actual == expected);
                REQUIRE(a == b);
                REQUIRE(memory.Chunks <= 2);
            }

    for (int byte = 1; byte <= 255; ++byte)
    {
        PagedMemory memory{std::string("Weapon  (000139B7)") + char(byte) + '\0', 8, true};
        std::array<char, 128> a{}, b{}; a.fill('?'); b.fill('?');
        const bool expected = memory.OldCopy(a.data(), a.size());
        REQUIRE(ItemNameReader::CopyPaged(memory.Base, b.data(), b.size(),
            [&](uintptr_t p, char* chunk, size_t n) { return memory.ReadChunk(p, chunk, n); }) == expected);
        REQUIRE(a == b);
    }

    // Capacity above the 256-byte chunk bound: a page split at 200, then a full 256-byte chunk that contains the
    // NUL at 300, so the reader must loop twice inside one page and still match the byte walk exactly.
    {
        PagedMemory memory{std::string(400, 'x'), 200, true};
        memory.Bytes[300] = 0;
        std::array<char, 512> a{}, b{}; a.fill('?'); b.fill('?');
        const bool expected = memory.OldCopy(a.data(), a.size());
        size_t largestChunk = 0;
        REQUIRE(ItemNameReader::CopyPaged(memory.Base, b.data(), b.size(),
            [&](uintptr_t p, char* chunk, size_t n) { largestChunk = std::max(largestChunk, n); return memory.ReadChunk(p, chunk, n); }) == expected);
        REQUIRE(expected);
        REQUIRE(largestChunk == 256);
        REQUIRE(a == b);
        REQUIRE(memory.Chunks == 2);
        REQUIRE(b[300] == '\0');
        REQUIRE(b[301] == '?');
    }

    char output[2]{};
    size_t chunks{};
    auto never = [&](uintptr_t, char*, size_t) { ++chunks; return true; };
    REQUIRE_FALSE(ItemNameReader::CopyPaged(0, output, 2, never));
    REQUIRE_FALSE(ItemNameReader::CopyPaged(0x1000, nullptr, 2, never));
    REQUIRE_FALSE(ItemNameReader::CopyPaged(0x1000, output, 1, never));
    REQUIRE(chunks == 0);
}
