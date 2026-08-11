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
bool IsSyncable(TESObjectREFR* apObject) noexcept
{
    // A temporary form id (at or above 0xFF000000) was created at runtime and names nothing on another
    // client. That covers everything taken out of an inventory, which needs a server-assigned id this
    // protocol does not have.
    if (apObject->IsTemporary())
        return false;

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

    World::Get().GetDispatcher().trigger(ObjectHoldEvent(pObject->formID, acRecord.IsLeft, acRecord.Kind == HandEvent::Dropped));
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
