#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace ReadProbe
{
/**
 * One syscall whose cost does not depend on the size of the region the pointer sits in.
 *
 * VirtualQuery walks page tables from the queried address to the end of the identical-attribute run, so a
 * probe into one of the game's resident pools costs up to milliseconds (measured 2026-09-15: 1 µs on a
 * 64 KB region, 10.6 ms at the base of a resident 1 GB region). ReadProcessMemory on the own process copies
 * the bytes in about 1 µs regardless, and fails closed on null, reserved, decommitted, NOACCESS and GUARD
 * pages. In the measured fixture a GUARD page kept its guard bit after the failed read; that is an
 * observation, not an API guarantee.
 *
 * Contract: true means every one of aBytes was readable at the instant of the call and has been copied. It
 * accepts a span that crosses two adjacent readable regions, which the VirtualQuery-based check refused. It
 * says nothing about object identity, lifetime, or write permission; the check-then-use exposure is the same
 * as before.
 */
inline bool Copy(const void* apSource, void* apOut, const size_t aBytes) noexcept
{
    if (!apSource || !apOut || aBytes == 0)
        return false;

    SIZE_T got = 0;

    if (!ReadProcessMemory(GetCurrentProcess(), apSource, apOut, aBytes, &got))
        return false;

    return got == aBytes;
}
}
