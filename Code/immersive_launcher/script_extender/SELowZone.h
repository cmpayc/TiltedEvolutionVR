// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#if TP_SKYRIMVR
namespace script_extender
{
// Blocks the address space the Script Extender's dll must not be relocated into, leaving gaps its
// trampoline allocator can still use. Call immediately before loading the dll, not earlier: held any
// longer than the load, it pushes the heap into the very range that allocator searches.
void ReserveLowZone();

// Hands that address space back. Safe to call more than once.
void ReleaseLowZone();
} // namespace script_extender
#endif
