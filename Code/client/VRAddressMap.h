#pragma once

#if TP_SKYRIMVR

#include <cstddef>
#include <cstdint>

// One Anniversary Edition Address Library id mapped to a SkyrimVR 1.4.15 address.
//
// The client addresses the game by AE id, but the community VR address library is keyed on
// SSE 1.5.97 ids, a different namespace. Tools/vr_addresses/resolve.mjs bridges the two
// offline and generates the table, so a VR build carries its addresses with it and does not
// need the user to install an address library.
struct VRAddress
{
    uint32_t aeId;
    uint32_t vrOffset;  // RVA into SkyrimVR.exe. Zero means the id has no VR mapping yet.
    uint8_t confidence; // 5 hand-verified override, 4-2 community curated, 1 automated diff, 0 addrlib
    const char* name;   // the client-side symbol name, for diagnostics
};

namespace VRAddresses
{
const VRAddress* Data() noexcept;
size_t Count() noexcept;

// Returns the entry even when it has no VR mapping, so callers can name what is missing.
// Null only when the id is absent from the table entirely.
const VRAddress* Lookup(uint64_t aeId) noexcept;

// Asking for an id with no VR mapping is fatal on purpose. Handing back a null pointer would
// only move the crash somewhere undiagnosable.
[[noreturn]] void ReportMissing(uint64_t aeId) noexcept;

// Logs what this build actually resolved, so a run's log states which addresses were in play.
void LogSummary() noexcept;
} // namespace VRAddresses

#endif
