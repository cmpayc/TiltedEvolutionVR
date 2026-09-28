#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ItemNameReader
{
/**
 * A NUL-terminated printable name, copied a page-bounded chunk at a time through a primitive that copies a span
 * at once (ReadProbe::Copy). Same result and output bytes as walking the name a byte at a time with a
 * readability check per byte.
 *
 * A chunk never crosses a 4 KB page. Readability changes only at page boundaries, so a chunk fails exactly
 * when the byte walk would have failed on that page's first byte, and a NUL before an unreadable page still
 * succeeds. The chunk is scanned after the copy and nothing is written to output past the NUL or the first
 * rejected byte, so the output matches the byte walk on success and on failure.
 */
template<class ReadChunk>
bool CopyPaged(uintptr_t address, char* output, size_t capacity, ReadChunk&& readChunk)
{
    if (!address || !output || capacity < 2) return false;
    constexpr size_t kPage = 4096;
    char chunk[256];
    size_t copied = 0;
    while (copied < capacity - 1)
    {
        const size_t toPage = kPage - (address % kPage);
        const size_t want = std::min({capacity - 1 - copied, toPage, sizeof(chunk)});
        if (!readChunk(address, chunk, want)) return false;
        for (size_t i = 0; i < want; ++i)
        {
            const char value = chunk[i];
            output[copied + i] = value;
            if (value == '\0') return true;
            if (value < 0x20 || value > 0x7e) return false;
        }
        copied += want;
        if (address > std::numeric_limits<uintptr_t>::max() - want) return false;
        address += want;
    }
    return false;
}
}
