#include <Services/ObjectService.h>

#include <World.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestObjectTransform.h>
#include <Messages/NotifyObjectTransform.h>
#include <Events/ObjectHoldEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>

#include <inttypes.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ObjectService::OnUpdate>(this);
    m_objectHoldConnection = aDispatcher.sink<ObjectHoldEvent>().connect<&ObjectService::OnObjectHold>(this);
    m_objectTransformConnection = aDispatcher.sink<NotifyObjectTransform>().connect<&ObjectService::OnObjectTransformNotify>(this);

    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
}

bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    if (pCell && pCell->loadedCellData && pCell->loadedCellData->encounterZone)
    {
        // Only return true if cell has the NoResetZone encounter zone
        if (pCell->loadedCellData->encounterZone->formID == 0xf90b1)
        {
            switch (pCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

bool ShouldSyncObject(const TESObjectREFR* apObject) noexcept
{
    if (!apObject)
        return false;

    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        return true;
    }
}

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // TODO(cosideci): clear object components
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer->parentCell;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;

    GameId cellId{};
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    GameId worldSpaceId{};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            return;
        }
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // Door seemed to be at the wrong form id (29, now 32), verify this.
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    AssignObjectsRequest request{};

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject))
        {
            spdlog::warn("Excluding sync for {:X}", pObject->formID);
            continue;
        }

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id))
        {
            spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        if (pObject->baseForm->formType == FormType::Container)
            objectData.CurrentInventory = pObject->GetInventory();

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);

        if (objectData.IsSenderFirst)
            continue;

        if (objectData.CurrentLockData != LockData{})
        {
            Lock* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
                if (!pLock)
                    continue;
            }

            pLock->lockLevel = objectData.CurrentLockData.LockLevel;
            pLock->SetLock(objectData.CurrentLockData.IsLocked);
            pObject->LockChange();
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);
        }
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    auto it = std::find_if(view.begin(), view.end(), [acServerId, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == acServerId; });

    if (it != view.end())
        return *it;

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ActivateFlag)
    {
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    if (!m_transport.IsConnected())
        return;

    if (Lock* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", acEvent.pObject->formID);
        return;
    }

    TESObjectCELL* pCell = acEvent.pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", acEvent.pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", acEvent.pObject->parentCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        // spdlog::error("Activator entity not found for form id {:X}", acEvent.pActivator->formID);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
        TESObjectREFR::OpenState localState = pObject->GetOpenState();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pObject->parentCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    if(!acMessage.IsLocked)
    {
        if (!pLock || !pLock->IsLocked())
            return;
    }

    if (!pLock && acMessage.IsLocked)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = Cast<TESObjectREFR>(pForm);

    if (!pObject)
    {
        spdlog::error("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
        return;
    }

    BSFixedString eventName(acMessage.EventName.c_str());
    if (acMessage.Animation == String{})
    {
        pObject->PlayAnimation(&eventName);
    }
    else
    {
        BSFixedString animation(acMessage.Animation.c_str());
        pObject->PlayAnimationAndWait(&animation, &eventName);
    }
}

void ObjectService::OnUpdate(const UpdateEvent&) noexcept
{
    RunHeldObjectUpdates();
}

void ObjectService::OnObjectHold(const ObjectHoldEvent& acEvent) noexcept
{
    const size_t cSlot = acEvent.IsLeft ? 1 : 0;

    if (!acEvent.IsReleased)
    {
        m_heldByHand[cSlot] = acEvent.FormId;

        // Picking it up again ends any settling from a previous throw.
        StopSettling(acEvent.FormId);

        return;
    }

    if (m_heldByHand[cSlot] != acEvent.FormId)
        return;

    m_heldByHand[cSlot] = 0;

    // Only start settling once no hand has it. Letting go with one hand while the other still holds on
    // is not a release.
    const size_t cOtherSlot = cSlot ^ 1;
    if (m_heldByHand[cOtherSlot] == acEvent.FormId)
        return;

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));
    if (!pObject)
        return;

    SettlingObject settling{};
    settling.FormId = acEvent.FormId;
    settling.LastPosition = pObject->position;

    m_settling.push_back(settling);
}

void ObjectService::StopSettling(const uint32_t acFormId) noexcept
{
    for (auto it = m_settling.begin(); it != m_settling.end(); ++it)
    {
        if (it->FormId != acFormId)
            continue;

        m_settling.erase(it);
        return;
    }
}

// 30 Hz. The 10 Hz the actor path uses is far too slow for something attached to a hand, and the server
// relays these the moment they arrive rather than batching, so the rate here is the rate other clients
// see. Nothing is sent unless something is held or still settling.
void ObjectService::RunHeldObjectUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    if (!m_heldByHand[0] && !m_heldByHand[1] && m_settling.empty())
        return;

    constexpr auto cDelayBetweenUpdates = 1000ms / 30;

    // Give up on a throw that will not settle, rather than streaming a cabbage rolling down a hill for
    // ever. The receiver takes over from wherever it had got to, which is a small disagreement at worst.
    constexpr double kMaxSettleTime = 3.0;
    // Below this much movement in a tick the object counts as still. A few consecutive still ticks, so a
    // cabbage pausing at the top of a bounce is not mistaken for one that has landed.
    constexpr float kRestThreshold = 0.5f;
    constexpr uint32_t kRestTicks = 3;

    static std::chrono::steady_clock::time_point lastSendTimePoint;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    for (size_t i = 0; i < std::size(m_heldByHand); ++i)
    {
        const uint32_t cFormId = m_heldByHand[i];
        if (!cFormId)
            continue;

        // Two-handed hold, already sent for the other hand this tick.
        if (i > 0 && m_heldByHand[i - 1] == cFormId)
            continue;

        SendObjectTransform(cFormId, false);
    }

    constexpr double cTickSeconds = 1.0 / 30.0;

    for (size_t i = m_settling.size(); i > 0; --i)
    {
        SettlingObject& settling = m_settling[i - 1];

        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(settling.FormId));
        if (!pObject)
        {
            m_settling.erase(m_settling.begin() + (i - 1));
            continue;
        }

        const float cMoved = glm::length(glm::vec3(pObject->position) - settling.LastPosition);
        settling.LastPosition = pObject->position;
        settling.Elapsed += cTickSeconds;

        settling.StillTicks = cMoved < kRestThreshold ? settling.StillTicks + 1 : 0;

        const bool cSettled = settling.StillTicks >= kRestTicks || settling.Elapsed >= kMaxSettleTime;

        // The final message is the only one sent without warp, which is what hands the object back to
        // the receiver's own physics. By now it is not moving, so there is nothing left to disagree on.
        SendObjectTransform(settling.FormId, cSettled);

        if (cSettled)
            m_settling.erase(m_settling.begin() + (i - 1));
    }
}

void ObjectService::SendObjectTransform(const uint32_t acFormId, const bool aIsReleased) noexcept
{
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acFormId));
    if (!pObject)
        return;

    RequestObjectTransform request{};

    if (!m_world.GetModSystem().GetServerModId(acFormId, request.Id))
    {
        spdlog::error("Server form id not found for held object {:X}", acFormId);
        return;
    }

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
        return;

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for held object {:X}, cell {:X}", acFormId, pCell->formID);
        return;
    }

    request.Position = pObject->position;
    request.Rotation = pObject->rotation;
    request.IsReleased = aIsReleased;

    m_transport.Send(request);

    if (aIsReleased)
        spdlog::info("Held object {:X} came to rest at ({:.1f}, {:.1f}, {:.1f})", acFormId, pObject->position.x, pObject->position.y, pObject->position.z);
}

void ObjectService::OnObjectTransformNotify(const NotifyObjectTransform& acMessage) noexcept
{
    const uint32_t cObjectId = m_world.GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
        return;

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
        return;

    // Someone else is driving this object now, most likely because they took it out of our hand or off
    // the floor while our throw was still settling. Their stream wins: two clients writing the same
    // object would only fight.
    StopSettling(cObjectId);

    pObject->position = acMessage.Position;
    pObject->SetRotation(acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z);

    // The warp flag is the whole difference between a held object and a released one, and both halves
    // were measured rather than guessed.
    //
    // While held, warp. That teleports the body to the write, so x and y track exactly and z sags by
    // about one gravity step between writes. What it also does is leave the body detached from the node,
    // which is invisible while we keep writing every 33 ms and becomes obvious the moment we stop: the
    // object hangs in the air, never falls, and cannot be picked up again.
    //
    // On release, do not warp. The ordinary sync puts the body and the node back together and hands the
    // object to local physics. Measured on two machines: without this the object falls 0 units after the
    // stream ends, with it 133 to 178. MoveTo instead of this restores nothing (0 units, twice) and
    // Disable/Enable is inconsistent (4 units, then 11.8). See PROGRESS.md.
    pObject->Update3DPosition(!acMessage.IsReleased);

    if (acMessage.IsReleased)
        spdlog::info("Remote object {:X} released at ({:.1f}, {:.1f}, {:.1f})", cObjectId, pObject->position.x, pObject->position.y, pObject->position.z);
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if ENVIRONMENT_DEBUG
    auto view = m_world.view<ObjectComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acEvent->object->formID, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == id; });

    if (itor == std::end(view))
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}
