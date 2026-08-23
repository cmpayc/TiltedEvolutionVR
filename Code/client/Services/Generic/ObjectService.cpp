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
#include <Messages/RequestObjectRemove.h>
#include <Messages/NotifyObjectRemove.h>
#include <Events/ObjectHoldEvent.h>
#include <Events/DynamicObjectCreatedEvent.h>
#include <Events/ObjectPickedUpEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>
#include <NetImmerse/NiNode.h>

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
    m_dynamicObjectConnection = aDispatcher.sink<DynamicObjectCreatedEvent>().connect<&ObjectService::OnDynamicObjectCreated>(this);
    m_objectPickedUpConnection = aDispatcher.sink<ObjectPickedUpEvent>().connect<&ObjectService::OnObjectPickedUp>(this);
    m_objectRemoveConnection = aDispatcher.sink<NotifyObjectRemove>().connect<&ObjectService::OnObjectRemoveNotify>(this);

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

/**
 * @brief Byte offset of the world transform's translation inside NiAVObject.
 *
 * A temporary reference never has its position written back from physics, so its own position field keeps
 * whatever value it was created with. The 3D node is the only honest source for where a held object actually
 * is, and NiAVObject is otherwise unmapped in this client.
 *
 * Established by measurement rather than from a layout table, by scanning a held **static** object's node for
 * three floats matching its reference position, which is known good for a static reference. Nine samples of a
 * moving cabbage all produced the same five offsets:
 *
 *     0x6c  0xa0  0xd4  0xe4  0xf4
 *
 * The first three are 0x34 apart, which is exactly sizeof(NiTransform) (NiMatrix3 0x24, NiPoint3 0xC, float
 * 0x4), so they are three consecutive transforms. Subtracting the 0x24 matrix gives their starts, 0x48, 0x7c
 * and 0xb0, which are local, world and previousWorld at the same offsets Skyrim SE uses. The last two are
 * worldBound's centre and its VR neighbour, matching only because a cabbage's bounds centre is within
 * tolerance of its position.
 *
 * So world.translate is 0x7c + 0x24, and it is the same on both builds: VR's extra 0x28 bytes sit after these
 * fields, which is consistent with NiAVObject growing from 0x110 to 0x138.
 */
constexpr size_t kNodeTranslateOffset = 0xA0;

// Where the object actually is, as opposed to where its reference claims it is. False when there is no 3D to
// ask, in which case the caller should fall back to the reference.
bool ReadNodeTranslate(TESObjectREFR* apObject, glm::vec3& aOut) noexcept
{
    NiNode* pNode = apObject->GetNiNode();
    if (!pNode)
        return false;

    const auto* pTranslate = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pNode) + kNodeTranslateOffset);

    aOut = glm::vec3(pTranslate[0], pTranslate[1], pTranslate[2]);

    return true;
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

void ObjectService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunHeldObjectUpdates();
    RunDrivenObjectTimeouts();
    RunDriftWatch(acEvent.Delta);
    RunCellDriftSweep(acEvent.Delta);
}

void ObjectService::RestoreObjectPhysics(TESObjectREFR* apObject) noexcept
{
    // Puts the rigid body back together with the node after warping, and hands the object to local physics.
    // Measured: without this an object falls 0 units once the stream ends, with it 133 to 178.
    //
    // This briefly also nudged the object 4 units upwards, on the theory that a body at rest needed a small
    // fall to wake it. That theory was wrong, and the lift was an unexplained hop applied to every released
    // object, so it is gone.
    apObject->Update3DPosition(false);
}

/**
 * @brief Starts watching an object that has just been handed back to local physics.
 *
 * Warping an object detaches its rigid body, and `RestoreObjectPhysics` is the one call that puts it back.
 * Nothing today checks that it worked. An object left with a leftover velocity and no collision drifts in a
 * straight line for ever and passes through walls, which is exactly what has been reported, and it is
 * invisible in the logs because the last line either client writes about the object is "released" or
 * "repaired".
 *
 * So every exit from our control arms this, and three seconds later the object is asked one question: are
 * you still moving? Nothing should be. Silence means the handover worked.
 */
void ObjectService::WatchForDrift(const uint32_t acFormId, const char* acpReason) noexcept
{
    StopWatchingDrift(acFormId);

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acFormId));
    if (!pObject)
        return;

    DriftWatch watch{};
    watch.FormId = acFormId;
    watch.pReason = acpReason;

    if (!ReadNodeTranslate(pObject, watch.Start))
        watch.Start = pObject->position;

    watch.Late = watch.Start;

    m_driftWatch.push_back(watch);
}

void ObjectService::StopWatchingDrift(const uint32_t acFormId) noexcept
{
    for (auto it = m_driftWatch.begin(); it != m_driftWatch.end(); ++it)
    {
        if (it->FormId != acFormId)
            continue;

        m_driftWatch.erase(it);
        return;
    }
}

/**
 * @brief Names the bodies near a drifting object and says whether it is moving away from them.
 *
 * There is one thing this client does that a single player game never does: it teleports another player's
 * body. `Actor::ForcePosition` is `SetPosition(position, aSyncHavok = true)` and
 * `InterpolationSystem::Update` calls it every frame for every remote actor, so a remote body's collision
 * is warped into place at frame rate rather than moved into it. Havok resolves a body that appears inside
 * something by ejecting the smaller of the two, and a body that reappears in the same place next frame
 * ejects it again. That is a steady push in a fixed direction which stops only when the two stop touching,
 * it needs no velocity of its own to keep going, and it cannot happen unless somebody else is connected.
 *
 * The alignment is what tests it. Near +1 the object is moving directly away from that body, which is what
 * being squeezed out looks like. Near zero or negative, that body is not the cause.
 *
 * The local player is reported on the same terms as a control. Their own body moves rather than teleports,
 * so it should not produce sustained ejection, and if it does then the mechanism is not what is written
 * above.
 */
void ObjectService::ReportDriftGeometry(const glm::vec3& acPosition, const glm::vec3& acDirection) noexcept
{
    // About five and a half metres. Contact is a couple of units; the rest of the range is there so a body
    // that has walked away is still reported, which is what makes a trail of windows readable.
    constexpr float kNearby = 400.f;

    const auto cReport = [&](const char* acpWhat, const uint32_t acFormId, const glm::vec3& acBody)
    {
        const glm::vec3 cFromBody = acPosition - acBody;
        const float cDistance = glm::length(cFromBody);

        if (cDistance > kNearby || cDistance <= 0.f)
            return false;

        spdlog::warn("    {} {:X} is {:.1f} units away, drift alignment {:+.2f}", acpWhat, acFormId, cDistance, glm::dot(acDirection, cFromBody / cDistance));

        return true;
    };

    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        cReport("local player", pPlayer->formID, pPlayer->position);

    size_t considered = 0;
    size_t reported = 0;

    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;

        ++considered;

        if (cReport("remote body", pActor->formID, pActor->position))
            ++reported;
    }

    if (!reported)
        spdlog::warn("    no remote body within {:.0f} units, of {} being interpolated", kNearby, considered);
}

void ObjectService::RunDriftWatch(const double aDelta) noexcept
{
    if (m_driftWatch.empty())
        return;

    // Long enough that an object dropped from hand height has landed and stopped bouncing, short enough that
    // the object is still nearby and still named by the same form id.
    constexpr double kWindow = 3.0;
    // The verdict is taken from the last second alone, not from the whole window, because an object that
    // fell and stopped has moved a long way from where it was released and is perfectly healthy.
    constexpr double kLateMark = 2.0;
    // Skyrim units, so this is about 7 cm per second. A slow endless drift sits right around here; anything
    // physical has stopped by now.
    constexpr float kStillMoving = 5.f;

    for (size_t i = m_driftWatch.size(); i > 0; --i)
    {
        DriftWatch& watch = m_driftWatch[i - 1];

        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(watch.FormId));
        if (!pObject)
        {
            m_driftWatch.erase(m_driftWatch.begin() + (i - 1));
            continue;
        }

        watch.Elapsed += aDelta;

        glm::vec3 position{};
        if (!ReadNodeTranslate(pObject, position))
            position = pObject->position;

        if (!watch.LateTaken && watch.Elapsed >= kLateMark)
        {
            watch.Late = position;
            watch.LateTaken = true;
        }

        if (watch.Elapsed < kWindow)
            continue;

        const glm::vec3 cLateMove = position - watch.Late;
        const float cLateMoved = glm::length(cLateMove);

        if (cLateMoved > kStillMoving)
        {
            const glm::vec3 cDirection = cLateMove / cLateMoved;

            spdlog::warn("Object {:X} ({}) is still moving {:.0f}s after {} (window {}): {:.1f} units in the last second, "
                         "direction ({:.2f}, {:.2f}, {:.2f}), now {:.1f} units from where it was let go, at ({:.1f}, {:.1f}, {:.1f})",
                         watch.FormId, watch.FormId >= 0xFF000000 ? "temporary" : "static", kWindow, watch.pReason, watch.Windows, cLateMoved, cDirection.x, cDirection.y, cDirection.z, glm::length(position - watch.Start), position.x, position.y, position.z);

            ReportDriftGeometry(position, cDirection);

            // Keep watching. One window says an object is drifting; a trail of them says whether it keeps
            // drifting after a body moves away, which is the whole question. Bounded, because a cabbage
            // rolling down a hill would otherwise be reported for ever.
            constexpr uint32_t kMaxWindows = 10;

            if (watch.Windows + 1 < kMaxWindows)
            {
                ++watch.Windows;
                watch.Elapsed = 0.0;
                watch.Start = position;
                watch.Late = position;
                watch.LateTaken = false;

                continue;
            }

            spdlog::warn("Object {:X} has been drifting for {:.0f}s, so it is no longer being watched", watch.FormId, kWindow * kMaxWindows);
        }

        m_driftWatch.erase(m_driftWatch.begin() + (i - 1));
    }
}

/**
 * @brief Watches the whole cell for a drift that has taken hold of everything at once.
 *
 * The watch above only ever looks at an object that has just left our control, which is the one thing a mass
 * drift is not. On 2026-08-22 every item in a room set off in the same direction, including ones nobody had
 * touched all session, and not one of them was being watched, so the logs of that run say nothing about it.
 *
 * This samples the cell instead and stays quiet until a crowd of references is moving the same way. Whether
 * that crowd is made of objects we stream or of objects that have never been named on the wire is the
 * question that decides where to look next, so the report counts both.
 */
void ObjectService::RunCellDriftSweep(const double aDelta) noexcept
{
    // A drift worth chasing covers tens of units a second, so a second between samples is plenty, and
    // walking a whole cell more often than that is not free.
    constexpr double kInterval = 1.0;
    // Units a second, the same threshold the per object watch uses: about 7 cm.
    constexpr float kMovingPerSecond = 5.f;
    // Below this it is an ordinary busy room: a thrown cabbage, a swinging door, a torch an NPC dropped.
    constexpr size_t kCrowd = 4;
    // How aligned the crowd has to be to count as one drift rather than several unrelated movements.
    constexpr float kCoherence = 0.8f;
    // This runs on the game's own thread and the comparison below is quadratic in the sample count.
    constexpr size_t kMaxSamples = 512;
    // Named individually, so the same objects can be found in the other client's log.
    constexpr size_t kNamed = 8;
    // One report a second while it lasts, and then silence. The trail matters, but not for ever.
    constexpr uint32_t kMaxReports = 20;

    m_cellSweepElapsed += aDelta;

    ++m_cellSweepFrames;
    m_cellSweepWorstFrame = std::max(m_cellSweepWorstFrame, aDelta);

    if (m_cellSweepElapsed < kInterval)
        return;

    const auto cElapsed = static_cast<float>(m_cellSweepElapsed);
    const auto cFps = static_cast<float>(m_cellSweepFrames) / cElapsed;
    const auto cWorstFrame = m_cellSweepWorstFrame;

    m_cellSweepElapsed = 0.0;
    m_cellSweepFrames = 0;
    m_cellSweepWorstFrame = 0.0;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    TESObjectCELL* pCell = pPlayer->parentCell;
    if (!pCell || !pCell->refData.refArray)
        return;

    // Crossing a cell boundary swaps the whole reference list, and comparing across it would report the room
    // we just left as having moved.
    if (pCell->formID != m_cellSweepId)
    {
        m_cellSweepId = pCell->formID;
        m_cellSamples.clear();
        m_cellSweepReports = 0;
    }

    struct Mover
    {
        uint32_t FormId{};
        glm::vec3 Position{};
        glm::vec3 Direction{};
        float Moved{};
        float ReferenceMoved{};
    };

    Vector<CellSample> samples;
    Vector<Mover> movers;

    const float cThreshold = kMovingPerSecond * cElapsed;

    for (uint32_t i = 0; i < pCell->refData.capacity && samples.size() < kMaxSamples; ++i)
    {
        TESObjectREFR* pRef = pCell->refData.refArray[i].Get();
        if (!pRef)
            continue;

        // Actors move under their own power and are never streamed as objects, so they are noise here.
        if (Cast<Actor>(pRef))
            continue;

        // The node rather than the reference, for the same reason the rest of this file reads it: a
        // temporary's reference position is frozen at creation and would read as perfectly still.
        glm::vec3 position{};
        if (!ReadNodeTranslate(pRef, position))
            continue;

        samples.push_back(CellSample{pRef->formID, position, pRef->position});

        for (const CellSample& cPrevious : m_cellSamples)
        {
            if (cPrevious.FormId != pRef->formID)
                continue;

            const glm::vec3 cMoved = position - cPrevious.Position;
            const float cDistance = glm::length(cMoved);

            if (cDistance > cThreshold)
                movers.push_back(Mover{pRef->formID, position, cMoved / cDistance, cDistance, glm::length(pRef->position - cPrevious.Reference)});

            break;
        }
    }

    m_cellSamples = std::move(samples);

    if (movers.size() < kCrowd)
    {
        m_cellSweepReports = 0;
        return;
    }

    glm::vec3 mean(0.f);

    for (const Mover& cMover : movers)
        mean += cMover.Direction;

    const float cMeanLength = glm::length(mean);
    if (cMeanLength <= 0.f)
        return;

    mean /= cMeanLength;

    float coherence = 0.f;
    float moved = 0.f;

    for (const Mover& cMover : movers)
    {
        coherence += glm::dot(cMover.Direction, mean);
        moved += cMover.Moved;
    }

    coherence /= static_cast<float>(movers.size());

    if (coherence < kCoherence)
    {
        m_cellSweepReports = 0;
        return;
    }

    if (m_cellSweepReports >= kMaxReports)
        return;

    ++m_cellSweepReports;

    size_t held = 0;
    size_t settling = 0;
    size_t driven = 0;
    size_t untouched = 0;
    size_t everHandled = 0;
    size_t nodeOnly = 0;

    for (const Mover& cMover : movers)
    {
        const bool cHeld = m_heldByHand[0] == cMover.FormId || m_heldByHand[1] == cMover.FormId;
        const bool cSettling = std::any_of(m_settling.begin(), m_settling.end(), [&cMover](const SettlingObject& acSettling) { return acSettling.FormId == cMover.FormId; });
        const bool cDriven = std::any_of(m_driven.begin(), m_driven.end(), [&cMover](const DrivenObject& acDriven) { return acDriven.FormId == cMover.FormId; });

        held += cHeld;
        settling += cSettling;
        driven += cDriven;
        everHandled += m_everHandled.count(cMover.FormId) != 0;

        if (!cHeld && !cSettling && !cDriven)
            ++untouched;

        // A node that has moved while the reference has not is a transform being written behind physics's
        // back. A body that is really moving drags the reference along with it.
        if (cMover.ReferenceMoved < 1.f)
            ++nodeOnly;
    }

    const glm::vec3 cPlayerAt = pPlayer->position;
    const float cPlayerMoved = m_cellSweepReports > 1 ? glm::length(cPlayerAt - m_cellSweepPlayerAt) : 0.f;
    m_cellSweepPlayerAt = cPlayerAt;

    /**
     * @brief Whether the crowd is being pushed away from a body, or simply all sliding the same way.
     *
     * This is the question the counts above cannot answer and it does not need the game changed to ask it.
     * Havok ejecting objects out of a body pushes each one along the line from the body to that object, so
     * objects on opposite sides of it travel in opposite directions. A crowd being ejected is radial and its
     * coherence is low. A crowd sliding together is uniform and its coherence is high.
     *
     * Every report so far has had a coherence near 1.00 in a fixed world direction, which already sits badly
     * with ejection from a moving body. Measuring the radiality outright settles it rather than leaving it an
     * inference: near +1 means the nearest remote body is pushing them, near 0 means it is not, whatever else
     * is true.
     */
    glm::vec3 centroid(0.f);

    for (const Mover& cMover : movers)
        centroid += cMover.Position;

    centroid /= static_cast<float>(movers.size());

    uint32_t nearestId = 0;
    float nearestDistance = std::numeric_limits<float>::max();
    glm::vec3 nearestAt(0.f);

    for (const auto cEntity : m_world.view<FormIdComponent, RemoteComponent>())
    {
        const auto& cFormId = m_world.get<FormIdComponent>(cEntity);

        const Actor* pRemote = Cast<Actor>(TESForm::GetById(cFormId.Id));
        if (!pRemote)
            continue;

        const glm::vec3 cAt(pRemote->position);
        const float cDistance = glm::length(cAt - centroid);

        if (cDistance >= nearestDistance)
            continue;

        nearestDistance = cDistance;
        nearestId = cFormId.Id;
        nearestAt = cAt;
    }

    float radiality = 0.f;

    if (nearestId)
    {
        for (const Mover& cMover : movers)
        {
            const glm::vec3 cFromBody = cMover.Position - nearestAt;
            const float cLength = glm::length(cFromBody);

            if (cLength > 0.001f)
                radiality += glm::dot(cMover.Direction, cFromBody / cLength);
        }

        radiality /= static_cast<float>(movers.size());
    }

    /**
     * The last two counts are the ones that decide where to look, and they answer different questions. The
     * first three say what is writing these objects at this moment; "warped or carried at some point" says
     * whether we have ever detached their rigid bodies. A crowd that is idle now but was warped earlier points
     * at a repair that did not take, and a crowd we have genuinely never touched points at the game's own
     * physics and not at us.
     */
    spdlog::warn("Cell drift ({}/{}): {} of {} references in cell {:X} are moving together at {:.1f} units a second, "
                 "direction ({:.2f}, {:.2f}, {:.2f}), coherence {:.2f}. In our hands {}, settling {}, driven by a remote client {}, "
                 "not being written by us {}, warped or carried by us at some point {}, node moved but reference did not {}. "
                 "The player moved {:.1f} units in the same second. Nearest remote body {:X} is {:.0f} units off, and they are moving away from it {:+.2f}. "
                 "Ran at {:.0f} fps, worst frame {:.0f} ms",
                 m_cellSweepReports, kMaxReports, movers.size(), m_cellSamples.size(), pCell->formID, moved / static_cast<float>(movers.size()) / cElapsed,
                 mean.x, mean.y, mean.z, coherence, held, settling, driven, untouched, everHandled, nodeOnly, cPlayerMoved, nearestId,
                 nearestId ? nearestDistance : 0.f, radiality, cFps, cWorstFrame * 1000.0);

    for (size_t i = 0; i < movers.size() && i < kNamed; ++i)
        spdlog::warn("    {:X} ({}) node moved {:.1f} units, reference moved {:.1f}, alignment {:+.2f}{}", movers[i].FormId,
                     movers[i].FormId >= 0xFF000000 ? "temporary" : "static", movers[i].Moved, movers[i].ReferenceMoved, glm::dot(movers[i].Direction, mean),
                     m_everHandled.count(movers[i].FormId) ? ", warped or carried by us before" : "");

    if (m_cellSweepReports == kMaxReports)
        spdlog::warn("Cell drift: {} reports is enough, so the sweep goes quiet until the cell settles or changes", kMaxReports);
}

void ObjectService::MarkDriving(const uint32_t acFormId) noexcept
{
    // Somebody is driving it again, so any motion from here on is theirs and not the drift we are hunting.
    StopWatchingDrift(acFormId);

    m_everHandled.insert(acFormId);

    const auto cNow = std::chrono::steady_clock::now();

    for (DrivenObject& driven : m_driven)
    {
        if (driven.FormId != acFormId)
            continue;

        driven.LastSeen = cNow;
        return;
    }

    m_driven.push_back(DrivenObject{acFormId, cNow});
}

void ObjectService::StopDriving(const uint32_t acFormId) noexcept
{
    for (auto it = m_driven.begin(); it != m_driven.end(); ++it)
    {
        if (it->FormId != acFormId)
            continue;

        m_driven.erase(it);
        return;
    }
}

// Repairs an object whose stream stopped without a release. A warped object left unrepaired is stuck at its
// last network position for good, and the next player to pick it up sees it move in their hand while
// everybody else watches it sit still, because the reference has stopped following the body.
void ObjectService::RunDrivenObjectTimeouts() noexcept
{
    if (m_driven.empty())
        return;

    // Comfortably longer than the 33 ms send interval, so ordinary jitter or a dropped packet does not end a
    // hold that is still going on.
    constexpr auto cSilenceBeforeRepair = 1000ms;

    const auto cNow = std::chrono::steady_clock::now();

    for (size_t i = m_driven.size(); i > 0; --i)
    {
        const DrivenObject& driven = m_driven[i - 1];

        if (cNow - driven.LastSeen < cSilenceBeforeRepair)
            continue;

        if (TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(driven.FormId)))
        {
            RestoreObjectPhysics(pObject);

            spdlog::info("Repaired object {:X} after its stream went quiet without a release", driven.FormId);

            WatchForDrift(driven.FormId, "a silent stream was repaired");
        }

        m_driven.erase(m_driven.begin() + (i - 1));
    }
}

void ObjectService::OnObjectHold(const ObjectHoldEvent& acEvent) noexcept
{
    const size_t cSlot = acEvent.IsLeft ? 1 : 0;

    if (!acEvent.IsReleased)
    {
        m_heldByHand[cSlot] = acEvent.FormId;

        m_everHandled.insert(acEvent.FormId);

        // Picking it up again ends any settling from a previous throw.
        StopSettling(acEvent.FormId);

        // It is in a hand, so of course it is moving. Nothing to judge until it is let go of again.
        StopWatchingDrift(acEvent.FormId);

        // Says whether this hold can be named on the wire at all, which is the first thing to check if one
        // client cannot see what another is carrying. A zero drop id on a temporary reference means it is not
        // in the registry and nothing will be sent.
        spdlog::info("Holding object {:X} ({}), dropId {:X}", acEvent.FormId, acEvent.FormId >= 0xFF000000 ? "temporary" : "static", GetDropId(acEvent.FormId));

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

    // Matches what the settle loop measures against. See RunHeldObjectUpdates.
    if (!ReadNodeTranslate(pObject, settling.LastPosition))
        settling.LastPosition = pObject->position;

    m_settling.push_back(settling);
}

void ObjectService::OnDynamicObjectCreated(const DynamicObjectCreatedEvent& acEvent) noexcept
{
    // Cap the registry. Every drop adds an entry and only a lookup removes a dead one, so a long session
    // spent dropping things nobody ever touches would otherwise grow this without limit. Oldest first,
    // since a dropped item nobody has picked up in hundreds of drops is not going to be missed.
    constexpr size_t kMaxDynamicObjects = 256;

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));
    if (!pObject || !pObject->baseForm)
        return;

    if (m_dynamicObjects.size() >= kMaxDynamicObjects)
        m_dynamicObjects.erase(m_dynamicObjects.begin());

    DynamicObject entry{};
    entry.DropId = acEvent.DropId;
    entry.FormId = acEvent.FormId;
    entry.BaseFormId = pObject->baseForm->formID;

    m_dynamicObjects.push_back(entry);

    spdlog::info("Registered dropped object {:X} (base {:X}) as dropId {:X}", entry.FormId, entry.BaseFormId, entry.DropId);
}

// A stale entry is worse than a missing one, because a recycled form id would sync the wrong object, so
// both lookups verify the reference still exists and still carries the base form it was registered with.
uint64_t ObjectService::GetDropId(const uint32_t acFormId) noexcept
{
    for (auto it = m_dynamicObjects.begin(); it != m_dynamicObjects.end(); ++it)
    {
        if (it->FormId != acFormId)
            continue;

        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(it->FormId));
        if (pObject && pObject->baseForm && pObject->baseForm->formID == it->BaseFormId)
            return it->DropId;

        m_dynamicObjects.erase(it);
        return 0;
    }

    return 0;
}

uint32_t ObjectService::GetDynamicFormId(const uint64_t acDropId) noexcept
{
    for (auto it = m_dynamicObjects.begin(); it != m_dynamicObjects.end(); ++it)
    {
        if (it->DropId != acDropId)
            continue;

        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(it->FormId));
        if (pObject && pObject->baseForm && pObject->baseForm->formID == it->BaseFormId)
            return it->FormId;

        m_dynamicObjects.erase(it);
        return 0;
    }

    return 0;
}

void ObjectService::ForgetDynamicObject(const uint64_t acDropId) noexcept
{
    for (auto it = m_dynamicObjects.begin(); it != m_dynamicObjects.end(); ++it)
    {
        if (it->DropId != acDropId)
            continue;

        m_dynamicObjects.erase(it);
        return;
    }
}

// Somebody took a dropped item into their inventory. Their own world copy is already gone, but everyone else's
// is a separate reference they have no way to identify from an inventory change, so it has to be named by its
// drop id.
void ObjectService::OnObjectPickedUp(const ObjectPickedUpEvent& acEvent) noexcept
{
    // Deliberately not GetDropId, which checks the reference is still alive and carries the base form it was
    // registered with. By the time this runs the pickup has destroyed the object, so that check can never pass
    // and using it silently threw every pickup away.
    //
    // The base form from the event does the same job instead: it is what stops a recycled temporary form id
    // naming a different object, which is the only thing the liveness check was protecting against here.
    uint64_t dropId = 0;
    for (const DynamicObject& entry : m_dynamicObjects)
    {
        if (entry.FormId == acEvent.FormId && entry.BaseFormId == acEvent.BaseFormId)
        {
            dropId = entry.DropId;
            break;
        }
    }

    const uint64_t cDropId = dropId;
    if (!cDropId)
    {
        spdlog::info("Picked up temporary object {:X} (base {:X}) that is not a known drop, nothing to remove elsewhere", acEvent.FormId, acEvent.BaseFormId);
        return;
    }

    // Whatever we were doing with it, stop. The object is leaving the world.
    StopSettling(acEvent.FormId);
    StopDriving(acEvent.FormId);

    for (uint32_t& held : m_heldByHand)
    {
        if (held == acEvent.FormId)
            held = 0;
    }

    if (m_transport.IsConnected())
    {
        RequestObjectRemove request{};
        request.DropId = cDropId;

        // Our own cell, not the object's. The object is already destroyed by the time this runs, so it has no
        // cell left to ask, and we were within arm's reach of it, so ours is the same one.
        //
        // The cell only decides who hears about this. Without one the message would reach nobody, so if it will
        // not resolve the others keep their copy: a leftover item is a much smaller problem than deleting the
        // wrong reference on somebody else's machine.
        TESObjectCELL* pCell = PlayerCharacter::Get()->parentCell;

        if (pCell && m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
            m_transport.Send(request);
        else
            spdlog::warn("Picked up dropped object {:X} but our cell will not resolve, other clients will keep their copy", acEvent.FormId);
    }

    ForgetDynamicObject(cDropId);

    spdlog::info("Picked up dropped object {:X}, dropId {:X}", acEvent.FormId, cDropId);
}

void ObjectService::OnObjectRemoveNotify(const NotifyObjectRemove& acMessage) noexcept
{
    const uint32_t cFormId = GetDynamicFormId(acMessage.DropId);
    if (!cFormId)
        return;

    StopSettling(cFormId);
    StopDriving(cFormId);

    if (TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cFormId)))
    {
        pObject->Delete();

        spdlog::info("Removed dropped object {:X} (dropId {:X}), it was picked up elsewhere", cFormId, acMessage.DropId);
    }

    ForgetDynamicObject(acMessage.DropId);
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

    /**
     * Give up on a throw that will not settle, rather than streaming a cabbage rolling down a hill for
     * ever. The receiver takes over from wherever it had got to, which is a small disagreement at worst.
     *
     * Three seconds was too short, and the log of 2026-08-21 says so with numbers: cabbage 1C0CA was
     * abandoned at the cap and then rolled another 190 units over the following twelve seconds, all of it
     * invisible to the other client. A kicked object rolls for tens of seconds, so the cap has to outlast a
     * roll rather than a fall.
     */
    constexpr double kMaxSettleTime = 20.0;
    /**
     * Below this speed the object counts as still. A handful of consecutive still ticks, so a cabbage
     * pausing at the top of a bounce is not mistaken for one that has landed.
     *
     * A speed rather than a distance per tick, which is what this used to be: 0.5 units at 30 Hz is 15 units
     * a second, a visible slide, and the same log caught the stream ending on an object still doing 16.
     */
    constexpr float kRestSpeed = 2.f;
    constexpr uint32_t kRestTicks = 5;

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

        // The node, not the reference, for the same reason SendObjectTransform reads it: a temporary
        // reference never has its position written back from physics, so a dropped item measured off the
        // reference reads as perfectly still from the moment it is let go and settles in three ticks.
        glm::vec3 position{};
        if (!ReadNodeTranslate(pObject, position))
            position = pObject->position;

        const float cSpeed = glm::length(position - settling.LastPosition) / static_cast<float>(cTickSeconds);
        settling.LastPosition = position;
        settling.Elapsed += cTickSeconds;

        settling.StillTicks = cSpeed < kRestSpeed ? settling.StillTicks + 1 : 0;

        const bool cSettled = settling.StillTicks >= kRestTicks || settling.Elapsed >= kMaxSettleTime;

        // The final message is the only one sent without warp, which is what hands the object back to
        // the receiver's own physics. By now it is not moving, so there is nothing left to disagree on.
        SendObjectTransform(settling.FormId, cSettled);

        if (cSettled)
        {
            m_settling.erase(m_settling.begin() + (i - 1));

            // Our own throw is over as far as the protocol is concerned. Whether the object agrees is the
            // question this answers.
            WatchForDrift(settling.FormId, "our own throw settled");
        }
    }
}

void ObjectService::SendObjectTransform(const uint32_t acFormId, const bool aIsReleased) noexcept
{
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acFormId));
    if (!pObject)
        return;

    RequestObjectTransform request{};

    // Two ways to name the object, and which one applies depends on where it came from. A static world
    // reference resolves identically on every client, so its form id travels directly. An item dropped
    // from an inventory is a temporary whose form id means nothing elsewhere, so it travels as the id of
    // the drop that created it. Anything that is neither cannot be named at all and is not sent.
    const uint64_t cDropId = GetDropId(acFormId);

    if (cDropId)
    {
        // Temporary references do not have their position written back from physics, so pObject->position is
        // whatever it was when the object was created and never changes again. Measured: the item falls and
        // behaves normally on screen while the field stays at chest height for as long as it is held.
        //
        // Sending that constant is worse than sending nothing. The receiver snaps its own copy those few
        // centimetres and warps it, which detaches its body for no benefit at all. So this stays shut until
        // the position comes from the 3D node instead. See kNodeTranslateOffset.
        if (!kNodeTranslateOffset)
            return;

        request.DropId = cDropId;
    }
    else if (pObject->IsTemporary() || !m_world.GetModSystem().GetServerModId(acFormId, request.Id))
    {
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

    // The node in preference to the reference. For a temporary the reference's position is frozen at whatever
    // it was created with, and for a static one the two agree, so the node is right in both cases and is the
    // only thing that works for a dropped item.
    glm::vec3 position{};
    if (!ReadNodeTranslate(pObject, position))
        position = pObject->position;

    request.Position = position;

    // Rotation still comes from the reference, and is stale for a temporary for the same reason the position
    // was. Fixing it means decomposing the node's world matrix at kNodeTranslateOffset - 0x24 into euler
    // angles, which is worth doing once carrying a dropped item is confirmed working.
    request.Rotation = pObject->rotation;
    request.IsReleased = aIsReleased;

    m_transport.Send(request);

    if (aIsReleased)
        spdlog::info("Held object {:X} came to rest at ({:.1f}, {:.1f}, {:.1f})", acFormId, pObject->position.x, pObject->position.y, pObject->position.z);
}

void ObjectService::OnObjectTransformNotify(const NotifyObjectTransform& acMessage) noexcept
{
    // Mirrors the sender: a drop id resolves through the registry to our own copy of that dropped item,
    // anything else is a static reference resolved through ModSystem.
    const uint32_t cObjectId = acMessage.DropId ? GetDynamicFormId(acMessage.DropId) : m_world.GetModSystem().GetGameId(acMessage.Id);

    // Rate limited, because these arrive at 30 Hz and a failure would otherwise flood the log. Silence
    // here used to be indistinguishable from never receiving the message at all, which is exactly the
    // ambiguity that made a missing pickup impossible to diagnose from two logs.
    static std::chrono::steady_clock::time_point lastResolveWarn;

    const auto cWarn = [&](const char* acpWhy)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastResolveWarn < 1000ms)
            return;

        lastResolveWarn = now;
        spdlog::warn("Object transform dropped: {}. dropId {:X}, id {:X}:{:X}", acpWhy, acMessage.DropId, acMessage.Id.ModId, acMessage.Id.BaseId);
    };

    if (cObjectId == 0)
    {
        cWarn(acMessage.DropId ? "no local object registered for this drop id" : "static form id did not resolve");
        return;
    }

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        cWarn("resolved id names no reference");
        return;
    }

    /**
     * An actor must never arrive here. The sender refuses to stream one at all (`HiggsService::IsSyncable`),
     * because a ragdoll is driven from its own rigid body and writing its reference does nothing, so anything
     * that resolves to an actor means the two clients disagree about what the id names: a drop-id pairing
     * that has gone stale, or mod indices that do not line up between the two installs.
     *
     * Refused rather than written, because the write is not harmless. Warping an actor teleports its
     * collision and leaves it detached from it, which is precisely what an NPC sliding slowly through the
     * world looks like. If this line ever appears in a log, it is the whole explanation.
     */
    if (Cast<Actor>(pObject))
    {
        cWarn("resolved id names an actor, which is never something a client streams");
        return;
    }

    /**
     * Two clients driving the same object at the same time, which until now was only ever reasoned about.
     *
     * It is the one shape in this protocol that can move an object without either client meaning to: each
     * side writes what the other last sent, so any systematic bias in the round trip, the integer truncation
     * in `Vector3_NetQuantize` for one, is applied again on every exchange and never corrected. That is a
     * constant slow drift in a fixed direction, and it lasts as long as both sides keep talking.
     *
     * Named rather than counted, because which of the two states we are in decides whether it can loop. A
     * hand holding the object is the dangerous one: settling below is cancelled the moment this message
     * arrives, so it stops on its own, while a hold keeps streaming until the hand opens.
     */
    if (m_heldByHand[0] == cObjectId || m_heldByHand[1] == cObjectId)
    {
        static std::chrono::steady_clock::time_point lastContentionWarn;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastContentionWarn >= 1000ms)
        {
            lastContentionWarn = now;
            spdlog::warn("Object {:X} is being driven by another client while it is in our own hand, so both clients are writing it", cObjectId);
        }
    }
    else if (std::any_of(m_settling.begin(), m_settling.end(), [cObjectId](const SettlingObject& acSettling) { return acSettling.FormId == cObjectId; }))
    {
        spdlog::info("Object {:X} was taken over by another client while our own throw was still settling", cObjectId);
    }

    // Someone else is driving this object now, most likely because they took it out of our hand or off
    // the floor while our throw was still settling. Their stream wins: two clients writing the same
    // object would only fight.
    StopSettling(cObjectId);

    pObject->position = acMessage.Position;
    pObject->SetRotation(acMessage.Rotation.x, acMessage.Rotation.y, acMessage.Rotation.z);

    if (acMessage.IsReleased)
    {
        StopDriving(cObjectId);
        RestoreObjectPhysics(pObject);

        spdlog::info("Remote object {:X} released at ({:.1f}, {:.1f}, {:.1f})", cObjectId, pObject->position.x, pObject->position.y, pObject->position.z);

        WatchForDrift(cObjectId, "a remote release handed it back to us");

        return;
    }

    // Only the warp actually moves the object: it teleports the rigid body to the write. Writing the position
    // without it sets the field and moves nothing, which reads back as a perfect miss of 0.00 and so looks
    // like success while nothing happens on screen. The cost is a detached body, which is what
    // RestoreObjectPhysics above exists to undo.
    pObject->Update3DPosition(true);

    // Remember we are warping this one, so it can be repaired even if the release never arrives. See
    // RunDrivenObjectTimeouts.
    MarkDriving(cObjectId);
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
