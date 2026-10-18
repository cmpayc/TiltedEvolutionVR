#pragma once

#include <Forms/MagicItem.h>

#include <Components/BGSEquipType.h>
#include <Components/BGSMenuDisplayObject.h>
#include <Components/TESDescription.h>

struct SpellItem : MagicItem
{
    enum class SpellFlag
    {
        kNone = 0,
        kCostOverride = 1 << 0,
        kFoodItem = 1 << 1,
        kExtendDuration = 1 << 3,
        kPCStartSpell = 1 << 17,
        kInstantCast = 1 << 18,
        kIgnoreLOSCheck = 1 << 19,
        kIgnoreResistance = 1 << 20,
        kNoAbsorb = 1 << 21,
        kNoDualCastMods = 1 << 23
    };

    BGSEquipType equipType;
    BGSMenuDisplayObject menuDisplayObject;
    TESDescription description;
    // SpellData, in the game's own field order. Confirmed at runtime 2026-08-29: Flames logged
    // costOverride 0xE (its magicka cost) with eSpellType 0 (SPELL), while the Nord Battle Cry
    // power logged costOverride 0 with eSpellType 2 (POWER).
    int32_t iCostOverride;
    int32_t iFlags;
    MagicSystem::SpellType eSpellType;
    float castTime;
    MagicSystem::CastingType eCastingType;
    MagicSystem::Delivery eDelivery;
    // more stuff
};
