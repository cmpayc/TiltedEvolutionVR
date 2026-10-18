#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace BodyTracking
{
struct SkinReadStats
{
    uint32_t Instances{}, Empty{}, Unlinked{}, NullSlots{}, MappedSlots{};
};
struct SkinReadDiagnostic
{
    const char* Operand{};
    bool Unreadable{};
    uintptr_t Skin{}, Data{}, Bones{}, Worlds{}, Partition{}, Bone{}, World{};
    uint32_t Count{};
    int Slot{-1};
};

// VR 1.4.15 UBM 0x140DC7DC0: input count is skinData+0x58;
// skin+0x3C is an OUTPUT, potentially zero before the first matrix update.
// Guard records every successful metadata/slot read for the caller's Stable().
// Map validates each non-null consumer against the exact native/flattened worlds.
template<class Guard, class Range, class Map>
bool ReadSkinConsumers(uintptr_t skin, Guard&& guard, Range&& range, Map&& map,
                       SkinReadStats& stats, SkinReadDiagnostic& diagnostic)
{
    diagnostic = {};
    diagnostic.Skin = skin;
    const auto fail = [&](const char* operand) { diagnostic.Operand = operand; return false; };
    const auto unreadable = [&](const char* operand) { diagnostic.Unreadable = true; return fail(operand); };
    if (!skin) return true;
    ++stats.Instances;
    if (skin > std::numeric_limits<uintptr_t>::max() - 0x30) return fail("skin-address");
    if (!guard(skin + 0x10, diagnostic.Data)) return unreadable("skin-data-read");
    if (!diagnostic.Data) return fail("skin-data");
    if (diagnostic.Data > std::numeric_limits<uintptr_t>::max() - 0x58) return fail("skin-data-address");
    if (!guard(diagnostic.Data + 0x58, diagnostic.Count)) return unreadable("skin-count-read");
    if (!guard(skin + 0x28, diagnostic.Bones)) return unreadable("skin-bones-read");
    if (!guard(skin + 0x30, diagnostic.Worlds)) return unreadable("skin-worlds-read");
    if (!guard(skin + 0x18, diagnostic.Partition)) return unreadable("skin-partition-read");
    if (diagnostic.Count > 1024) return fail("skin-count");
    if (!diagnostic.Count) { ++stats.Empty; return true; }
    // No world input to the verified consumer yet. Guarded pointers/count must
    // still compare equal later; population cannot silently survive Stable().
    if (!diagnostic.Worlds) { ++stats.Unlinked; return true; }
    // A populated worlds array IS consumed regardless of the bones pointer.
    // A missing ARRAY must not be confused with a valid null bone SLOT.
    if (!diagnostic.Bones) return fail("skin-bones");
    const size_t bytes = diagnostic.Count * sizeof(uintptr_t);
    if (!range(diagnostic.Bones, bytes)) return unreadable("skin-bones-range");
    if (!range(diagnostic.Worlds, bytes)) return unreadable("skin-worlds-range");
    for (uint32_t slot = 0; slot < diagnostic.Count; ++slot)
    {
        diagnostic.Slot = static_cast<int>(slot);
        diagnostic.Bone = diagnostic.World = 0;
        if (!guard(diagnostic.Bones + slot * sizeof(uintptr_t), diagnostic.Bone)) return unreadable("skin-bone-slot");
        if (!guard(diagnostic.Worlds + slot * sizeof(uintptr_t), diagnostic.World)) return unreadable("skin-world-slot");
        if (!diagnostic.World) { ++stats.NullSlots; continue; }
        if (const char* reason = map(diagnostic.Bone, diagnostic.World)) return fail(reason);
        ++stats.MappedSlots;
    }
    return true;
}
}
