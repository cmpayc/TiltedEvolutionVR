#include <TiltedOnlinePCH.h>

#include <Forms/TESNPC.h>

#if TP_SKYRIMVR
#include <Games/References.h>
#include <Misc/BSString.h>
#include <World.h>
#endif

TP_THIS_FUNCTION(TSetLeveledNpc, TESNPC*, TESNPC, TESNPC*);
static TSetLeveledNpc* RealSetLeveledNpc = nullptr;

TESNPC* TP_MAKE_THISCALL(HookSetLeveledNpc, TESNPC, TESNPC* apSelectedNpc)
{
    spdlog::info("For TESNPC: {}, spawning: {}", apThis->fullName.value, apSelectedNpc->fullName.value);

    return TiltedPhoques::ThisCall(RealSetLeveledNpc, apThis, Cast<TESNPC>(TESForm::GetById(0x3B547)));
}

#if TP_SKYRIMVR
TP_THIS_FUNCTION(TGetActivateText, bool, TESNPC, TESObjectREFR* apRef, BSString* apResult, bool aUnk1);
static TGetActivateText* RealGetActivateText = nullptr;

/**
 * @brief Takes the activation prompt off another player.
 *
 * Nothing can be done with a remote player, so the rollover asking for a button press is noise,
 * and going through with it opens a dialogue that leads nowhere: HookProcessResponse already
 * drops the response.
 *
 * This is the game asking the base form whether a reference has anything to activate.
 * PlayerCharacter::PickCrosshairReference, PlayerCharacter::OnUpdateCrosshairText and the VR hand
 * pick all call it and skip the reference when the answer is false, so refusing here takes away
 * the prompt and stops the reference from becoming the activation target at all.
 *
 * The test is on the reference rather than on this base form, which every actor built from it
 * shares.
 *
 * The server's bBlockRemotePlayerActivation turns this off, back to the vanilla prompt.
 */
bool TP_MAKE_THISCALL(HookGetActivateText, TESNPC, TESObjectREFR* apRef, BSString* apResult, bool aUnk1)
{
    if (World::Get().GetServerSettings().BlockRemotePlayerActivation)
    {
        Actor* pActor = Cast<Actor>(apRef);
        ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;

        if (pExtension && pExtension->IsRemotePlayer())
            return false;
    }

    return TiltedPhoques::ThisCall(RealGetActivateText, apThis, apRef, apResult, aUnk1);
}
#endif

static TiltedPhoques::Initializer s_npcInitHooks(
    []()
    {
        POINTER_SKYRIMSE(TSetLeveledNpc, s_SetLeveledNpc, 14375);

        RealSetLeveledNpc = s_SetLeveledNpc.Get();

        // TP_HOOK(&RealSetLeveledNpc, HookSetLeveledNpc);

#if TP_SKYRIMVR
        POINTER_SKYRIMSE(TGetActivateText, s_getActivateText, 24716);

        RealGetActivateText = s_getActivateText.Get();

        TP_HOOK(&RealGetActivateText, HookGetActivateText);
#endif
    });
