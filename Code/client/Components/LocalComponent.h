#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActionEvent.h>

struct LocalComponent
{
    LocalComponent(uint32_t aId) noexcept
        : Id(aId)
    {
    }

    uint32_t Id;
    ActionEvent CurrentAction;
    bool IsDead = false;

    // Down but not dead, tracked beside IsDead because an essential actor spends its whole knockdown with
    // IsDead false. See RequestDeathStateChange::IsBleedingOut.
    bool IsBleedingOut = false;
    bool IsWeaponDrawn = false;
};
