#pragma once

// Form ids of DLC records carry the plugin's load order index in their top byte, and that index is
// not the same on every install. SkyrimSE 1.6.1170 loads the DLC as 02 Dawnguard, 03 HearthFires,
// 04 Dragonborn. A vanilla SkyrimVR 1.4.15 measured here loads them as 02 HearthFires,
// 03 Dragonborn, 04 Dawnguard, and nothing on disk controls that: listing the masters in
// plugins.txt and setting sTestFile1..10 in SkyrimVR.ini were both tried and neither changed it.
//
// Hardcoding either order resolves every id into the wrong plugin on the other install. That fails
// silently rather than loudly, because the DLC are independent of each other, so there is no
// conflict to warn about, just a lookup into a plugin that never defined the record. It cost the
// SE build nothing and the VR build all nine of the ids below.
//
// So the index is read from the running game instead of assumed.
//
// aLocalId is the record's id inside its plugin, i.e. the form id with its top byte cleared.
// Returns 0 when the plugin is not loaded or the game has not loaded its data files yet. 0 is not a
// valid form id, so a caller that cannot resolve fails the same way it would for a missing record.

uint32_t DawnguardForm(uint32_t aLocalId) noexcept;
uint32_t DragonbornForm(uint32_t aLocalId) noexcept;
