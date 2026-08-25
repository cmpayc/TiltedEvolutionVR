#include "Forms/TESObjectCELL.h"
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/TESNPC.h>
#include <Forms/TESQuest.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>

#include <Games/Skyrim/ActorSanity.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/MountEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Events/MoveActorEvent.h>
#include <Events/PartyJoinedEvent.h>

#include <Components/PendingEquipmentComponent.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <DefaultObjectManager.h>
#include <Forms/TESObjectARMO.h>
#include <Structs/ActionEvent.h>
#include <Messages/CancelAssignmentRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyRelinquishControl.h>

#include <World.h>
#include <Games/TES.h>

#if TP_SKYRIMVR
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

namespace
{
bool IsReadable(const void* apPtr, const size_t aSize) noexcept
{
    if (!apPtr)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apPtr, &info, sizeof(info)))
        return false;

    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return false;

    constexpr DWORD cReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & cReadable))
        return false;

    const auto cRegionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

    return reinterpret_cast<uintptr_t>(apPtr) + aSize <= cRegionEnd;
}

/**
 * @brief The decorated type name of a candidate pointer, by reading MSVC's RTTI. Null if it is not an object.
 *
 * Reads only. The previous attempt asked the object for its type by calling vtable slot 2, which crashed the
 * session of 2026-08-23 17:09 with `call rax` on a candidate that was not an NiObject at all: readable memory
 * says nothing about what calling through it will do, and the type cannot be checked by a call that presumes
 * the type. Nothing here transfers control, so the worst a bad candidate costs is a failed check.
 *
 * The complete object locator sits at `vtable[-1]`. On x64 its fields past the signature are image relative,
 * and it stores its own RVA, so subtracting that from where it was found gives the base of whichever module
 * owns it without having to know which one that is. The base is then confirmed by its `MZ`, which makes a
 * false positive essentially impossible.
 */
const char* RttiName(const void* apCandidate) noexcept
{
    if (!IsReadable(apCandidate, sizeof(void*)))
        return nullptr;

    const auto* cpVtable = *reinterpret_cast<const uint8_t* const*>(apCandidate);

    if (!IsReadable(cpVtable - sizeof(void*), sizeof(void*)))
        return nullptr;

    const auto* cpLocator = *reinterpret_cast<const uint8_t* const*>(cpVtable - sizeof(void*));

    // signature, offset, cdOffset, type descriptor rva, class descriptor rva, self rva.
    constexpr size_t kLocatorSize = 0x18;

    if (!IsReadable(cpLocator, kLocatorSize))
        return nullptr;

    const auto cSignature = *reinterpret_cast<const uint32_t*>(cpLocator);
    if (cSignature != 1)
        return nullptr;

    const auto cTypeRva = *reinterpret_cast<const uint32_t*>(cpLocator + 0x0C);
    const auto cSelfRva = *reinterpret_cast<const uint32_t*>(cpLocator + 0x14);

    const auto cBase = reinterpret_cast<uintptr_t>(cpLocator) - cSelfRva;

    if (!IsReadable(reinterpret_cast<const void*>(cBase), 2) || *reinterpret_cast<const uint16_t*>(cBase) != 0x5A4D)
        return nullptr;

    // vftable pointer, spare, then the decorated name.
    constexpr size_t kNameOffset = 0x10;

    const auto* cpDescriptor = reinterpret_cast<const char*>(cBase + cTypeRva);

    if (!IsReadable(cpDescriptor, kNameOffset + 1))
        return nullptr;

    return cpDescriptor + kNameOffset;
}

// A decorated name reads ".?AVbhkCharacterController@@", so the wanted text is looked for inside it rather
// than matched whole. Bounded and checked a byte at a time, because it is still unverified memory.
bool RttiNameContains(const char* acpName, const char* acpWanted) noexcept
{
    constexpr size_t kMaxName = 128;

    for (size_t start = 0; start < kMaxName; ++start)
    {
        if (!IsReadable(acpName + start, 1) || !acpName[start])
            return false;

        size_t i = 0;

        while (acpWanted[i] && IsReadable(acpName + start + i, 1) && acpName[start + i] == acpWanted[i])
            ++i;

        if (!acpWanted[i])
            return true;
    }

    return false;
}

/**
 * @brief Finds where a remote actor keeps its character controller, once, and says so.
 *
 * Step one of taking remote bodies out of collision. `MiddleProcess` does not model the controller on this
 * build and the SE offset cannot be assumed, so this walks the process for a pointer whose RTTI names one.
 * The offset it prints is meant to be read out of a log once and then written down as a constant, the same
 * way `kNodeTranslateOffset` was arrived at.
 */
void SetBodyCollision(Actor* apActor, const bool aEnabled) noexcept
{
    /**
     * Every remote body, every time it is set up.
     *
     * This began as a one shot probe and the guard came with it, which on 2026-08-23 meant the filter was
     * cleared on the first NPC to arrive and on nothing else, the remote player's own body included, while
     * the log showed a successful write and the room went on drifting. A fix that runs once is not a fix.
     *
     * Cheap enough to repeat: a handful of guarded reads, and it returns immediately once the layer is
     * already what it should be, so a 3D rebuild re-applying it costs nothing and says nothing.
     */
    if (!apActor)
        return;

    if (!apActor->currentProcess || !apActor->currentProcess->middleProcess)
    {
        spdlog::warn("Character controller probe: actor {:X} has no middle process, so there is nothing to search", apActor->formID);
        return;
    }

    /**
     * Measured on SkyrimVR 1.4.15 on 2026-08-23 by the scan this replaced: the process holds a
     * `bhkCharRigidBodyController` here, with a `bhkRagdollPenetrationUtil` alongside it at +0x258. The
     * concrete class is not `bhkCharacterController`, which is the abstract base and never appears by name.
     *
     * Verified rather than trusted. If a future build moves it, the name will not match and the scan below
     * says what is there instead, which is how this offset was found in the first place.
     */
    constexpr size_t kCharControllerOffset = 0x250;

    const auto* cpBase = reinterpret_cast<const uint8_t*>(apActor->currentProcess->middleProcess);

    const void* pController = IsReadable(cpBase + kCharControllerOffset, sizeof(void*)) ? *reinterpret_cast<const void* const*>(cpBase + kCharControllerOffset) : nullptr;

    const char* pControllerName = RttiName(pController);

    if (!pControllerName || !RttiNameContains(pControllerName, "CharRigidBodyController"))
    {
        spdlog::warn("Character controller probe: MiddleProcess+{:#X} of actor {:X} is not a controller. Everything named in the first 0x400 bytes follows", kCharControllerOffset, apActor->formID);

        for (size_t offset = 0; offset + sizeof(void*) <= 0x400; offset += sizeof(void*))
        {
            if (!IsReadable(cpBase + offset, sizeof(void*)))
                continue;

            if (const char* pName = RttiName(*reinterpret_cast<const void* const*>(cpBase + offset)))
                spdlog::info("    MiddleProcess+{:#X} is a '{}'", offset, pName);
        }

        return;
    }

    const auto* cpController = reinterpret_cast<const uint8_t*>(pController);


    /**
     * The collision filter, which is where RTTI stops being able to help.
     *
     * Measured on 2026-08-23: the controller points at a `bhkRigidBody` here, with the havok level
     * `ahkpCharacterRigidBody` alongside it at +0x350. Everything past this point is plain integers that no
     * type information can locate, so the offsets below are the standard havok layout rather than anything
     * this found on its own:
     *
     *   bhkRefObject      +0x10  hkReferencedObject* referencedObject, which is the hkpRigidBody
     *   hkpWorldObject    +0x20  hkpLinkedCollidable collidable
     *   hkpCollidable     +0x20  hkpTypedBroadPhaseHandle broadPhaseHandle
     *   broadPhaseHandle  +0x08  uint32 collisionFilterInfo
     *
     * Which puts the filter at hkpRigidBody+0x48. Assumed layouts are how this investigation went wrong
     * repeatedly, so it is checked rather than believed: the low seven bits are the collision layer, and a
     * character's should read 30, `L_CHARCONTROLLER`. Any other value means the arithmetic is wrong and
     * nothing should be written through it.
     */
    constexpr size_t kRigidBodyOffset = 0x360;
    constexpr size_t kReferencedObjectOffset = 0x10;
    constexpr size_t kCollisionFilterOffset = 0x4C;
    constexpr uint32_t kLayerMask = 0x7F;
    constexpr uint32_t kCharControllerLayer = 30;

    if (!IsReadable(cpController + kRigidBodyOffset, sizeof(void*)))
        return;

    const auto* cpRigidBody = *reinterpret_cast<const uint8_t* const*>(cpController + kRigidBodyOffset);

    const char* pRigidBodyName = RttiName(cpRigidBody);

    if (!pRigidBodyName || !RttiNameContains(pRigidBodyName, "bhkRigidBody"))
    {
        spdlog::warn("Collision filter probe: controller+{:#X} is not a bhkRigidBody, so the layout has moved", kRigidBodyOffset);
        return;
    }

    if (!IsReadable(cpRigidBody + kReferencedObjectOffset, sizeof(void*)))
        return;

    const auto* cpHavokBody = *reinterpret_cast<const uint8_t* const*>(cpRigidBody + kReferencedObjectOffset);

    if (!IsReadable(cpHavokBody + kCollisionFilterOffset, sizeof(uint32_t)))
    {
        spdlog::warn("Collision filter probe: nothing readable at hkpRigidBody+{:#X}", kCollisionFilterOffset);
        return;
    }

    const uint32_t cFilter = *reinterpret_cast<const uint32_t*>(cpHavokBody + kCollisionFilterOffset);
    const uint32_t cLayer = cFilter & kLayerMask;

    constexpr uint32_t kNonCollidableLayer = 15;

    const uint32_t cFrom = aEnabled ? kNonCollidableLayer : kCharControllerLayer;
    const uint32_t cTo = aEnabled ? kCharControllerLayer : kNonCollidableLayer;

    // Already in the wanted state, which a 3D rebuild will ask for again. Silent, or every rebuild would log
    // a line saying nothing happened.
    if (cLayer == cTo)
        return;

    if (cLayer != cFrom)
    {
        spdlog::warn("Collision filter: hkpRigidBody+{:#X} of actor {:X} holds {:#010X}, layer {}, and layer {} was expected. Nothing is being written", kCollisionFilterOffset, apActor->formID,
                     cFilter, cLayer, cFrom);
        return;
    }

    /**
     * The change itself: this body stops colliding with anything.
     *
     * Only the layer is touched. The rest of the word is a collision group and whatever else the filter
     * encodes, and none of it is ours to reinterpret, so it is preserved exactly and the seven layer bits are
     * replaced. Measured 0x0780001E on 2026-08-23, layer 30, which is how the offset was confirmed at all.
     *
     * Standing on the floor is not lost by this, because a remote body does not stand: InterpolationSystem
     * forces its position every frame from the owner's stream, and always did. What it does lose is pushing
     * clutter about, which is the entire point, and being hit by a thrown object, which is the price agreed
     * for it.
     *
     * Whether to clear it at all is the server's `bEnableRemoteBodyCollision`, decided by the callers. Putting
     * it back when a body becomes ours is not subject to that switch and never should be, or an actor handed
     * over keeps the ghost collision it was given while it was somebody else's.
     */
    const uint32_t cWanted = (cFilter & ~kLayerMask) | cTo;

    auto* pFilter = const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(cpHavokBody + kCollisionFilterOffset));

    *pFilter = cWanted;

    /**
     * Read back rather than assumed written. This is havok's own memory and something else may own the word,
     * in which case it will not stay and saying so is better than reporting a change that did not happen.
     */
    const uint32_t cAfter = *pFilter;

    spdlog::info("Collision filter: actor {:X} moved from layer {} to {}, filter {:#010X} -> {:#010X}{}", apActor->formID, cLayer, aEnabled ? "L_CHARCONTROLLER" : "L_NONCOLLIDABLE", cFilter, cAfter,
                 cAfter == cWanted ? "" : " (it did not hold, so something else owns this word)");
}
} // namespace
#endif

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_referenceAddedConnection = m_dispatcher.sink<ActorAddedEvent>().connect<&CharacterService::OnActorAdded>(this);
    m_referenceRemovedConnection = m_dispatcher.sink<ActorRemovedEvent>().connect<&CharacterService::OnActorRemoved>(this);

    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this);
    m_actionConnection = m_dispatcher.sink<ActionEvent>().connect<&CharacterService::OnActionEvent>(this);

    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&CharacterService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&CharacterService::OnDisconnected>(this);

    m_assignCharacterConnection = m_dispatcher.sink<AssignCharacterResponse>().connect<&CharacterService::OnAssignCharacter>(this);
    m_characterSpawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&CharacterService::OnCharacterSpawn>(this);
    m_referenceMovementSnapshotConnection = m_dispatcher.sink<ServerReferencesMoveRequest>().connect<&CharacterService::OnReferencesMoveRequest>(this);
    m_factionsConnection = m_dispatcher.sink<NotifyFactionsChanges>().connect<&CharacterService::OnFactionsChanges>(this);
    m_ownershipTransferConnection = m_dispatcher.sink<NotifyOwnershipTransfer>().connect<&CharacterService::OnOwnershipTransfer>(this);
    m_removeCharacterConnection = m_dispatcher.sink<NotifyRemoveCharacter>().connect<&CharacterService::OnRemoveCharacter>(this);
    m_remoteSpawnDataReceivedConnection = m_dispatcher.sink<NotifySpawnData>().connect<&CharacterService::OnRemoteSpawnDataReceived>(this);

    m_mountConnection = m_dispatcher.sink<MountEvent>().connect<&CharacterService::OnMountEvent>(this);
    m_notifyMountConnection = m_dispatcher.sink<NotifyMount>().connect<&CharacterService::OnNotifyMount>(this);

    m_initPackageConnection = m_dispatcher.sink<InitPackageEvent>().connect<&CharacterService::OnInitPackageEvent>(this);
    m_newPackageConnection = m_dispatcher.sink<NotifyNewPackage>().connect<&CharacterService::OnNotifyNewPackage>(this);

    m_notifyRespawnConnection = m_dispatcher.sink<NotifyRespawn>().connect<&CharacterService::OnNotifyRespawn>(this);
    m_beastFormChangeConnection = m_dispatcher.sink<BeastFormChangeEvent>().connect<&CharacterService::OnBeastFormChange>(this);

    m_addExperienceEventConnection = m_dispatcher.sink<AddExperienceEvent>().connect<&CharacterService::OnAddExperienceEvent>(this);
    m_syncExperienceConnection = m_dispatcher.sink<NotifySyncExperience>().connect<&CharacterService::OnNotifySyncExperience>(this);

    m_dialogueEventConnection = m_dispatcher.sink<DialogueEvent>().connect<&CharacterService::OnDialogueEvent>(this);
    m_dialogueSyncConnection = m_dispatcher.sink<NotifyDialogue>().connect<&CharacterService::OnNotifyDialogue>(this);

    m_subtitleEventConnection = m_dispatcher.sink<SubtitleEvent>().connect<&CharacterService::OnSubtitleEvent>(this);
    m_subtitleSyncConnection = m_dispatcher.sink<NotifySubtitle>().connect<&CharacterService::OnNotifySubtitle>(this);

    m_actorTeleportConnection = m_dispatcher.sink<NotifyActorTeleport>().connect<&CharacterService::OnNotifyActorTeleport>(this);

    m_relinquishConnection = m_dispatcher.sink<NotifyRelinquishControl>().connect<&CharacterService::OnNotifyRelinquishControl>(this);

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D, PlayerComponent>(aEntity);
}

bool CharacterService::TakeOwnership(const uint32_t acFormId, const uint32_t acServerId, const entt::entity acEntity) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acFormId));
    if (!pActor)
    {
        spdlog::error("Cannot find actor to take control over, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::error("Cannot take control over remote player actor, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::error("Cannot take control over remote player summon, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

#if TP_SKYRIMVR
    // It is ours again, so it collides again. Without this a body handed over keeps the ghost collision it was
    // given while it was somebody else's, for the rest of the session.
    SetBodyCollision(pActor, true);
#endif

    pExtension->SetRemote(false);

    // TODO(cosideci): this should be done differently.
    // Send an ownership claim request, and have the server broadcast the result.
    // Only then should components be added or removed.
    m_world.emplace_or_replace<LocalComponent>(acEntity, acServerId);
    m_world.emplace_or_replace<LocalAnimationComponent>(acEntity);
    DeleteRemoteEntityComponents(acEntity);

    RequestOwnershipClaim request;
    request.ServerId = acServerId;
    request.NewActorData = BuildActorData(pActor);

    m_transport.Send(request);

    return true;
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (pActor && ((pActor->formID & 0xFF000000) == 0xFF000000))
    {
        pActor->Delete();
        spdlog::info("\tDeleted actor {:X}", aFormId);
    }
}

void CharacterService::OnActorAdded(const ActorAddedEvent& acEvent) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));

    if (acEvent.FormId == 0x14)
    {
        pActor->GetExtension()->SetPlayer(true);
    }

    entt::entity entity;

    const auto view = m_world.view<RemoteComponent>();
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            return remoteComponent.CachedRefId == acEvent.FormId;
        });

    if (it != std::end(view))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
        pActor->GetExtension()->SetRemote(true);

        entity = *it;
    }
    else
        entity = m_world.create();

    m_world.emplace_or_replace<FormIdComponent>(entity, acEvent.FormId);
    m_world.emplace_or_replace<EarlyAnimationBufferComponent>(entity);

    /**
     * Weapon state has to be restored from here rather than from ProcessNewEntity.
     *
     * ProcessNewEntity is const and m_weaponDrawUpdates is not, and re-queueing is the point: applying the
     * flag once does not reliably take, which is why ApplyCachedWeaponDraws retries at half a second and again
     * at two. So the state is read before ProcessNewEntity restores the inventory and drops the component.
     */
    const auto* pPending = m_world.try_get<PendingEquipmentComponent>(entity);
    const bool cHadPending = pPending != nullptr;
    const bool cWeaponWasDrawn = cHadPending && pPending->WeaponDrawn;
    const bool cWasSneaking = cHadPending && pPending->Sneaking;

    ProcessNewEntity(entity);

    if (!cHadPending)
        return;

    /**
     * The weapon state captured at sight-loss is deliberately NOT restored.
     *
     * It is only what the body happened to show at that moment, and the server's value in the spawn message is
     * authority. Re-queueing the captured one clobbers it: on 2026-08-19 the server said drawn, the relocation
     * branch queued that correctly, and this line then overwrote it with the stale false a few seconds later,
     * so the weapon stayed sheathed. It is still read below, purely to report what was there.
     */

    // Sneak is not restored either: it has no wire field and reaches a remote body as animation graph
    // variables, so this says whether ActorState's own bit survived the rebuild.
    Actor* pReturned = Cast<Actor>(TESForm::GetById(acEvent.FormId));

    if (!pReturned)
        return;

    spdlog::info("Remote player body {:X} came back: weapon drawn was {}, actual now {}. Sneaking was {}, actual now {}.", acEvent.FormId, cWeaponWasDrawn, pReturned->actorState.IsWeaponDrawn(), cWasSneaking, pReturned->actorState.IsSneaking());

    /**
     * Always queue a weapon state here, even when it already looks right.
     *
     * This was unconditional through every session where remote players reappeared reliably, and dropping it
     * on 2026-08-19 at 23:56 is the one change from that build still in place while they stopped reappearing.
     * The reverted GetNiNode guard from the same build did not account for it. So whatever
     * ApplyCachedWeaponDraws does to a body through SetWeaponDrawnEx, it is evidently doing more than setting
     * a flag, and the body depends on it. Restored deliberately rather than reasoned away.
     *
     * The server's value wins over the one captured at sight-loss, which is what stops this reintroducing the
     * clobbering it used to cause.
     */
    const auto cDesired = m_desiredWeaponDrawn.find(acEvent.FormId);
    const bool cWanted = cDesired != m_desiredWeaponDrawn.end() ? cDesired->second : cWeaponWasDrawn;

    if (cDesired != m_desiredWeaponDrawn.end())
    {
        spdlog::info("Re-applying the server's weapon drawn {} to body {:X} now that it is back", cWanted, acEvent.FormId);

        m_desiredWeaponDrawn.erase(cDesired);
    }

    m_weaponDrawUpdates[acEvent.FormId] = {cWanted};
}

void CharacterService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    auto view = m_world.view<FormIdComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.FormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to remove not found in form ids map {:X}", acEvent.FormId);
        return;
    }

    const auto cId = *entityIt;

    /**
     * A remote player's body is not ours to remove, however thoroughly we have lost sight of it.
     *
     * This is driven by DiscoveryService noticing a form missing from the process lists, which is a statement
     * about what is loaded here and not about whether that player still exists. The server is the authority on
     * that and says so with NotifyRemoveCharacter. Acting on local sight alone deleted the body, took the
     * mapping with it, and left the player invisible until the server happened to send another spawn: 29
     * seconds on 2026-08-19 at 22:01, and again at 22:17 after a MoveTo rebuilt the body's 3D and took it out
     * of the lists for longer than the removal grace period covers.
     *
     * Returning early is what keeps the entity whole. Stripping FormIdComponent below would break the server
     * id to form id mapping just as effectively as deleting the actor, and the next spawn request would then
     * have to build a second body from scratch.
     *
     * The trade is a body that leaks if the server never sends a removal, seen as another player's character
     * standing about doing nothing. That is the better of the two failures.
     */
    /**
     * States every fact the branch below depends on, for dynamic forms only.
     *
     * The keep-path has now failed to fire three times for a remote player's body and each diagnosis was an
     * inference from its absence, which cannot distinguish "the form no longer resolves" from "the entity has
     * no RemoteComponent" from "it is not temporary". One line settles it.
     */
    if ((acEvent.FormId & 0xFF000000) == 0xFF000000)
    {
        Actor* pProbe = Cast<Actor>(TESForm::GetById(acEvent.FormId));

        spdlog::info("Removal probe {:X}: form resolves {}, RemoteComponent {}, temporary {}, has 3D {}, extension {}", acEvent.FormId, pProbe != nullptr, m_world.all_of<RemoteComponent>(cId), pProbe && pProbe->IsTemporary(), pProbe && pProbe->GetNiNode() != nullptr, pProbe && pProbe->GetExtension() != nullptr);
    }

    /**
     * A locally owned temporary is not ours to give away, for the same reason a remote body is not ours to
     * delete: the sweep that reported it missing only ever saw that it had no 3D, and that is what a rebuild
     * looks like.
     *
     * Being wrong costs more on this side than on the remote one. Removing a remote body loses it until the
     * next spawn, while removing a local one runs CancelServerAssignment, which hands the actor to another
     * client and deletes our copy, so it never comes back. On 2026-08-22 three thugs were spawned at
     * 18:50:30.023 and given away 194ms later, and the other player fought them alone while every message
     * about them logged "could not find actor server id".
     *
     * The form still resolving is what makes this safe to skip. Once the game really does delete the
     * temporary, the probe above finds nothing, this does not fire, and removal runs as it did before.
     */
    if (m_world.all_of<LocalComponent>(cId))
    {
        if (const Actor* pLocal = Cast<Actor>(TESForm::GetById(acEvent.FormId)); pLocal && pLocal->IsTemporary())
        {
            spdlog::info("Lost sight of local actor {:X}, keeping it. Its form still resolves, so it has not gone anywhere and its ownership stays here.", acEvent.FormId);

            return;
        }
    }

    if (Actor* pRemotePlayer = Cast<Actor>(TESForm::GetById(acEvent.FormId)); pRemotePlayer)
    {
        /**
         * @brief Identified by its components, not by the extension's player flag.
         *
         * IsRemotePlayer() was the obvious test and it is not reliable here. The flag is only set by
         * SetPlayer(acMessage.IsPlayer) in the path that applies 3D, and that path does not always run: on
         * 2026-08-20 body FF000870 was spawned and reached "New entity remotely managed" with no "Applied 3D"
         * line at all, so the flag was never set, this branch was skipped, and the body was deleted exactly as
         * before. The player stayed invisible until they re-entered the cave.
         *
         * RemoteComponent is present by definition, since ProcessNewEntity logged the body as remotely
         * managed, and IsTemporary is the very condition CancelServerAssignment uses to decide to delete. So
         * this intercepts precisely the case that does the damage, using state that is always there.
         *
         * A player's summon also matches, and keeping one of those alive costs a body that lingers rather than
         * a player nobody can see.
         */
        const bool cIsManagedRemoteBody = m_world.all_of<RemoteComponent>(cId) && pRemotePlayer->IsTemporary();

        if (cIsManagedRemoteBody)
        {
            /**
             * The face has to be regenerated once the body's 3D comes back.
             *
             * FaceGenSystem::Update latches on FaceGenComponent::Generated and never runs a second time, so
             * rebuilt head geometry keeps the shader property it was born with, which carries no tint texture
             * and renders black. Deleting and respawning the body used to reset that by building a new
             * component; keeping the body does not, which is why a player returned with a black head on
             * 2026-08-19. FaceTints stay in the component, so clearing the latch is all that is needed.
             */
            if (auto* pFaceGen = m_world.try_get<FaceGenComponent>(cId))
                pFaceGen->Generated = false;

            // Equipment does not survive the rebuild either, so it is captured here and put back when the body
            // reappears. See PendingEquipmentComponent for the measurements behind that.
            const Inventory cCarried = pRemotePlayer->GetActorInventory();
            const size_t cWorn = pRemotePlayer->GetWornArmor().Entries.size();
            const bool cWeaponDrawn = pRemotePlayer->actorState.IsWeaponDrawn();
            const bool cSneaking = pRemotePlayer->actorState.IsSneaking();

            m_world.emplace_or_replace<PendingEquipmentComponent>(cId, cCarried, cWeaponDrawn, cSneaking);

            spdlog::info("Lost sight of remote player body {:X}, keeping it. Queued its face for regeneration and saved {} items ({} worn), weapon drawn {}, sneaking {}. Only the server removes a player.", acEvent.FormId, cCarried.Entries.size(), cWorn, cWeaponDrawn, cSneaking);

            return;
        }
    }

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    spdlog::info("Actor removed, form id: {:X}", acEvent.FormId);
}

void CharacterService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunSpawnUpdates();
    RunLocalUpdates();
    RunFactionsUpdates();
    RunRemoteUpdates();
    RunExperienceUpdates();
    ApplyCachedWeaponDraws(acUpdateEvent);
    RunOffHandWeaponUpdates();
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept
{
    // Go through all the forms that were previously detected
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);
        // Delete all temporary actors on connect
        if (formIdComponent.Id > 0xFF000000)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor)
            {
                spdlog::info("Deleting temporary actor on connect: {:X}", pActor->formID);
                pActor->Delete();
            }

            continue;
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) const noexcept
{
    auto remoteView = m_world.view<FormIdComponent, RemoteComponent>();
    for (auto entity : remoteView)
    {
        auto& formIdComponent = remoteView.get<FormIdComponent>(entity);

        auto pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        if (pActor->GetExtension()->IsRemotePlayer())
        {
            spdlog::info("Deleting remote player actor on disconnect: {:X}", pActor->formID);
            pActor->Delete();
        }
        else
            pActor->GetExtension()->SetRemote(false);
    }

    m_world.clear<WaitingForAssignmentComponent, LocalComponent, RemoteComponent>();
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    spdlog::info("Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [view, cookie = acMessage.Cookie](auto entity) { return view.get<WaitingForAssignmentComponent>(entity).Cookie == cookie; });

    if (itor == std::end(view))
    {
        spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto cEntity = *itor;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        spdlog::error(__FUNCTION__ ": form id component doesn't exist, cookie: {:X}", acMessage.Cookie);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": actor not found, form id: {:X}", formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    if (acMessage.Owner)
    {
        spdlog::info("Received local actor, form id: {:X}", pActor->formID);

        m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId);
        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        pActor->GetExtension()->SetRemote(false);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
    }
    else
    {
        spdlog::info("Received remote actor, form id: {:X}, isweapondrawn: {}", pActor->formID, acMessage.IsWeaponDrawn);

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id);

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        InterpolationSystem::Setup(m_world, cEntity);
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        pActor->SetActorValues(acMessage.AllActorValues);
        ValidateActor(pActor, "before SetActorInventory");
        pActor->SetActorInventory(acMessage.CurrentInventory);
        ValidateActor(pActor, "after SetActorInventory");

        if (pActor->IsDead() != acMessage.IsDead)
            acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

        m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};

        MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
    }
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) noexcept
{
    /**
     * A body somebody else is carrying is not spawned here when dead body sync is off. See
     * RequestServerAssignment for the outgoing half.
     *
     * The message's own flags decide it rather than anything about the form, since there is no local actor to
     * ask yet. A dead player's body still arrives: IsPlayer wins over IsDead.
     */
    if (!m_world.GetServerSettings().DeadBodySyncEnabled && acMessage.IsDead && !acMessage.IsPlayer)
        return;

    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteItor != std::end(remoteView))
    {
        /**
         * Already have a body for this character, so act on the position the request carries.
         *
         * The server sends a spawn request when a character comes into view, which for a player crossing a
         * load door means our body for them is still standing in the cell we just left. Simply returning left
         * it there: on 2026-08-19 a player walked into a dungeon, this refused three times over 21 seconds
         * while their body sat outside, and they stayed invisible until they walked out and back in, which
         * finally produced a spawn request the client would act on. The other direction worked the whole time,
         * which is what made it look asymmetric.
         *
         * MoveTo takes the local player's cell, the same as the fresh spawn path below, so this pulls the
         * existing body through the door instead of building a second one.
         */
        const entt::entity cEntity = *remoteItor;

        Actor* pExisting = nullptr;

        if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity))
            pExisting = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));

        if (pExisting)
        {
            /**
             * Only pull the body across when it is not already in the local player's cell.
             *
             * The server sends a full spawn message on every cell change the character makes, which in an
             * exterior is constantly: ten arrived in fourteen seconds on 2026-08-19 from one player walking
             * about. MoveTo is not cheap, it detaches and reattaches 3D, and doing that repeatedly to a remote
             * body is how 3D gets torn down underneath something else that is still reading it.
             *
             * Drift within the cell needs no help here: movement sync corrects a remote actor's position every
             * frame. The only thing this call is needed for is the case interpolation cannot fix, which is the
             * body being in the wrong cell entirely, and a cell compare is enough to spot it.
             */
            TESObjectCELL* const cpPlayerCell = PlayerCharacter::Get()->parentCell;

            /**
             * A temporary body cannot be moved between cells. It has to be rebuilt.
             *
             * MoveTo destroys it. That is measured, not inferred: the removal probe on 2026-08-20 caught
             * FF000870 seventy milliseconds after a MoveTo reporting "form resolves false", so the reference
             * was already gone. Every attempt to protect the body downstream of that was doomed, because there
             * was nothing left to protect, and that is why remote players stopped reappearing.
             *
             * The message being handled is a complete spawn request: appearance, inventory, face tints, weapon
             * state and position. So the honest response to a stale body is to throw it away and build a new
             * one from this, which is the path that reliably produces a correct body anyway.
             *
             * Persistent references are not affected and are still moved, since the game keeps those.
             */
            if (pExisting->parentCell != cpPlayerCell)
            {
                if (pExisting->IsTemporary())
                {
                    spdlog::info("Remote id {:X} body {:X} is in another cell and is temporary, so it is being rebuilt from this request rather than moved", acMessage.ServerId, pExisting->formID);

                    DeleteTempActor(pExisting->formID);
                    DeleteRemoteEntityComponents(cEntity);

                    pExisting = nullptr;
                }
                else
                {
                    spdlog::info("Character with remote id {:X} is already spawned but in another cell, moving its body to the requested position", acMessage.ServerId);

                    pExisting->rotation.x = acMessage.Rotation.x;
                    pExisting->rotation.z = acMessage.Rotation.y;
                    pExisting->MoveTo(cpPlayerCell, acMessage.Position);

                    /**
                     * MoveTo detaches and reattaches the 3D, so anything in a hand was just placed again.
                     *
                     * A shield or a sheathed weapon is given its node once, when it is equipped, from the
                     * actor's weapon flag as it reads at that instant, and nothing moves it afterwards on a
                     * body that never plays the sheathe animation. So a body pulled through a load door comes
                     * out holding its shield in its hand, and the weapon state agreeing, which it usually does,
                     * is exactly the case that used to queue nothing at all.
                     *
                     * Only here, not on every spawn request. The server sends one on every cell change a
                     * character makes, ten in fourteen seconds in an exterior, and the reseat strips and
                     * restores the body's worn armor. Firing it whenever a request happened to arrive would
                     * flicker every remote player continuously; firing it where the 3D was actually rebuilt
                     * costs one repair per load door.
                     */
                    QueueWeaponDrawUpdate(pExisting->formID, acMessage.IsWeaponDrawn);
                }
            }

            /**
             * Take the weapon state from the message even when nothing else needed doing.
             *
             * This is the only recurring chance to re-sync it. Weapon state reaches us at spawn or assignment,
             * or through DrawWeaponRequest, which the owning client sends only when the state *changes*. So a
             * player who drew their weapon while we did not have their body stays sheathed to us for ever: they
             * will not send again, and we discarded this field on every cell change by returning early here.
             * That is why the body was already wrong before any rebuild, which the state captured at sight-loss
             * on 2026-08-19 showed as weapon drawn false while the player was holding one.
             *
             * Queued rather than set, because a single application does not reliably take. See
             * ApplyCachedWeaponDraws.
             */
            if (pExisting && pExisting->actorState.IsWeaponDrawn() != acMessage.IsWeaponDrawn)
            {
                spdlog::info("Remote id {:X} weapon drawn is {} here but {} on the server, re-queueing it", acMessage.ServerId, pExisting->actorState.IsWeaponDrawn(), acMessage.IsWeaponDrawn);

                QueueWeaponDrawUpdate(pExisting->formID, acMessage.IsWeaponDrawn);

                // Remembered as well, because the queue above will very likely spend both its attempts before
                // the body has the 3D to take them. See m_desiredWeaponDrawn.
                m_desiredWeaponDrawn[pExisting->formID] = acMessage.IsWeaponDrawn;
            }

            // Only keep the existing body if it is still there. A temporary one in the wrong cell was just
            // torn down above, and the full spawn below rebuilds it from this message.
            if (pExisting)
                return;
        }

        // The entity outlived its actor, so the components are stale and every future request for this
        // character would be refused until something else happened to clean them up. Drop them and fall
        // through to spawn a fresh body.
        spdlog::warn("Character with remote id {:X} has an entity but no actor, so it is being respawned.", acMessage.ServerId);

        DeleteRemoteEntityComponents(cEntity);
    }

    Actor* pActor = nullptr;

    std::optional<entt::entity> entity;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            const auto cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

        auto waitingView = m_world.view<FormIdComponent, WaitingForAssignmentComponent>();
        const auto waitingItor = std::find_if(std::begin(waitingView), std::end(waitingView), [waitingView, cActorId](auto entity) { return waitingView.get<FormIdComponent>(entity).Id == cActorId; });

        if (waitingItor != std::end(waitingView))
        {
            spdlog::info("Character with form id {:X} already has a spawn request in progress.", cActorId);
            return;
        }

        auto* const pForm = TESForm::GetById(cActorId);
        pActor = Cast<Actor>(pForm);

        if (!pActor)
        {
            spdlog::error("Failed to retrieve Actor {:X}, it will not be spawned, possibly missing mod", cActorId);
            spdlog::error("\tForm : {:X}", pForm ? pForm->formID : 0);
            return;
        }

        const auto view = m_world.view<FormIdComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [cActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == cActorId; });

        if (itor != std::end(view))
            entity = *itor;
        else
            entity = m_world.create();
    }

    if (!pActor)
    {
        spdlog::error("Actor object {:X} could not be created.", acMessage.ServerId);
        return;
    }

    spdlog::info("CharacterSpawnRequest, server id: {:X}, form id: {:X}", acMessage.ServerId, pActor->formID);

    /**
     * @brief What the request carries, since the rebuild path depends entirely on it being complete.
     *
     * A body in another cell is thrown away and rebuilt from this message rather than moved, on the grounds
     * that the message holds appearance, inventory, face tints, weapon state and position, and so reliably
     * produces a correct body. A player reported a rebuilt body arriving naked with a black head, which is
     * either that premise failing or the application of it failing, and nothing logged so far tells the two
     * apart: the worn count in ProcessNewEntity is read before the inventory is applied, and an empty tint set
     * makes FaceGenSystem::Setup return without creating a component at all, silently.
     *
     * So this says what arrived, and the line at the end of the 3D path says what came of it.
     */
    const size_t cWornInRequest = std::count_if(acMessage.InventoryContent.Entries.begin(), acMessage.InventoryContent.Entries.end(),
                                                [](const Inventory::Entry& acEntry) { return acEntry.IsWorn(); });

    spdlog::info("\tit carries {} items ({} worn), {} face tints, weapon drawn {}, dead {}", acMessage.InventoryContent.Entries.size(), cWornInRequest,
                 acMessage.FaceTints.Entries.size(), acMessage.IsWeaponDrawn, acMessage.IsDead);

    /**
     * @brief Re-enables the actor a spawn request names, if the game has disabled it.
     *
     * Here because a player could otherwise vanish from another player's screen and never come back.
     *
     * Turned off on 2026-08-18 to test whether it was behind that day's crashes, and restored the same evening
     * because a player disappeared within one session of it being off. So it is load bearing, but it is still a
     * suspect: an actor the game disabled has usually been torn down, and resurrecting it and then applying 3D
     * and inventory leaves the game holding structures it thinks are gone. Two crashes that day landed in the
     * same chain, AI package procedure into TESConditionItem::IsTrue into an actor value read, on an actor with
     * one pointer field overwritten and the rest stale but plausible, which is what a freed and partly reused
     * block looks like.
     *
     * Note that the session where it was off shows the disappearing arriving by a different route than this
     * one: DiscoveryService lost sight of the remote player for a frame, ActorRemovedEvent fired, and
     * CancelServerAssignment deleted the actor outright because a remote player's body is a temporary form. So
     * this is not the only way a player vanishes, and probably not the main one.
     */
    if (pActor->IsDisabled())
    {
        spdlog::warn("Disabled actor is being re-enabled: {:X}", pActor->formID);
        pActor->EnableImpl();
    }

    pActor->GetExtension()->SetRemote(true);

    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(*entity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    if (acMessage.IsPlayerSummon)
    {
        // Prevents remote summons agroing other players.
        pActor->SetCommandingActor(PlayerCharacter::Get()->GetHandle());
    }

    auto& remoteComponent = m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID);

    auto& interpolationComponent = InterpolationSystem::Setup(m_world, *entity);
    interpolationComponent.Position = acMessage.Position;

    AnimationSystem::Setup(m_world, *entity);

    m_world.emplace_or_replace<WaitingFor3D>(*entity, acMessage);

    auto& remoteAnimationComponent = m_world.get<RemoteAnimationComponent>(*entity);

    AnimationSystem::AddActionsForReplay(remoteAnimationComponent, acMessage.ActionsToReplay);

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(*entity, acMessage.ActionsToReplay);
#endif
}

void CharacterService::OnRemoteSpawnDataReceived(const NotifySpawnData& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto itor = std::find_if(
        std::begin(view), std::end(view),
        [view, id = acMessage.Id](auto entity)
        {
            if (auto serverId = Utils::GetServerId(entity))
            {
                if (*serverId == id)
                    return true;
            }
            return false;
        });

    if (itor == std::end(view))
        return;

    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*itor))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acMessage.NewActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acMessage.NewActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
    }

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    pActor->SetActorValues(acMessage.NewActorData.InitialActorValues);
    ValidateActor(pActor, "before SetActorInventory");
    pActor->SetActorInventory(acMessage.NewActorData.InitialInventory);
    ValidateActor(pActor, "after SetActorInventory");
    m_weaponDrawUpdates[pActor->formID] = {acMessage.NewActorData.IsWeaponDrawn};

    if (pActor->IsDead() != acMessage.NewActorData.IsDead)
        acMessage.NewActorData.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Applied remote spawn data, actor form id: {:X}", pActor->formID);
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        auto itor = std::find_if(std::begin(view), std::end(view), [serverId = serverId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == serverId; });

        if (itor == std::end(view))
            continue;

        auto& interpolationComponent = view.get<InterpolationComponent>(*itor);
        auto& animationComponent = view.get<RemoteAnimationComponent>(*itor);
        const auto& movement = update.UpdatedMovement;

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;

        InterpolationSystem::AddPoint(interpolationComponent, point);

        for (const auto& action : update.ActionEvents)
        {
            animationComponent.TimePoints.push_back(action);
        }
    }
}

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    auto view = m_world.view<LocalAnimationComponent, FormIdComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        auto& localComponent = view.get<LocalAnimationComponent>(*itor);

        localComponent.Append(acActionEvent);
    }
    else if (m_transport.IsOnline())
    {
        // A `LocalAnimationComponent` is not attached yet, but the actor already exists and is running animations

        auto view = m_world.view<FormIdComponent, EarlyAnimationBufferComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            view.get<EarlyAnimationBufferComponent>(*itor).Actions.push_back(acActionEvent);
        }
    }
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent, CacheComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            auto& formIdComponent = view.get<FormIdComponent>(*itor);

            auto* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
                return;

            auto& cacheComponent = view.get<CacheComponent>(*itor);
            cacheComponent.FactionsContent = factions;

            pActor->SetFactions(cacheComponent.FactionsContent);
        }
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept
{
    // TODO(cosideci): handle case if no one has it, therefore no RemoteComponent
    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [&acMessage, &view](auto entity) { return view.get<RemoteComponent>(entity).Id == acMessage.ServerId; });

    if (itor != std::end(view))
    {
        auto& formIdComponent = view.get<FormIdComponent>(*itor);

        if (TakeOwnership(formIdComponent.Id, acMessage.ServerId, *itor))
        {
            spdlog::info("Ownership claimed {:X}", acMessage.ServerId);
            return;
        }
    }

    spdlog::warn("Actor for ownership transfer not found {:X}", acMessage.ServerId);

    RequestOwnershipTransfer request{};
    request.ServerId = acMessage.ServerId;

    m_transport.Send(request);
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acMessage.ServerId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor))
            CharacterService::DeleteTempActor(pFormIdComponent->Id);

        DeleteRemoteEntityComponents(*itor);
    }
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, id = acMessage.ActorId](auto aEntity) { return view.get<RemoteComponent>(aEntity).Id == id; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to respawn not found in: {:X}", acMessage.ActorId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;

    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*it);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();

    m_transport.Send(request);
}

void CharacterService::OnMountEvent(const MountEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.RiderID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (riderIt == std::end(view))
    {
        spdlog::warn("Rider not found, form id: {:X}", acEvent.RiderID);
        return;
    }

    const entt::entity cRiderEntity = *riderIt;

    std::optional<uint32_t> riderServerIdRes = Utils::GetServerId(cRiderEntity);
    if (!riderServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    const auto mountIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.MountID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (mountIt == std::end(view))
    {
        spdlog::warn("Mount not found, form id: {:X}", acEvent.MountID);
        return;
    }

    const entt::entity cMountEntity = *mountIt;

    std::optional<uint32_t> mountServerIdRes = Utils::GetServerId(cMountEntity);
    if (!mountServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    if (m_world.try_get<RemoteComponent>(cMountEntity))
        TakeOwnership(acEvent.MountID, *mountServerIdRes, cMountEntity);

    MountRequest request;
    request.MountId = mountServerIdRes.value();
    request.RiderId = riderServerIdRes.value();

    m_transport.Send(request);
}

void CharacterService::OnNotifyMount(const NotifyMount& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.RiderId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (riderIt == std::end(remoteView))
    {
        spdlog::warn("Rider with remote id {:X} not found.", acMessage.RiderId);
        return;
    }

    auto& riderFormIdComponent = remoteView.get<FormIdComponent>(*riderIt);
    TESForm* pRiderForm = TESForm::GetById(riderFormIdComponent.Id);
    Actor* pRider = Cast<Actor>(pRiderForm);

    Actor* pMount = nullptr;

    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): remove this, cause of NotifyRelinquishControl?
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error("{}: failed to find server id", __FUNCTION__);
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.MountId)
        {
            auto& mountFormIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.MountId, mountFormIdComponent.Id);
            }

            TESForm* pMountForm = TESForm::GetById(mountFormIdComponent.Id);
            pMount = Cast<Actor>(pMountForm);
            pMount->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            break;
        }
    }

    pRider->InitiateMountPackage(pMount);
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto actorIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.ActorId, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (actorIt == std::end(view))
        return;

    const entt::entity cActorEntity = *actorIt;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ActorId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);

    pActor->SetPackage(pPackage);
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.ActorID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.ActorID);
        return;
    }

    DialogueRequest request{};
    request.ServerId = serverIdRes.value();
    request.SoundFilename = acEvent.VoiceFile;

    m_transport.Send(request);
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for dialogue with remote id {:X} not found.", acMessage.ServerId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);
    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    if (!pActor)
        return;

    pActor->StopCurrentDialogue(true);
    pActor->SpeakSound(acMessage.SoundFilename.c_str());
}

void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.SpeakerID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.SpeakerID);
        return;
    }

    SubtitleRequest request{};
    request.ServerId = serverIdRes.value();
    request.Text = acEvent.Text;
    request.TopicFormId = acEvent.TopicFormID;

    m_transport.Send(request);
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for dialogue with remote id {:X} not found.", acMessage.ServerId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);
    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    if (!pActor)
        return;

    // This is only for fallout 4
    TESTopicInfo* pInfo = nullptr;
    pInfo = Cast<TESTopicInfo>(TESForm::GetById(acMessage.TopicFormId));

    SubtitleManager::Get()->ShowSubtitle(pActor, acMessage.Text.c_str(), pInfo);
}

void CharacterService::OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept
{
    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): this entity iteration shouldn't technically be necessary, just look for the local component
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error(__FUNCTION__ ": failed to find server id for entity");
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.ServerId)
        {
            auto& formIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.ServerId, formIdComponent.Id);
            }

            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
            {
                // Probably left the room and/or temporary.
                spdlog::info(__FUNCTION__ ": no local Actor for serverId {:X} to relinquish", serverId);
                continue;
            }

            pActor->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            spdlog::info(__FUNCTION__ ": relinquished control of actor {:X} with server id {:X}", pActor->formID, acMessage.ServerId);

            return;
        }
    }

    spdlog::error("Did not find actor to relinquish control of, server id {:X}", acMessage.ServerId);
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": failed to retrieve actor to teleport.");
        return;
    }

    MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

    spdlog::info("Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({}, {}, {})", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Takes ownership of all actors
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

void CharacterService::MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
    }
    // In case of lazy-loading of exterior cells
    else
    {
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error(__FUNCTION__ ": failed to fetch cell to teleport, actor: {:X}, worldspace: {:X}, cell: {:X}, position: {}, {}, {}", apActor->formID, acWorldSpaceId.BaseId, acCellId.BaseId, acPosition.x, acPosition.y, acPosition.z);
        return;
    }

    apActor->MoveTo(pCell, acPosition);
}

void CharacterService::ProcessNewEntity(entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": actor for new entity not found, form id: {:X}", formIdComponent.Id);
        return;
    }

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // TODO(cosideci): don't just take all actors (i.e. from other parties),
        // maybe check it server side, add a variable to the request.
        if (m_world.GetPartyService().IsLeader() && !pActor->IsTemporary() && !pActor->IsMount())
        {
            spdlog::info("Sending ownership claim for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);

            TakeOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
        {
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}, worn armor pieces: {}", pActor->formID, pRemoteComponent->Id, pActor->GetWornArmor().Entries.size());

#if TP_SKYRIMVR
            if (!m_world.GetServerSettings().RemoteBodyCollisionEnabled)
                SetBodyCollision(pActor, false);
#endif

            // The body has come back after we kept it through a 3D rebuild, so put its equipment back on.
            if (const auto* pPending = m_world.try_get<PendingEquipmentComponent>(aEntity))
            {
                const Inventory cContent = pPending->Content;

                m_world.remove<PendingEquipmentComponent>(aEntity);

                pActor->SetActorInventory(cContent);

                spdlog::info("Restored {} saved items to remote player body {:X}, now wearing {} armor pieces", cContent.Entries.size(), pActor->formID, pActor->GetWornArmor().Entries.size());
            }
        }

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    RequestServerAssignment(aEntity);
}

void CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    /**
     * With dead body sync off, a corpse is never registered with the server.
     *
     * The outgoing half of the switch and the broadest of the three: a body that is never assigned is never
     * owned, never broadcast and never handed to anybody, so no other client is asked to spawn it. Refusing
     * only the incoming spawns would leave all of that traffic running with nothing to show for it.
     *
     * The player is excluded outright rather than by IsDead, because a dead player is still a player and their
     * body is not one of the bodies this switch is about.
     */
    if (!m_world.GetServerSettings().DeadBodySyncEnabled && formIdComponent.Id != 0x14 && pActor->IsDead())
        return;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return;

    // A deleted actor keeps its slot in the form table for a while but loses its cell along with its 3D, and
    // the party sweep in OnPartyJoinedEvent walks every FormIdComponent there is, stale ones included. So a
    // body torn down for a rebuild reaches here and reads its cell id off nothing.
    if (!pActor->parentCell)
        return;

    AssignCharacterRequest message{};

    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::error("Server reference id not found for form id {:X}", formIdComponent.Id);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pActor->parentCell->formID);
        return;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    // Serialize the base form
    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->formID >= 0xFF000000;

    if (isPlayer)
    {
        pNpc->MarkChanged(0x2000800);
    }

    const auto changeFlags = pNpc->GetChangeFlags();

    if (isPlayer || changeFlags != 0)
    {
        message.ChangeFlags = changeFlags;
        pNpc->Serialize(&message.AppearanceBuffer);
    }

    if (isPlayer)
    {
        auto& entries = message.FaceTints.Entries;

        const auto& tints = PlayerCharacter::Get()->GetTints();

        entries.resize(tints.length);

        for (auto i = 0u; i < tints.length; ++i)
        {
            entries[i].Alpha = tints[i]->alpha;
            entries[i].Color = tints[i]->color;
            entries[i].Type = tints[i]->type;

            if (tints[i]->texture)
                entries[i].Name = tints[i]->texture->name.AsAscii();
        }
    }

    if (isPlayer)
    {
        auto& questLog = message.QuestContent.Entries;
        auto& modSystem = m_world.GetModSystem();

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            auto* pQuest = objective.instance->quest;
            if (!pQuest)
                continue;

            if (!QuestService::IsNonSyncableQuest(pQuest))
            {
                GameId id{};

                if (modSystem.GetServerModId(pQuest->formID, id))
                {
                    auto& entry = questLog.emplace_back();
                    entry.Stage = pQuest->currentStage;
                    entry.Id = id;
                }
            }
        }

        // remove duplicates
        const auto ip = std::unique(questLog.begin(), questLog.end());
        questLog.resize(std::distance(questLog.begin(), ip));
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    message.IsPlayerSummon = pActor->GetCommandingActor() && pActor->GetCommandingActor()->formID == 0x14;

    if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    if (isTemporary)
    {
        if (pNpc && !m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId))
        {
            spdlog::error("Server NPC form id not found for form id {:X}", pNpc->formID);
            return;
        }
    }

    // Serialize actions
    auto* const pExtension = pActor->GetExtension();

    message.LatestAction = pExtension->LatestAnimation;
    pActor->SaveAnimationVariables(message.LatestAction.Variables);

    spdlog::info("Request id: {:X}, cookie: {:X}, entity: {:X}", formIdComponent.Id, sCookieSeed, to_integral(aEntity));

    if (m_transport.Send(message))
    {
        m_world.emplace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);

        sCookieSeed++;
    }
}

void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));

        if (pActor)
        {
            if (pActor->IsTemporary())
            {
                spdlog::info("Temporary Remote Deleted {:X}", aFormId);
                pActor->Delete();
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
            }
        }

        DeleteRemoteEntityComponents(aEntity);

        return;
    }

    // In the event we were waiting for assignment, drop it
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);

        CancelAssignmentRequest message;
        message.Cookie = waitingComponent.Cookie;

        m_transport.Send(message);

        m_world.remove<WaitingForAssignmentComponent>(aEntity);
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;

        if (Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId)))
        {
            if (!pActor->IsTemporary())
            {
                auto& modSystem = m_world.GetModSystem();

                if (TESWorldSpace* pWorldSpace = pActor->GetWorldSpace())
                {
                    if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
                        spdlog::error("World space id not found, despite having a world space, {:X}", pWorldSpace->formID);
                }

                if (TESObjectCELL* pCell = pActor->GetParentCell())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }

        spdlog::info(
            "Transferring ownership of local actor, server id: {:X}, worldspace: {:X}, cell: {:X}, position: "
            "({}, {}, {})",
            request.ServerId, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position.x, request.Position.y, request.Position.z);

        m_transport.Send(request);

        m_world.remove<LocalAnimationComponent, LocalComponent>(aEntity);
    }
}

Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity);
    auto* pInterpolationComponent = m_world.try_get<InterpolationComponent>(aEntity);

    if (!pWaitingFor3D || !pInterpolationComponent)
    {
        spdlog::error(__FUNCTION__ ": could not find WaitingFor3D or InterpolationComponent");
        return nullptr;
    }

    auto& acMessage = pWaitingFor3D->SpawnRequest;

    Actor* pActor = nullptr;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            const uint32_t cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X}.", remoteComponent.Id);
        return nullptr;
    }

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, pInterpolationComponent->Position);
    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawned character for entity, server id: {:X}", remoteComponent.Id);

    return pActor;
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();

    return actorData;
}

void CharacterService::RunLocalUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 100ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    ClientReferencesMoveRequest message;
    message.Tick = m_transport.GetClock().GetCurrentTick();

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);

        AnimationSystem::Serialize(m_world, message, localComponent, animationComponent, formIdComponent);
    }

    m_transport.Send(message);
}

void CharacterService::RunRemoteUpdates() noexcept
{
    // Delay by 300ms to let the interpolation system accumulate interpolation points
    const auto tick = m_transport.GetClock().GetCurrentTick() - 300;

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();

    for (auto entity : interpolatedEntities)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        /**
         * An actor that dies while it is already being synced stops being moved from the network, which is the
         * case the two gates at the ends of the pipeline cannot catch: it was alive and legitimately assigned
         * when it was spawned. Dropping the actor rather than skipping the call keeps interpolation running,
         * which the spawn decision below depends on, and is a path this loop already takes for an entity with
         * no form.
         *
         * The consequence is each client's own ragdoll settling the body instead of two clients warping it at
         * each other, which is what dragging a corpse looked like.
         *
         * PlayerComponent rather than the extension's player flag: it is set from the spawn message's IsPlayer
         * and lives in the ECS, so it is there whether or not the path that applies 3D has run. The flag is
         * not, which is the same trap OnActorRemoved documents.
         */
        if (pActor && !m_world.GetServerSettings().DeadBodySyncEnabled && pActor->IsDead() && !m_world.all_of<PlayerComponent>(entity))
            pActor = nullptr;

        InterpolationSystem::Update(pActor, interpolationComponent, tick);
    }

    auto animatedView = m_world.view<RemoteComponent, RemoteAnimationComponent, FormIdComponent>();

    for (auto entity : animatedView)
    {
        auto& animationComponent = animatedView.get<RemoteAnimationComponent>(entity);
        auto& formIdComponent = animatedView.get<FormIdComponent>(entity);

        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        AnimationSystem::Update(m_world, pActor, animationComponent, tick);
    }

    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();

    for (auto entity : facegenView)
    {
        auto& formIdComponent = facegenView.get<FormIdComponent>(entity);
        auto& faceGenComponent = facegenView.get<FaceGenComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        FaceGenSystem::Update(m_world, pActor, faceGenComponent);
    }

    auto waitingView = m_world.view<FormIdComponent, WaitingFor3D>();

    Vector<entt::entity> toRemove;
    for (auto entity : waitingView)
    {
        auto& formIdComponent = waitingView.get<FormIdComponent>(entity);
        auto& waitingFor3D = waitingView.get<WaitingFor3D>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || !pActor->GetNiNode())
            continue;

        // By now, the actor has materialized in the world and is ready for further setup

        ValidateActor(pActor, "before SetActorInventory");
        pActor->SetActorInventory(waitingFor3D.SpawnRequest.InventoryContent);
        ValidateActor(pActor, "after SetActorInventory");
        pActor->SetFactions(waitingFor3D.SpawnRequest.FactionsContent);

        if (!waitingFor3D.SpawnRequest.ActionsToReplay.Actions.empty())
        {
            pActor->LoadAnimationVariables(waitingFor3D.SpawnRequest.ActionsToReplay.Actions[0].Variables);
        }

        m_weaponDrawUpdates[pActor->formID] = {waitingFor3D.SpawnRequest.IsWeaponDrawn};

        if (pActor->IsDead() != waitingFor3D.SpawnRequest.IsDead)
            waitingFor3D.SpawnRequest.IsDead ? pActor->Kill() : pActor->Respawn();

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        toRemove.push_back(entity);

        // The other half of the pair logged at the spawn request: what the body ended up wearing, and whether
        // its face was ever generated. A black head is either a tint set that never arrived or one that
        // arrived and could not be applied, and only the two lines together say which.
        const auto* pFaceGen = m_world.try_get<FaceGenComponent>(entity);

        spdlog::info("Applied 3D for actor, form id: {:X}, now wearing {} armor pieces, face tints {}", pActor->formID,
                     pActor->GetWornArmor().Entries.size(), pFaceGen ? (pFaceGen->Generated ? "generated" : "pending") : "absent");
    }

    for (auto entity : toRemove)
        m_world.remove<WaitingFor3D>(entity);
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        const auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        // Check if cached factions and current factions are identical
        auto factions = pActor->GetFactions();

        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;

        // If not send the current factions and replace the cached factions
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    auto invisibleView = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(entt::exclude<FormIdComponent>);
    Vector<entt::entity> entities(invisibleView.begin(), invisibleView.end());

    for (const auto entity : entities)
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(entity);
        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);

        if (const auto pWorldSpace = PlayerCharacter::Get()->GetWorldSpace())
        {
            float characterX = interpolationComponent.Position.x;
            float characterY = interpolationComponent.Position.y;
            const auto characterCoords = GridCellCoords::CalculateGridCellCoords(characterX, characterY);
            const TES* pTES = TES::Get();
            const auto playerCoords = GridCellCoords(pTES->centerGridX, pTES->centerGridY);

            // TODO(cosideci): IsDragon probably shouldn't be straight up false here.
            if (GridCellCoords::IsCellInGridCell(characterCoords, playerCoords, false))
            {
                auto* pActor = Cast<Actor>(TESForm::GetById(remoteComponent.CachedRefId));
                if (!pActor)
                {
                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor)
                        continue;

                    remoteComponent.CachedRefId = pActor->formID;
                }

                pActor->MoveTo(PlayerCharacter::Get()->parentCell, interpolationComponent.Position);
            }
        }
    }
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (m_cachedExperience == 0.f)
        return;

    if (!World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;

    m_cachedExperience = 0.f;

    m_transport.Send(message);

    spdlog::debug("Sending over experience {}", message.Experience);
}

namespace
{
constexpr uint8_t kWeaponDrawPasses = 2;

#if TP_SKYRIMVR
// The last two passes are the hand items rather than the weapon state. See DetachHandItems.
constexpr uint8_t kWeaponDrawTotalPasses = 4;

// What a body can be holding that has to be reseated, in the order the ids are carried between passes.
enum HandItem : size_t
{
    kShield = 0,
    kRightHand = 1,
    kLeftHand = 2,
};

/**
 * @brief Takes a remote body's shield and weapons off, so the pass after this one can put them back.
 *
 * A held item is placed on the hand or on the body once, when it is equipped, from whatever the actor's weapon
 * state said at that moment, and nothing moves it afterwards. A remote body is equipped and given its weapon
 * state by two paths that do not wait for each other, so a body that spawns carrying anything routinely ends up
 * wearing it on the hand while standing idle. It looks correct the moment the weapon comes out, which is what
 * makes it easy to read as an animation problem rather than an attachment one.
 *
 * Equipping it again is the whole repair, and it is what a player does by hand to clear it: the owner takes the
 * item off and puts it back on, the change arrives as two NotifyEquipmentChanges seconds apart, and the body
 * attaches it afresh against a state that has stopped moving by then.
 *
 * Doing both halves in one call does not work, and the log said why. Queued, they cancel out. Applied now, they
 * ran and left the tree byte for byte the same, while the repair the owner drove rebuilt the whole 3D, which is
 * what hand sync noticed ten seconds later. So this follows the owner's shape exactly: take it off, wait a
 * pass, then do what OnNotifyEquipmentChanges does for a piece of armor, which is to strip the worn armor,
 * equip the item and put the armor back.
 *
 * The first version of this handled the shield alone and returned early when there was no shield, before it
 * reached the weapon state or the armor, so a player carrying a sword and no shield got no repair at all.
 */
void DetachHandItems(Actor& aActor, const bool aWeaponDrawn, uint32_t (&aOut)[3]) noexcept
{
    aOut[kShield] = 0;
    aOut[kRightHand] = 0;
    aOut[kLeftHand] = 0;

    // With the weapon out the hand is where all of this belongs, so there is nothing to repair and no reason
    // to make the body flicker.
    if (aWeaponDrawn)
        return;

    if (auto* pChanges = aActor.GetContainerChanges())
    {
        // 39 is the shield's biped slot.
        if (TESObjectARMO* pShield = pChanges->GetArmor(39))
            aOut[kShield] = pShield->formID;
    }

    // Slot 1 is the right hand and slot 0 the left. Weapons only: the same call also reports spells, which
    // have no 3D to reseat, and the shield, which is covered above. A torch is neither and is left alone,
    // untested rather than deliberately excluded.
    const uint32_t cSlot[2] = {1, 0};
    const size_t cIndex[2] = {kRightHand, kLeftHand};

    for (size_t hand = 0; hand < 2; ++hand)
    {
        TESForm* pHeld = aActor.GetEquippedWeapon(cSlot[hand]);

        if (pHeld && pHeld->formType == FormType::Weapon)
            aOut[cIndex[hand]] = pHeld->formID;
    }

    if (!aOut[kShield] && !aOut[kRightHand] && !aOut[kLeftHand])
        return;

    /**
     * The flag the attach below reads, forced rather than asked for.
     *
     * By this point the sheathe has had a second and a half to run and nothing is mid animation, so a flag that
     * still reads drawn means the transition never completed on this body. That is one of the two ways an item
     * ends up on the hand, and leaving it would make the reattach put the item straight back there.
     */
    if (aActor.actorState.IsWeaponDrawn())
        aActor.actorState.SetWeaponDrawn(false);

    // Both overrides, because the equip hooks refuse to touch a remote actor without one. Nothing is broadcast
    // either way: the change events are only raised for a local actor.
    ScopedEquipOverride equipOverride;
    ScopedInventoryOverride inventoryOverride;

    auto* pEquipManager = EquipManager::Get();
    auto& defaultObjects = DefaultObjectManager::Get();

    // The shield takes no equip slot. That one belongs to weapons and spells, while a shield is armor and takes
    // its place from its biped slot. Everything else matches what OnNotifyEquipmentChanges passes, since that
    // is the path known to repair this when the owner drives it.
    const auto cUnEquipHeld = [&](const uint32_t acId, TESForm* apSlot) {
        if (!acId)
            return;

        if (TESForm* pItem = TESForm::GetById(acId))
            pEquipManager->UnEquip(&aActor, pItem, nullptr, 1, apSlot, false, true, false, false, nullptr);
    };

    cUnEquipHeld(aOut[kShield], nullptr);
    cUnEquipHeld(aOut[kRightHand], defaultObjects.rightEquipSlot);
    cUnEquipHeld(aOut[kLeftHand], defaultObjects.leftEquipSlot);

    spdlog::info("Took the hand items off remote body {:X}: shield {:X}, right {:X}, left {:X}. They go back next pass.", aActor.formID, aOut[kShield], aOut[kRightHand], aOut[kLeftHand]);
}

/**
 * @brief Puts back what DetachHandItems took off, the way an owner driven equip would.
 *
 * The armor around it is stripped and restored because that is what OnNotifyEquipmentChanges does for any piece
 * of armor, and because the game will not swap a worn piece out on its own. It is also the part of the owner
 * driven repair that rebuilds the 3D, which is the thing a plain equip of the item alone never did.
 */
void ReattachHandItems(Actor& aActor, const uint32_t (&acItems)[3]) noexcept
{
    if (!acItems[kShield] && !acItems[kRightHand] && !acItems[kLeftHand])
        return;

    ScopedEquipOverride equipOverride;
    ScopedInventoryOverride inventoryOverride;

    auto* pEquipManager = EquipManager::Get();
    auto& defaultObjects = DefaultObjectManager::Get();
    auto& modSystem = World::Get().GetModSystem();

    const Inventory cWornArmor = aActor.GetWornArmor();

    const auto cEachArmor = [&](auto&& aFunc) {
        for (const auto& cEntry : cWornArmor.Entries)
        {
            if (TESForm* pArmor = TESForm::GetById(modSystem.GetGameId(cEntry.BaseId)))
                aFunc(pArmor);
        }
    };

    cEachArmor([&](TESForm* apArmor) { pEquipManager->UnEquip(&aActor, apArmor, nullptr, 1, nullptr, false, true, false, false, nullptr); });

    /**
     * The zero check is the point of this, not the null check.
     *
     * An empty hand is stored as form id zero and is the common case: a player carrying one sword leaves two of
     * the three slots empty. TESForm::GetById(0) is not documented to return null, and the first version of this
     * went straight from the id to the equip, so an empty hand could hand whatever form zero resolves to to
     * EquipManager and have it equipped into the right hand. That is a candidate for the sword that came back
     * attached to the waist at an angle nothing would produce on purpose.
     */
    const auto cEquipHeld = [&](const uint32_t acId, TESForm* apSlot) {
        if (!acId)
            return;

        if (TESForm* pItem = TESForm::GetById(acId))
            pEquipManager->Equip(&aActor, pItem, nullptr, 1, apSlot, false, true, false, false);
    };

    cEquipHeld(acItems[kShield], nullptr);
    cEquipHeld(acItems[kRightHand], defaultObjects.rightEquipSlot);

    /**
     * The off hand weapon is left off, and that is not an oversight.
     *
     * Vanilla has no left hip sheath. A one handed weapon in the off hand is not drawn at all once it is put
     * away, which is what the owner sees on their own screen. Equipping it here puts it back on the SHIELD
     * node with a shield's sheathed placement, so a sword ends up lying flat against the waist. Leaving it
     * off reproduces the correct look, and RunOffHandWeaponUpdates puts it back the moment the body draws.
     *
     * Only the off hand needs this. A two handed weapon is always in the right hand, so anything reaching
     * the left slot is one handed by definition and there is no weapon type to test.
     */

    cEachArmor([&](TESForm* apArmor) { pEquipManager->Equip(&aActor, apArmor, nullptr, 1, nullptr, false, true, false, false); });

    spdlog::info("Put the hand items back on remote body {:X}: shield {:X}, right {:X}, around {} worn armor pieces. Off hand {:X} stays off while the weapon is away.", aActor.formID, acItems[kShield], acItems[kRightHand], cWornArmor.Entries.size(), acItems[kLeftHand]);

}
#else
constexpr uint8_t kWeaponDrawTotalPasses = kWeaponDrawPasses;
#endif
} // namespace

void CharacterService::QueueWeaponDrawUpdate(const uint32_t acFormId, const bool acDrawn) noexcept
{
    const auto cExisting = m_weaponDrawUpdates.find(acFormId);

    // Already on its way to the same state, so it is left to run rather than started again from the beginning.
    // See the declaration: the hand item passes are late enough that restarting is the same as cancelling.
    if (cExisting != m_weaponDrawUpdates.end() && cExisting->second.m_drawWeapon == acDrawn)
        return;

    m_weaponDrawUpdates[acFormId] = {acDrawn};
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    std::vector<uint32_t> toRemove{};

    /**
     * We do 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence, and on VR three
     * more for the shield and weapons it leaves on the wrong node.
     *
     * The hand item passes have to come after the last weapon one, not because the state needs to settle, since
     * DetachHandItems forces the flag itself, but because SetWeaponDrawnEx forces a draw and sheathe cycle when
     * the state already matches what is being asked for, and that would undo the reattach.
     *
     * Past that the gaps are only there to keep the unequip and the equip in different drains of the equip
     * queue, since the two cancel out inside one. They were a second and a half and a second, chosen with
     * nothing behind them, which cost three seconds of a body standing there holding its sword wrong. A quarter
     * second is around twenty frames in VR and is still generous for that.
     */
    constexpr double kPassAt[] = {0.5, 2.0, 2.25, 2.75};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];

        data.m_timer += acUpdateEvent.Delta;

        if (data.m_timer <= kPassAt[data.m_pass])
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor)
            continue;

        if (data.m_pass < kWeaponDrawPasses)
        {
            /**
             * Called even when the body has no 3D, which matters for more than the weapon.
             *
             * Skipping it looks reasonable, since a body with no 3D cannot take a weapon state, and on
             * 2026-08-19 that skip was tried. It stopped remote players reappearing at all: the body was
             * relocated, never rebuilt, and the game had reclaimed the dynamic reference within forty seconds.
             * This call is what was keeping the actor alive and nudging its 3D back, entirely as a side effect.
             * Do not make it conditional again without something else taking over that job.
             */
            pActor->SetWeaponDrawnEx(data.m_drawWeapon);
        }
#if TP_SKYRIMVR
        else if (!pActor->GetExtension()->IsRemotePlayer())
        {
            // An NPC plays its own sheathe animation, so the game puts its weapons away correctly and this
            // repair is pure churn on it. Skipping them also stops the client stripping and restoring the
            // worn armor of every actor in the cell each time one spawns.
            toRemove.push_back(cId);
            continue;
        }
        else if (data.m_pass == kWeaponDrawPasses)
        {
            DetachHandItems(*pActor, data.m_drawWeapon, data.m_handItems);
        }
        else
        {
            ReattachHandItems(*pActor, data.m_handItems);

            if (data.m_handItems[kLeftHand])
                m_offHandWeapons[cId] = {data.m_handItems[kLeftHand], true};
        }
#endif

        if (++data.m_pass >= kWeaponDrawTotalPasses)
            toRemove.push_back(cId);
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}

/**
 * @brief Keeps an off hand weapon off a remote body while its weapon is away, and puts it back when it is not.
 *
 * See OffHandWeapon for why taking it off is what makes the body look right. This is the other half: without it
 * the weapon would stay invisible through the fight as well.
 *
 * Cheap enough to run every frame. The map holds one entry per remote player carrying something in the off
 * hand, and an entry only does work on the frame the body's weapon state changes.
 */
void CharacterService::RunOffHandWeaponUpdates() noexcept
{
#if TP_SKYRIMVR
    Vector<uint32_t> toRemove{};

    // Looked up by key rather than bound from the iteration, which yields a const value here. The loop over
    // m_weaponDrawUpdates above does the same thing for the same reason.
    for (const auto& [cId, _] : m_offHandWeapons)
    {
        OffHandWeapon& state = m_offHandWeapons[cId];

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));

        // The body is gone, or is no longer somebody else's. Either way this is not ours to manage, and the
        // entry has to go rather than wait for a recycled form id to inherit it.
        if (!pActor || !pActor->GetExtension()->IsRemotePlayer())
        {
            toRemove.push_back(cId);
            continue;
        }

        const bool cDrawn = pActor->actorState.IsWeaponDrawn();

        // Stowed while the weapon is away and held while it is out are both already right.
        if (cDrawn != state.Stowed)
            continue;

        TESForm* pItem = TESForm::GetById(state.ItemId);
        if (!pItem)
        {
            toRemove.push_back(cId);
            continue;
        }

        ScopedEquipOverride equipOverride;
        ScopedInventoryOverride inventoryOverride;

        auto* pEquipManager = EquipManager::Get();
        TESForm* pLeftSlot = DefaultObjectManager::Get().leftEquipSlot;

        if (cDrawn)
            pEquipManager->Equip(pActor, pItem, nullptr, 1, pLeftSlot, false, true, false, false);
        else
            pEquipManager->UnEquip(pActor, pItem, nullptr, 1, pLeftSlot, false, true, false, false, nullptr);

        state.Stowed = !cDrawn;

        spdlog::debug("Off hand weapon {:X} on remote body {:X} is {}", state.ItemId, cId, state.Stowed ? "off, which is how vanilla shows one that is put away" : "back in the hand");
    }

    for (uint32_t id : toRemove)
        m_offHandWeapons.erase(id);
#endif
}
