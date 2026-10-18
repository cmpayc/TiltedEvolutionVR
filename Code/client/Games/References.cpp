#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <RTTI.h>

#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>
#include <Forms/TESPackage.h>
#include <Forms/TESWeather.h>
#include <SaveLoad.h>
#include <ExtraData/ExtraDataList.h>

#include <Misc/GameVM.h>

#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Games/Misc/Lock.h>
#include <AI/AIProcess.h>
#include <Magic/MagicCaster.h>
#include <Sky/Sky.h>

#include <Events/InitPackageEvent.h>

#include <TiltedCore/Serialization.hpp>

#include <Services/PapyrusService.h>
#include <Services/DebugService.h>
#include <World.h>

#include <AI/AITimer.h>
#include <Combat/CombatTargetSelector.h>

namespace Settings
{
int32_t* GetDifficulty() noexcept
{
    POINTER_SKYRIMSE(int32_t, s_difficulty, 381472);
    return s_difficulty.Get();
}

float* GetGreetDistance() noexcept
{
    POINTER_SKYRIMSE(float, s_greetDistance, 370892);
    return s_greetDistance.Get();
}

} // namespace Settings

namespace GameplayFormulas
{

float CalculateRealDamage(Actor* apHittee, float aDamage, bool aKillMove) noexcept
{
    using TGetDifficultyMultiplier = float(int32_t, int32_t, bool);
    POINTER_SKYRIMSE(TGetDifficultyMultiplier, s_getDifficultyMultiplier, 26503);

    bool isPlayer = apHittee == PlayerCharacter::Get();

    float multiplier = s_getDifficultyMultiplier(PlayerCharacter::Get()->difficulty, ActorValueInfo::kHealth, isPlayer);

    float realDamage = aDamage;

    // TODO(cosideci): this seems problematic? It may not register the kill for others?
    // Disabled for now, cause this check seems to have totally broken everything, let's see what happens.
    // if (!aKillMove || multiplier < 1.0)
    realDamage = aDamage * multiplier;

    return realDamage;
}

} // namespace GameplayFormulas

void FadeOutGame(bool aFadingOut, bool aBlackFade, float aFadeDuration, bool aRemainVisible, float aSecondsToFade) noexcept
{
#if TP_SKYRIMVR
    // SkyrimVR's version of this function takes two more arguments than 1.6.1170's, reads the first
    // two as dwords rather than bytes, and takes the FIRST ARGUMENT WITH THE OPPOSITE POLARITY.
    // Getting it wrong cost two bugs: the missing seventh argument crashed on the death fade, and
    // then the inverted first argument faded the screen to black on respawn and never brought it
    // back, because every call was doing the opposite of what it asked for.
    //
    // Both are the same function: id 52847 is SE 0x1409768E0 (0xBC bytes) and VR 0x140903080 (0x123).
    // The prologues agree instruction for instruction (the `mov qword [rsp+0x20],-2` EH sentinel,
    // `movaps [rsp+0x30],xmm6`, `movzx ebp,r9b` for argument 4, `movaps xmm6,xmm2` for argument 3,
    // then the same `_tls_index` / `gs:[0x58]` pair), and both fill the same fade object:
    //
    //   [obj+0x20] = argument 3, the fade duration      [obj+0x24] = from argument 1
    //   [obj+0x1C] = argument 5, the seconds before it   [obj+0x25] = from argument 2
    //   [obj+0x26] = argument 4, remain visible          [obj+0x10] = argument 7, VR only
    //
    // The polarity, which is the part that matters:
    //
    //   SE 0x14097694E:  xor dil,1  /  mov [rax+0x24],dil     ->  [0x24] = !argument1
    //   VR 0x14090310C:  cmp r15d,1 /  sete cl / mov [rdi+0x24],cl  ->  [0x24] = (argument1 == 1)
    //
    // so the same argument writes opposite values. Which way the field reads is settled by SE's
    // sibling at 0x1409769B0: it loads the same global, calls the same object getter 0x14061BBA0,
    // and hardcodes `mov word [rax+0x24],0x0100` with remain-visible set, which is a fade to black.
    // So **[obj+0x24] == 0 means fading out**, and on VR that means passing 0 to fade out and 1 to
    // fade in. Argument 2 needs no inversion: SE writes it straight through and VR's `(x == 1)`
    // agrees with it for both 0 and 1.
    //
    // That same sibling is also where VR's extra argument comes from. It takes the pointer in rdx and
    // ends in exactly VR's tail, `lock inc dword [rdi+8]`, store into [obj+0x10], DecRef the old. SE
    // keeps that as a separate entry point; VR merged it in as argument 7.
    //
    // The rest of the deltas: SE's frame puts argument 5 at [rsp+0x80] and reads nothing above it,
    // VR's puts it at [rsp+0x90] and reads [rsp+0xA0]. All 13 SE call sites set only [rsp+0x20]; all
    // 34 VR call sites also set [rsp+0x28] and [rsp+0x30]. int32_t rather than bool for the first two
    // because VR compares the whole dword against 1, and a bool whose upper bits the compiler left
    // stale would miss. Argument 6 is a byte this function never reads, and every VR call site
    // passes 0.
    //
    // nullptr for argument 7 is the game's own common case, not a bypass: the callee null-checks it
    // (`test rax,rax` / `jz` over the IncRef, then stores it either way) and most VR call sites pass
    // a literal `mov qword [rsp+0x30],0`. The object is NiRefObject-derived, refcount at +0x8.
    using TFadeOutGame = void(int32_t, int32_t, float, bool, float, bool, void*);
    POINTER_SKYRIMSE(TFadeOutGame, fadeOutGame, 52847);
    fadeOutGame.Get()(aFadingOut ? 0 : 1, aBlackFade ? 1 : 0, aFadeDuration, aRemainVisible, aSecondsToFade, false, nullptr);
#else
    using TFadeOutGame = void(bool, bool, float, bool, float);
    POINTER_SKYRIMSE(TFadeOutGame, fadeOutGame, 52847);
    fadeOutGame.Get()(aFadingOut, aBlackFade, aFadeDuration, aRemainVisible, aSecondsToFade);
#endif
}

// Disable AI sync for now, experiment didn't work, code might be useful later on though.
#define AI_SYNC 0

TP_THIS_FUNCTION(TCheckForNewPackage, bool, void, Actor* apActor, uint64_t aUnk1);
static TCheckForNewPackage* RealCheckForNewPackage = nullptr;

bool TP_MAKE_THISCALL(HookCheckForNewPackage, void, Actor* apActor, uint64_t aUnk1)
{
#if AI_SYNC
    if (apActor && apActor->GetExtension()->IsRemote())
        return false;

#endif
    return TiltedPhoques::ThisCall(RealCheckForNewPackage, apThis, apActor, aUnk1);
}

TP_THIS_FUNCTION(TInitFromPackage, void, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor);
static TInitFromPackage* RealInitFromPackage = nullptr;

void TP_MAKE_THISCALL(HookInitFromPackage, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor)
{
#if AI_SYNC
    // This guard is here for when the client sets the package based on a remote message
    if (s_execInitPackage)
        return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);

    if (arActor && arActor->GetExtension()->IsRemote())
        return;

    if (arActor && apPackage)
        World::Get().GetRunner().Trigger(InitPackageEvent(arActor->formID, apPackage->formID));

#endif
    return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);
}

TP_THIS_FUNCTION(TSetCurrentPickREFR, void, Console, BSPointerHandle<TESObjectREFR>* apRefr);
static TSetCurrentPickREFR* RealSetCurrentPickREFR = nullptr;

void TP_MAKE_THISCALL(HookSetCurrentPickREFR, Console, BSPointerHandle<TESObjectREFR>* apRefr)
{
    uint32_t formId = 0;

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(apRefr->handle.iBits);
    if (pObject)
        formId = pObject->formID;

    World::Get().GetDebugService().SetDebugId(formId);

    return TiltedPhoques::ThisCall(RealSetCurrentPickREFR, apThis, apRefr);
}

static TiltedPhoques::Initializer s_referencesHooks(
    []()
    {
        POINTER_SKYRIMSE(TCheckForNewPackage, s_checkForNewPackage, 39114);
        POINTER_SKYRIMSE(TInitFromPackage, s_initFromPackage, 38959);
        POINTER_SKYRIMSE(TSetCurrentPickREFR, s_setCurrentPickREFR, 51093);

        RealCheckForNewPackage = s_checkForNewPackage.Get();
        RealInitFromPackage = s_initFromPackage.Get();
        RealSetCurrentPickREFR = s_setCurrentPickREFR.Get();

        TP_HOOK(&RealCheckForNewPackage, HookCheckForNewPackage);
        TP_HOOK(&RealInitFromPackage, HookInitFromPackage);
        TP_HOOK(&RealSetCurrentPickREFR, HookSetCurrentPickREFR);
    });
