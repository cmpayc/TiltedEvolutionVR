#include <TiltedOnlinePCH.h>

#include <Services/HiggsService.h>

#include <Events/UpdateEvent.h>
#include <Events/ObjectHoldEvent.h>

#if TP_SKYRIMVR

#include <ModCompat/HiggsAPI.h>

#include <World.h>

#include <Actor.h>
#include <TESObjectREFR.h>
#include <Forms/TESForm.h>

namespace
{
IHiggsInterface001* s_pInterface = nullptr;

// SKSE loads its plugins moments after the client starts it, so this normally succeeds on the first
// attempt. The window is generous only so a slow load cannot miss it.
constexpr double kRetryInterval = 1.0;
constexpr uint32_t kMaxAttempts = 60;

enum class HandEvent
{
    Pulled,
    Grabbed,
    Dropped,
};

const char* Describe(const HandEvent aKind) noexcept
{
    switch (aKind)
    {
    case HandEvent::Pulled: return "pulled";
    case HandEvent::Grabbed: return "grabbed";
    case HandEvent::Dropped: return "dropped";
    }

    return "unknown";
}

struct HandEventRecord
{
    HandEvent Kind{};
    bool IsLeft{};
    uint32_t FormId{};
};

/**
 * @brief What each hand was last reported holding, index 0 right and 1 left, matching ObjectService's slots.
 *
 * This is what ObjectService has been *told*, not what HIGGS holds, and the gap between the two is the whole
 * point of it. See ReconcileHolds.
 */
uint32_t s_held[2]{};

std::mutex s_queueMutex;

// Deliberately std::vector rather than TiltedPhoques::Vector. This is filled from HIGGS's threads and
// drained on the update, and the TiltedPhoques containers allocate through the current allocator, which
// is per thread. Not worth the risk for a queue this small.
std::vector<HandEventRecord> s_queue;

// HIGGS gives its callbacks no user data, so these have to be free functions, and it calls them from
// whatever thread it happens to be on. Havok workers and the update thread have both been observed,
// with the worker ids differing every time, so assume any thread.
//
// That makes the form id the only thing worth taking here. The registry, the world and the transport are
// none of them safe from here, and even the reference is unsafe to keep: reading formID now is safer
// than storing the pointer and dereferencing it a frame later, when it may already be gone.
void Enqueue(const HandEvent aKind, const bool aIsLeft, TESObjectREFR* apObject) noexcept
{
    if (!apObject)
        return;

    std::scoped_lock lock(s_queueMutex);
    s_queue.push_back(HandEventRecord{aKind, aIsLeft, apObject->formID});
}

void OnPulled(bool aIsLeft, TESObjectREFR* apObject) noexcept
{
    Enqueue(HandEvent::Pulled, aIsLeft, apObject);
}

void OnGrabbed(bool aIsLeft, TESObjectREFR* apObject) noexcept
{
    Enqueue(HandEvent::Grabbed, aIsLeft, apObject);
}

void OnDropped(bool aIsLeft, TESObjectREFR* apObject) noexcept
{
    Enqueue(HandEvent::Dropped, aIsLeft, apObject);
}

// Which holds are worth telling other clients about.
//
// Whether the object can be *named* on the wire is deliberately not decided here. A static reference
// travels by form id and a dropped item by the id of the drop that created it, and only ObjectService
// knows which dropped items it has a pairing for. Duplicating that judgement would just be a second place
// for the two to disagree, so everything non-actor is dispatched and ObjectService drops what it cannot
// name.
bool IsSyncable(TESObjectREFR* apObject) noexcept
{
    // Actors are excluded because a ragdoll cannot be driven by writing to its reference. Four separate
    // writes were measured doing nothing at all to a dropped corpse while the same code moves an
    // ordinary object, because the ragdoll writes the reference's position from itself every frame.
    // Dragging bodies needs the ragdoll's own rigid body and is a separate job. See PROGRESS.md.
    return !Cast<Actor>(apObject);
}

void HandleEvent(const HandEventRecord& acRecord) noexcept
{
    const char* pKind = Describe(acRecord.Kind);
    const char* pHand = acRecord.IsLeft ? "left" : "right";

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acRecord.FormId));
    if (!pObject)
    {
        spdlog::warn("HIGGS {} ({} hand): form {:X} no longer resolves", pKind, pHand, acRecord.FormId);
        return;
    }

    const uint32_t cBaseId = pObject->baseForm ? pObject->baseForm->formID : 0;
    const auto cBaseType = pObject->baseForm ? static_cast<uint32_t>(pObject->baseForm->formType) : 0;

    spdlog::info("HIGGS {} ({} hand): form {:X} ({}), base {:X} type {}, at ({:.1f}, {:.1f}, {:.1f})", pKind, pHand, pObject->formID, pObject->IsTemporary() ? "temporary" : "static", cBaseId, cBaseType, pObject->position.x, pObject->position.y, pObject->position.z);

    // A pull is the yank towards the hand that precedes a distance grab, and it does not always end in
    // one, so the stream starts at the grab. Only grabs and drops bracket a hold.
    if (acRecord.Kind == HandEvent::Pulled)
        return;

    if (!IsSyncable(pObject))
        return;

    const size_t cSlot = acRecord.IsLeft ? 1 : 0;
    const bool cReleased = acRecord.Kind == HandEvent::Dropped;

    // Only what is actually dispatched is recorded, so the two stay in step: everything refused above never
    // reached ObjectService and must not look like a hold that needs ending.
    if (cReleased)
    {
        if (s_held[cSlot] == pObject->formID)
            s_held[cSlot] = 0;
    }
    else
    {
        s_held[cSlot] = pObject->formID;
    }

    World::Get().GetDispatcher().trigger(ObjectHoldEvent(pObject->formID, acRecord.IsLeft, cReleased));
}

/**
 * @brief Ends a hold HIGGS never reported ending.
 *
 * The dropped callback is not reliable for a two handed hold. Measured on 2026-08-22: 5C004 was grabbed by the
 * left hand at 20:46:32.417 and no left drop ever arrived, while the right hand went on to grab and drop it
 * twice more. ObjectService's left slot therefore never cleared, so it streamed the object at 30 Hz for the
 * rest of the session and the other client streamed it straight back. That is the mutual warp loop
 * OnObjectTransformNotify warns about, and it ran for twenty seconds before the cell sweep caught sixteen
 * untouched objects being dragged along in one direction at fifteen units a second.
 *
 * A live query settles it where an event cannot: IsHoldingObject says whether the hand has anything at all,
 * and GetGrabbedObject says what. Two virtual calls a hand per update, against a 30 Hz stream that otherwise
 * never stops.
 */
void ReconcileHolds() noexcept
{
    if (!s_pInterface)
        return;

    /**
     * Both hands are read first, and a believed hold survives if *either* of them reports the object.
     *
     * Per hand was wrong and the log of 2026-08-22 21:03 says so: the right hand grabbed 5C004 at 21:03:42.357,
     * the left joined it at 21:03:43.441, and 0.28s later HIGGS reported the right hand holding nothing while
     * the object was plainly still in both. Two handing moves the object to one hand's slot, so asking each
     * hand about its own slot ends a hold that is still going on.
     *
     * Asked via IsHoldingObject first in each case: a pointer left behind from a previous hold would read as a
     * live one, and that staleness is what this exists to catch, so the flag is the gate.
     */
    uint32_t actual[std::size(s_held)]{};

    for (size_t hand = 0; hand < std::size(actual); ++hand)
    {
        const bool cIsLeft = hand == 1;

        if (!s_pInterface->IsHoldingObject(cIsLeft))
            continue;

        if (const TESObjectREFR* pGrabbed = s_pInterface->GetGrabbedObject(cIsLeft))
            actual[hand] = pGrabbed->formID;
    }

    for (size_t hand = 0; hand < std::size(s_held); ++hand)
    {
        const uint32_t cBelieved = s_held[hand];
        if (!cBelieved)
            continue;

        if (cBelieved == actual[0] || cBelieved == actual[1])
            continue;

        const bool cIsLeft = hand == 1;

        spdlog::warn("HIGGS never reported the {} hand letting go of {:X}, and neither hand holds it now (right {:X}, left {:X}), so the hold is ended here",
                     cIsLeft ? "left" : "right", cBelieved, actual[0], actual[1]);

        s_held[hand] = 0;

        World::Get().GetDispatcher().trigger(ObjectHoldEvent(cBelieved, cIsLeft, true));
    }
}
} // namespace

HiggsService::HiggsService(entt::dispatcher& aDispatcher)
    : m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&HiggsService::OnUpdate>(this))
{
}

void HiggsService::Resolve(const double aDelta) noexcept
{
    if (m_settled)
        return;

    m_sinceLastAttempt += aDelta;
    if (m_sinceLastAttempt < kRetryInterval)
        return;

    m_sinceLastAttempt = 0.0;
    ++m_attempts;

    s_pInterface = HiggsAPI::Acquire();

    if (!s_pInterface)
    {
        if (m_attempts < kMaxAttempts)
            return;

        // Not an error. Running SkyrimVR without HIGGS is a legitimate setup, it just has no hand
        // physics to sync. Say what is off and what still works, so this does not read as a fault.
        spdlog::warn("HIGGS not found after {}s. Objects you carry will not be synced to other players. Objects other players carry are still shown.", kMaxAttempts);
        m_settled = true;

        return;
    }

    // HIGGS has no way to unregister a callback, so this has to happen exactly once.
    s_pInterface->AddPulledCallback(&OnPulled);
    s_pInterface->AddGrabbedCallback(&OnGrabbed);
    s_pInterface->AddDroppedCallback(&OnDropped);

    m_settled = true;

    spdlog::info("HIGGS hand events subscribed");
}

void HiggsService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    Resolve(acEvent.Delta);

    std::vector<HandEventRecord> events;
    {
        std::scoped_lock lock(s_queueMutex);
        events.swap(s_queue);
    }

    for (const HandEventRecord& record : events)
        HandleEvent(record);

    // After the drain, so a grab and its drop arriving in the same batch are both accounted for before the
    // live state is compared against them.
    ReconcileHolds();
}

#else

// HIGGS is a SkyrimVR mod. Nothing to poll for on SE, so the update sink is left unconnected and no hold
// events are ever dispatched. An SE client still receives and applies object transforms, which is what
// lets it show what a VR player is carrying.
HiggsService::HiggsService(entt::dispatcher&)
{
}

void HiggsService::Resolve(double) noexcept
{
}

void HiggsService::OnUpdate(const UpdateEvent&) noexcept
{
}

#endif
