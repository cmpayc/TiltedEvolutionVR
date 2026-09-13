#include <TESObjectREFR.h>

#if TP_SKYRIMVR

#include <Forms/TESObjectCELL.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiNode.h>

/**
 * @brief Stops the Havok island activation listener dereferencing a reference that has no 3D.
 *
 * The crash is `SkyrimVR.exe+03AD7B1`, seen on 2026-08-16, 2026-08-19 and 2026-08-30, and it is not a port
 * defect: SE 1.6.1170 at `0x1403F6438` has the identical unguarded read. AE id 25857 is a listener the Havok
 * world broadcasts to when a simulation island activates. It takes the entity, reads its collision object,
 * finds the reference that owns that object's 3D, and reparents the 3D under the right shadow scene node. It
 * calls `GetNiNode` (virtual slot 0x70) four times and dereferences the result at +0x30 every time without
 * ever checking it, so a reference with no 3D faults on whichever branch it takes.
 *
 * The 2026-08-30 dump named the object for the first time: `FF000D89`, a plain `TESObjectREFR` rather than an
 * actor, with a parent cell and a `loadedData`, but `loadedData->data3D` null. It was reached from our own
 * `ActorValueService::OnHealthChangeBroadcast` calling `Actor::Kill`, which puts a ragdoll into the world,
 * which activates the island the stale body was sitting in.
 *
 * What leaves a reference in that state is still open. `TESObjectREFR::GetByHandle` leaking a handle was one
 * cause and is fixed; this one was not deleted by us at all. So this is a guard and not the fix: when it
 * fires it names the reference, which is the thing every previous attempt at this crash lacked.
 *
 * Skipping the whole listener costs nothing in that state. Its tail is the reparent itself, a call that
 * no-ops because the listener passes it a null second argument, and a counter it bumps for light references.
 * The handle reference it takes at VR `0x1403AD6C7` is released at `0x1403AD83D` inside the same call, so
 * skipping is refcount neutral.
 *
 * The cost is one property read, one `FindReferenceFor3D` and one virtual call per activated entity,
 * repeating work the original does immediately afterwards. That is only on island activation, not per frame,
 * and it buys skipping a call that would otherwise fault.
 */

namespace
{
// Key 2 of an `hkpWorldObject`'s property array is its `bhkNiCollisionObject`. Taken from the listener
// itself, which passes a literal 2: `mov r8d, 2` at VR `0x1403AD67F`.
constexpr uint32_t kCollisionObjectProperty = 2;

// `NiCollisionObject::sceneObject`. The listener reads the same field, `mov rcx, [rcx+0x10]` at VR
// `0x1403AD69C`.
constexpr size_t kSceneObjectOffset = 0x10;

using TGetProperty = void**(void** apResult, void* apEntity, uint32_t aKey);
using TFindReferenceFor3D = TESObjectREFR*(NiAVObject* apObject3D);
using TNotifyIslandActivated = void(void* apEntity, bool aUnk);

TNotifyIslandActivated* RealNotifyIslandActivated = nullptr;

// The reference the listener is about to work on, resolved exactly the way it resolves it. Null whenever any
// step of the chain comes up empty, which is the case the listener already handles for itself.
TESObjectREFR* ResolveReference(void* apEntity) noexcept
{
    POINTER_SKYRIMSE(TGetProperty, s_getProperty, 77799);
    POINTER_SKYRIMSE(TFindReferenceFor3D, s_findReferenceFor3D, 19750);

    void* pCollisionObject = nullptr;
    s_getProperty.Get()(&pCollisionObject, apEntity, kCollisionObjectProperty);

    if (!pCollisionObject)
        return nullptr;

    auto* pSceneObject = *reinterpret_cast<NiAVObject**>(reinterpret_cast<uint8_t*>(pCollisionObject) + kSceneObjectOffset);
    if (!pSceneObject)
        return nullptr;

    return s_findReferenceFor3D.Get()(pSceneObject);
}

void HookNotifyIslandActivated(void* apEntity, bool aUnk)
{
    TESObjectREFR* pReference = apEntity ? ResolveReference(apEntity) : nullptr;

    if (pReference && !pReference->GetNiNode())
    {
        // Once per reference. An island that keeps activating would otherwise write this every frame, and the
        // form id is the whole content of the line, so repeats add nothing.
        //
        // Locked because the listener runs on whichever thread woke the island, which is a task pool worker
        // or a Papyrus VM job as often as it is the main thread. Only reached when a reference is already
        // broken, so the cost never lands on a healthy activation.
        static std::mutex s_reportedLock;
        static Set<uint32_t> s_reported;

        std::scoped_lock _{s_reportedLock};

        if (s_reported.size() >= 256)
            s_reported.clear();

        if (s_reported.insert(pReference->formID).second)
        {
            spdlog::warn("Reference {:X} ({}) is in the physics world with no 3D, so the Havok activation listener was skipped for it. Base {:X}, cell {:X}, at ({:.1f}, {:.1f}, {:.1f}).",
                         pReference->formID, pReference->formID >= 0xFF000000 ? "temporary" : "static",
                         pReference->baseForm ? pReference->baseForm->formID : 0,
                         pReference->parentCell ? pReference->parentCell->formID : 0,
                         pReference->position.x, pReference->position.y, pReference->position.z);
        }

        return;
    }

    RealNotifyIslandActivated(apEntity, aUnk);
}

TiltedPhoques::Initializer s_shadowSceneAttachGuard(
    []()
    {
        POINTER_SKYRIMSE(TNotifyIslandActivated, s_notifyIslandActivated, 25857);

        RealNotifyIslandActivated = s_notifyIslandActivated.Get();

        TP_HOOK(&RealNotifyIslandActivated, HookNotifyIslandActivated);
    });
} // namespace

#endif
