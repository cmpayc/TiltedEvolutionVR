#pragma once

// Form ids of DLC records carry the plugin's load order index in their top byte, and SkyrimVR does
// not order the official masters the way SkyrimSE does. The client had these ids hardcoded with the
// SE indices, so on VR every one of them resolved into the wrong plugin.
//
// The order is fixed by the exe, not derived from the Data directory, which is what makes hardcoding
// it sound in the first place. Both installs here have byte-identical timestamps on all of their
// .esm files, so a timestamp tie-break would have produced the same order for both; instead each
// game's save records its own, and they differ:
//
//   index  SkyrimSE 1.6.1170     SkyrimVR 1.4.15
//   00     Skyrim.esm            Skyrim.esm
//   01     Update.esm            Update.esm
//   02     Dawnguard.esm         HearthFires.esm
//   03     HearthFires.esm       Dragonborn.esm
//   04     Dragonborn.esm        Dawnguard.esm
//   05     Creation Club, if any SkyrimVR.esm
//
// Read out of the plugin array of a save from each game. Creation Club content lands at 06 and above
// on SE and does not disturb the DLC, and no id here refers to SkyrimVR.esm.
//
// This assumes a vanilla install, which is the same assumption the hardcoded local ids already make.
// A mod that inserts a master ahead of the DLC would break these, and the robust alternative is
// ModManager::GetByName() plus Mod::GetFormId(), which resolves the index at runtime.

#if TP_SKYRIMVR
constexpr uint32_t kHearthFiresIndex = 0x02;
constexpr uint32_t kDragonbornIndex = 0x03;
constexpr uint32_t kDawnguardIndex = 0x04;
#else
constexpr uint32_t kDawnguardIndex = 0x02;
constexpr uint32_t kHearthFiresIndex = 0x03;
constexpr uint32_t kDragonbornIndex = 0x04;
#endif

// aLocalId is the record's id inside its plugin, i.e. the SE form id with its top byte cleared.
constexpr uint32_t DawnguardForm(uint32_t aLocalId) noexcept { return (kDawnguardIndex << 24) | aLocalId; }
constexpr uint32_t HearthFiresForm(uint32_t aLocalId) noexcept { return (kHearthFiresIndex << 24) | aLocalId; }
constexpr uint32_t DragonbornForm(uint32_t aLocalId) noexcept { return (kDragonbornIndex << 24) | aLocalId; }

#if !TP_SKYRIMVR
// Every id these helpers replaced, so the SE build is provably the same numbers it always was.
static_assert(DawnguardForm(0x00283A) == 0x200283A);  // vampire lord race
static_assert(DawnguardForm(0x011A84) == 0x2011A84);  // vampire lord armor
static_assert(DawnguardForm(0x0071D0) == 0x20071D0);  // vampire transformation quest
static_assert(DawnguardForm(0x008431) == 0x2008431);  // revered dragon
static_assert(DawnguardForm(0x00C5F5) == 0x200C5F5);  // legendary dragon, fire
static_assert(DawnguardForm(0x00C5FD) == 0x200C5FD);  // legendary dragon, frost
static_assert(DragonbornForm(0x018279) == 0x4018279); // Solstheim crime faction
static_assert(DragonbornForm(0x036134) == 0x4036134); // serpentine dragon, fire
static_assert(DragonbornForm(0x036133) == 0x4036133); // serpentine dragon, frost
#endif
