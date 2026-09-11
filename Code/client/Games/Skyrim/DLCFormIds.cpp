#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/DLCFormIds.h>
#include <Games/TES.h>

namespace
{
/**
 * @brief Looks up a plugin's load order index and applies it to a record id local to that plugin.
 *
 * The load order cannot change while the process is running, so a successful lookup is cached.
 * A failure is never cached, because the mods list is empty until the game has loaded its data
 * files and callers can run before that.
 *
 * The IsLoaded() check matters: the mods list also holds files that are present but not loaded, and
 * those carry standardId 0xFF, so using one would build an id in the 0xFF dynamic form range and
 * alias a runtime object instead of failing.
 */
uint32_t ResolveForm(const char* acpFilename, uint32_t aLocalId, Mod*& apCached) noexcept
{
    if (!apCached)
    {
        auto* const cpModManager = ModManager::Get();

        if (!cpModManager)
            return 0;

        Mod* const pMod = cpModManager->GetByName(acpFilename);

        if (!pMod || !pMod->IsLoaded())
            return 0;

        apCached = pMod;
    }

    return apCached->GetFormId(aLocalId);
}

Mod* s_pDawnguard = nullptr;
Mod* s_pDragonborn = nullptr;
} // namespace

uint32_t DawnguardForm(uint32_t aLocalId) noexcept
{
    return ResolveForm("Dawnguard.esm", aLocalId, s_pDawnguard);
}

uint32_t DragonbornForm(uint32_t aLocalId) noexcept
{
    return ResolveForm("Dragonborn.esm", aLocalId, s_pDragonborn);
}
