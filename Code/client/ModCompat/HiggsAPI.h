#pragma once

#if TP_SKYRIMVR

struct TESForm;
struct TESObjectREFR;
struct NiObject;

/**
 * @brief HIGGS's plugin interface, mirrored from include/higgsinterface001.h in
 * https://github.com/adamhynek/higgs.
 *
 * Slot order is the entire contract. Every entry has to stay at its index, so add nothing in the
 * middle and reorder nothing. Verified against the shipped higgs_vr.dll v1.10.10: its vtable holds
 * exactly the 41 entries declared here, slot 0 is `mov eax, 0x10CCC8 / ret` (GetBuildNumber
 * returning 1101000) and slots 1-3 each open with `test rdx, rdx / jz`, the null check every
 * Add*Callback does on its one argument.
 *
 * Slots named Unmodelled_ take or return types the client has no definition for (hkpCollisionFilter,
 * NiTransform, BSFixedString, std::string_view). They exist only to keep the slots after them at the
 * right index. Give one a real signature before calling it; as declared, calling it is a bug.
 */
struct IHiggsInterface001
{
    enum class CollisionFilterComparisonResult : uint8_t
    {
        Continue,
        Collide,
        Ignore,
    };

    using PulledCallback = void (*)(bool aIsLeft, TESObjectREFR* apPulled);
    using GrabbedCallback = void (*)(bool aIsLeft, TESObjectREFR* apGrabbed);
    using DroppedCallback = void (*)(bool aIsLeft, TESObjectREFR* apDropped);
    using StashedCallback = void (*)(bool aIsLeft, TESForm* apStashed);
    using ConsumedCallback = void (*)(bool aIsLeft, TESForm* apConsumed);
    using CollisionCallback = void (*)(bool aIsLeft, float aMass, float aSeparatingVelocity);
    using NoArgCallback = void (*)();

    virtual uint32_t GetBuildNumber() = 0;                                  // 0
    virtual void AddPulledCallback(PulledCallback aCallback) = 0;           // 1
    virtual void AddGrabbedCallback(GrabbedCallback aCallback) = 0;         // 2
    virtual void AddDroppedCallback(DroppedCallback aCallback) = 0;         // 3
    virtual void AddStashedCallback(StashedCallback aCallback) = 0;         // 4
    virtual void AddConsumedCallback(ConsumedCallback aCallback) = 0;       // 5
    virtual void AddCollisionCallback(CollisionCallback aCallback) = 0;     // 6
    virtual void GrabObject(TESObjectREFR* apObject, bool aIsLeft) = 0;     // 7
    virtual TESObjectREFR* GetGrabbedObject(bool aIsLeft) = 0;              // 8
    virtual bool IsHandInGrabbableState(bool aIsLeft) = 0;                  // 9
    virtual void DisableHand(bool aIsLeft) = 0;                             // 10
    virtual void EnableHand(bool aIsLeft) = 0;                              // 11
    virtual bool IsDisabled(bool aIsLeft) = 0;                              // 12
    virtual void DisableWeaponCollision(bool aIsLeft) = 0;                  // 13
    virtual void EnableWeaponCollision(bool aIsLeft) = 0;                   // 14
    virtual bool IsWeaponCollisionDisabled(bool aIsLeft) = 0;               // 15
    virtual bool IsTwoHanding() = 0;                                        // 16
    virtual void AddStartTwoHandingCallback(NoArgCallback aCallback) = 0;   // 17
    virtual void AddStopTwoHandingCallback(NoArgCallback aCallback) = 0;    // 18
    virtual bool CanGrabObject(bool aIsLeft) = 0;                           // 19
    virtual void Unmodelled_AddCollisionFilterComparisonCallback() = 0;     // 20
    virtual void Unmodelled_AddPrePhysicsStepCallback() = 0;                // 21
    virtual uint64_t GetHiggsLayerBitfield() = 0;                           // 22
    virtual void SetHiggsLayerBitfield(uint64_t aBitfield) = 0;             // 23
    virtual NiObject* GetHandRigidBody(bool aIsLeft) = 0;                   // 24
    virtual NiObject* GetWeaponRigidBody(bool aIsLeft) = 0;                 // 25
    virtual NiObject* GetGrabbedRigidBody(bool aIsLeft) = 0;                // 26
    virtual void ForceWeaponCollisionEnabled(bool aIsLeft) = 0;             // 27
    virtual bool IsHoldingObject(bool aIsLeft) = 0;                         // 28
    virtual void Unmodelled_GetFingerValues() = 0;                          // 29
    virtual void AddPreVrikPreHiggsCallback(NoArgCallback aCallback) = 0;   // 30
    virtual void AddPreVrikPostHiggsCallback(NoArgCallback aCallback) = 0;  // 31
    virtual void AddPostVrikPreHiggsCallback(NoArgCallback aCallback) = 0;  // 32
    virtual void AddPostVrikPostHiggsCallback(NoArgCallback aCallback) = 0; // 33
    virtual void Unmodelled_Deprecated1() = 0;                              // 34
    virtual void Unmodelled_Deprecated2() = 0;                              // 35
    virtual void Unmodelled_GetGrabTransform() = 0;                         // 36
    virtual void Unmodelled_SetGrabTransform() = 0;                         // 37
    virtual bool GetSettingDouble(const char* acpName, double& aOut) = 0;   // 38
    virtual bool SetSettingDouble(const char* acpName, double aValue) = 0;  // 39
    virtual void Unmodelled_GetGrabbedNodeName() = 0;                       // 40
};

namespace HiggsAPI
{
/**
 * @brief Locates HIGGS's interface singleton inside a loaded higgs_vr.dll.
 *
 * HIGGS hands this object out in reply to an SKSE message, which needs a plugin handle the client
 * has no way to get: it loads SKSE itself rather than being loaded by it. So it finds the same
 * object directly by walking HIGGS's RTTI. See HiggsAPI.cpp.
 *
 * Returns null when higgs_vr.dll is not loaded, which is the ordinary "user has no HIGGS" case.
 * A load that is present but unwalkable logs an error and also returns null.
 */
IHiggsInterface001* Acquire() noexcept;
} // namespace HiggsAPI

#endif
