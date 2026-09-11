#pragma once

#include <Misc/IMovementState.h>

struct ActorState : IMovementState
{
    virtual ~ActorState();

    uint32_t flags1;
    uint32_t flags2;

    bool IsWeaponDrawn() const noexcept { return (flags2 >> 5 & 7) >= 3; }

    bool IsWeaponFullyDrawn() const noexcept { return (flags2 >> 5 & 7) == 3; }

    bool IsBleedingOut() const noexcept { return (flags1 & 0x1E00000) == 0x1000000 || (flags1 & 0x1E00000) == 0xE00000; }

    /**
     * @brief Whether the actor is crouched. **Verified on SkyrimVR 1.4.15 only.**
     *
     * Measured by logging flags1 through a crouch and stand: the value moves between 0x00000041 standing and
     * 0x00000241 crouching, so the bit is 0x200 on that build.
     *
     * It was first written as 0x100, reasoned from the lifeState field IsBleedingOut reads at bits 21 to 24 and
     * counting the usual layout forward from there. That was one bit out and the flag never fired, which is
     * exactly why the same reasoning should not be trusted to carry the mask across to Skyrim SE. Nothing here
     * has been checked against an SE build, so callers there should re-measure it the same way rather than
     * assume this holds.
     */
    bool IsSneaking() const noexcept { return (flags1 & 0x200) != 0; }

    bool SetWeaponDrawn(bool aDraw) noexcept;
};
