#pragma once

#include <Misc/BSScript.h>

struct SkyrimVM
{
    virtual ~SkyrimVM();

    static SkyrimVM* Get();

    // The SE offsets moved by 0x10 in the 1.6.1179 update. SkyrimVR predates it and keeps the
    // layout this fork shipped before that update, so both members need their own values here.
#if TP_SKYRIMVR
    uint8_t pad8[0x200 - 0x8];
    BSScript::IVirtualMachine* virtualMachine;
    uint8_t pad208[0x680 - 0x208];
    uint8_t inactive;
#else
    uint8_t pad8[0x210 - 0x8];
    BSScript::IVirtualMachine* virtualMachine;
    uint8_t pad218[0x690 - 0x218];
    int32_t inactive;
#endif
};

#if TP_SKYRIMVR
static_assert(offsetof(SkyrimVM, virtualMachine) == 0x200);
static_assert(offsetof(SkyrimVM, inactive) == 0x680);
#else
static_assert(offsetof(SkyrimVM, virtualMachine) == 0x210);
static_assert(offsetof(SkyrimVM, inactive) == 0x690);
#endif

using GameVM = SkyrimVM;
