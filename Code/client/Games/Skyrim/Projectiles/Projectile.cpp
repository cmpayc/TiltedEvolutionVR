#include "Projectile.h"
#include <Games/Skyrim/Forms/TESObjectWEAP.h>
#include <Games/Skyrim/Forms/MagicItem.h>
#include <Games/Skyrim/Forms/TESAmmo.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <World.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Forms/SpellItem.h>

TP_THIS_FUNCTION(TLaunch, BSPointerHandle<Projectile>*, BSPointerHandle<Projectile>, Projectile::LaunchData& arData);
static TLaunch* RealLaunch = nullptr;

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, LaunchData& apLaunchData) noexcept
{
    BSPointerHandle<Projectile>* result = TiltedPhoques::ThisCall(RealLaunch, apResult, apLaunchData);

    TP_ASSERT(result, "No projectile handle returned.");
    if (!result)
    {
        spdlog::error("No projectile handle returned.");
        return nullptr;
    }

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");
    if (!pProjectile)
    {
        spdlog::error("No projectile found.");
        return nullptr;
    }

    pProjectile->fPower = apLaunchData.fPower;

    return result;
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, Projectile::LaunchData& arData)
{
    // sync concentration spells through spell cast sync, the rest through projectile sync
    if (arData.pSpell)
    {
        if (auto* pSpell = Cast<SpellItem>(arData.pSpell))
        {
            if (pSpell->eCastingType == MagicSystem::CastingType::CONCENTRATION)
            {
                return TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
            }
        }
    }

    if (arData.pShooter)
    {
        Actor* pActor = Cast<Actor>(arData.pShooter);
        if (pActor)
        {
            ActorExtension* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->IsRemote())
            {
                apThis->handle.iBits = 0;
                return apThis;
            }
        }
    }

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
    if (arData.pProjectileBase)
        Event.ProjectileBaseID = arData.pProjectileBase->formID;
    if (arData.pShooter)
        Event.ShooterID = arData.pShooter->formID;
    if (arData.pFromWeapon)
        Event.WeaponID = arData.pFromWeapon->formID;
    if (arData.pFromAmmo)
        Event.AmmoID = arData.pFromAmmo->formID;
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    if (arData.pParentCell)
        Event.ParentCellID = arData.pParentCell->formID;
    if (arData.pSpell)
        Event.SpellID = arData.pSpell->formID;
    Event.CastingSource = arData.eCastingSource;
    Event.UnkBool1 = arData.bUnkBool1;
    Event.Area = arData.iArea;
    Event.Power = arData.fPower;
    Event.Scale = arData.fScale;
    Event.AlwaysHit = arData.bAlwaysHit;
    Event.NoDamageOutsideCombat = arData.bNoDamageOutsideCombat;
    Event.AutoAim = arData.bAutoAim;
    Event.UnkBool2 = arData.bUnkBool2;
    Event.DeferInitialization = arData.bDeferInitialization;
    Event.ForceConeOfFire = arData.bForceConeOfFire;

    auto result = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);

    TP_ASSERT(result, "No projectile handle returned.");

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");

    Event.Power = pProjectile->fPower;

    World::Get().GetRunner().Trigger(Event);

    return result;
}

// The patch below rewrites a load inside id 34452, MagicCaster::LaunchSpell, into a null check on the
// value loaded, so that a null projectile makes the function return false instead of being
// dereferenced. Every constant it uses is codegen specific and none of them survived to VR, which is
// why a VR build crashed executing a bogus address the moment a spell was cast: the jump landed in the
// middle of an instruction and over the front of a call, and the return path unwound 0x138 bytes of a
// 0x158 byte frame, so its `ret` jumped to whatever it happened to pop.
//
// The site is the `mov rbx,[rsp+slot]` in this run, which is identical in both builds and is what
// pairs the two: `mov qword [rsp+slot],0` / `lea rdx,[rsp+slot]` / `lea rcx,[rbp+0xB0]` / call / nop /
// **mov rbx,[rsp+slot]** / `mov eax,[rip+...]` / `cmp [rbx+...],eax` / `jne` / `test r12b,r12b` /
// `je` / `mov rdx,[rbp-0x38]` / `test rdx,rdx` / `je` / `test dword [rdx+0x28],0x3FF`. SE 0x1405C00D4
// against VR 0x140554D1A, and the `cmp` displacement differs by the usual -8, 0x12C against 0x124.
//
// The load is 5 bytes in both, so the resume offset is the site plus 5. The frame size is the
// function's own `sub rsp`, and the epilogue the exit path reproduces occurs exactly once in each
// body: SE `add rsp,0x138`, VR `add rsp,0x158`, each followed by the same eight pops, which are the
// reverse of the same eight pushes in both prologues and therefore need no per build handling.
//
// The site offset is best obtained by scanning the function for `48 8b 5c 24 xx`, not by reading it
// off a hex dump: each body contains exactly two of them and they pair one for one, SE +0x1C8 with
// VR +0x1C6 and SE +0x374 with VR +0x397, so the second is unambiguous. Reading the row by eye put
// this three bytes late on the first attempt, which landed the jump inside the instruction it was
// supposed to replace.
#if TP_SKYRIMVR
static constexpr size_t kLaunchSpellSite = 0x397;
static constexpr size_t kLaunchSpellResume = 0x39C;
static constexpr int32_t kLaunchSpellLocal = 0x58;
static constexpr uint32_t kLaunchSpellFrame = 0x158;
#else
static constexpr size_t kLaunchSpellSite = 0x374;
static constexpr size_t kLaunchSpellResume = 0x379;
static constexpr int32_t kLaunchSpellLocal = 0x50;
static constexpr uint32_t kLaunchSpellFrame = 0x138;
#endif

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        POINTER_SKYRIMSE(TLaunch, s_launch, 44108);

        RealLaunch = s_launch.Get();

        TP_HOOK(&RealLaunch, HookLaunch);

        VersionDbPtr<uint8_t> hookLoc(34452);

        struct C : TiltedPhoques::CodeGenerator
        {
            C(uint8_t* apLoc)
            {
                // replicate
                mov(rbx, ptr[rsp + kLaunchSpellLocal]);

                // nullptr check
                cmp(rbx, 0);
                jz("exit");
                // jump back
                jmp_S(apLoc + kLaunchSpellResume);

                L("exit");
                // return false; scratch space from the registers
                mov(al, 0);
                add(rsp, kLaunchSpellFrame);
                pop(r15);
                pop(r14);
                pop(r13);
                pop(r12);
                pop(rdi);
                pop(rsi);
                pop(rbx);
                pop(rbp);
                ret();
            }
        } gen(hookLoc.Get());
        TiltedPhoques::Jump(hookLoc.Get() + kLaunchSpellSite, gen.getCode());
    });
