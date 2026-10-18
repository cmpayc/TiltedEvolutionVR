// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.

#include "SELowZone.h"

#if TP_SKYRIMVR
#include <Windows.h>
#include <cstdint>

namespace script_extender
{
namespace
{
// The Script Extender takes (its own dll base - kSkseReach) as the lower bound of the search for its
// codegen buffer, then walks down from that base for a free 64k block. Our exe is linked
// /DYNAMICBASE:NO and its game segment runs past 0x180000000, which is where the dll wants to sit, so
// the dll always gets relocated. Land it under kSkseReach and that subtraction underflows: the bound
// check then passes on the first iteration, so it gives up after a single probe, logs "couldn't
// allocate trampoline, no free space before image" and skips the rest of its init. Nothing loads
// after that, and preloaded plugins go on to fault on the interfaces the skipped init owed them.
constexpr uintptr_t kSkseReach = 0x78000000;

// Below this nothing can host the dll image anyway, and the loader keeps its own thunks down there.
constexpr uintptr_t kLowZoneStart = 0x100000;

constexpr uintptr_t kGranularity = 0x10000;

// What the trampoline needs, and small enough that the dll image can never fit in one.
constexpr uintptr_t kGapSize = 0x10000;

// One gap per stride. Reserving solid would push the dll above kSkseReach and then leave the search
// with nothing to find, which is the failure this spacing exists to avoid.
constexpr uintptr_t kStride = 0x400000;

// The range holds 0x78000000 / kStride strides, plus slack for a fragmented address space.
constexpr size_t kMaxBlocks = 4096;

void* s_blocks[kMaxBlocks]{};
size_t s_blockCount = 0;
} // namespace

void ReserveLowZone()
{
    MEMORY_BASIC_INFORMATION info{};

    uintptr_t address = kLowZoneStart;
    while (address < kSkseReach && s_blockCount < kMaxBlocks)
    {
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)))
            return;

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= address)
            return;

        if (info.State == MEM_FREE)
        {
            // The region can start below where we are allowed to reserve, so clamp both ends.
            uintptr_t start = (address + kGranularity - 1) & ~(kGranularity - 1);
            const uintptr_t end = regionEnd < kSkseReach ? regionEnd : kSkseReach;

            // Chop it up so every run left free is too small to host the image, but still wide
            // enough to hand the trampoline search a block. The gap sits at the bottom of each
            // chunk, never the top: a gap at the top of the range would join the free space above
            // it into one run, and the image would land on the gap and straddle kSkseReach, which
            // puts its base back under the reach and underflows the bound after all. The last
            // chunk therefore has to stay reserved right up to kSkseReach.
            while (start < end && s_blockCount < kMaxBlocks)
            {
                const uintptr_t chunkEnd = (start + kStride < end) ? start + kStride : end;
                const uintptr_t gap = (chunkEnd - start > kGapSize) ? kGapSize : 0;

                if (void* pBlock = VirtualAlloc(reinterpret_cast<void*>(start + gap), chunkEnd - start - gap, MEM_RESERVE, PAGE_NOACCESS))
                    s_blocks[s_blockCount++] = pBlock;

                start = chunkEnd;
            }
        }

        address = regionEnd;
    }
}

void ReleaseLowZone()
{
    for (size_t i = 0; i < s_blockCount; ++i)
        VirtualFree(s_blocks[i], 0, MEM_RELEASE);

    s_blockCount = 0;
}
} // namespace script_extender
#endif
