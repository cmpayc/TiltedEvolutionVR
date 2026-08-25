#pragma once
#include "Structs/Inventory.h"
#include "Structs/ActorData.h"

struct ActorAddedEvent;
struct ActorRemovedEvent;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct EquipmentChangeEvent;
struct FormIdComponent;
struct ActionEvent;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct ServerReferencesMoveRequest;
struct NotifyInventoryChanges;
struct NotifyFactionsChanges;
struct NotifyRemoveCharacter;
struct NotifySpawnData;
struct NotifyOwnershipTransfer;
struct SpellCastEvent;
struct NotifySpellCast;
struct InterruptCastEvent;
struct NotifyInterruptCast;
struct AddTargetEvent;
struct NotifyAddTarget;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct MountEvent;
struct NotifyMount;
struct InitPackageEvent;
struct NotifyNewPackage;
struct NotifyRespawn;
struct BeastFormChangeEvent;
struct AddExperienceEvent;
struct NotifySyncExperience;
struct DialogueEvent;
struct NotifyDialogue;
struct SubtitleEvent;
struct NotifySubtitle;
struct NotifyActorTeleport;
struct NotifyRelinquishControl;
struct PartyJoinedEvent;

struct Actor;
struct World;
struct TransportService;

/**
 * @brief Handles actors and players.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void DeleteTempActor(const uint32_t aFormId) noexcept;

    bool TakeOwnership(const uint32_t acFormId, const uint32_t acServerId, const entt::entity acEntity) const noexcept;

    void OnActorAdded(const ActorAddedEvent& acEvent) noexcept;
    void OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    void OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept;
    void OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) const noexcept;
    void OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept;
    void OnCharacterSpawn(const CharacterSpawnRequest& acMessage) noexcept;
    void OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept;
    void OnActionEvent(const ActionEvent& acActionEvent) const noexcept;
    void OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept;
    void OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept;
    void OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept;
    void OnRemoteSpawnDataReceived(const NotifySpawnData& acEvent) noexcept;
    void OnMountEvent(const MountEvent& acEvent) const noexcept;
    void OnNotifyMount(const NotifyMount& acMessage) const noexcept;
    void OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept;
    void OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept;
    void OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept;
    void OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept;
    void OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept;
    void OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept;
    void OnDialogueEvent(const DialogueEvent& acEvent) noexcept;
    void OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept;
    void OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept;
    void OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept;
    void OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept;
    void OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;

    void ProcessNewEntity(entt::entity aEntity) const noexcept;

private:
    void MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept;

    void RequestServerAssignment(entt::entity aEntity) const noexcept;
    void CancelServerAssignment(entt::entity aEntity, uint32_t aFormId) const noexcept;
    void DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept;

    Actor* CreateCharacterForEntity(entt::entity aEntity) const noexcept;
    ActorData BuildActorData(Actor* apActor) const noexcept;

    void RunLocalUpdates() const noexcept;
    void RunRemoteUpdates() noexcept;
    void RunFactionsUpdates() const noexcept;
    void RunSpawnUpdates() const noexcept;
    void RunExperienceUpdates() noexcept;
    void ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept;
    void RunOffHandWeaponUpdates() noexcept;

    /**
     * @brief Asks for a body's weapon state to be applied and, on VR, its hand items to be reseated, without
     *        restarting a request that is already running.
     *
     * For the queue sites reached by a *repeating* message, which is the spawn request for a character we
     * already have a body for. Assigning into m_weaponDrawUpdates rebuilds the entry with its pass counter back
     * at zero, and the passes that matter are late: the shield comes off at 2.25s and goes back at 2.75s. The
     * server sends a spawn request on every cell change a character makes, ten in fourteen seconds in an
     * exterior, so a body re-queued at that rate never lives long enough to be repaired.
     *
     * That is not hypothetical for the weapon state either. The test that guards its queue site compares the
     * *actor's* live flag against the message, and the flag stays wrong until the passes apply it, so a
     * repeating request kept matching and kept restarting the cycle that would have fixed it.
     *
     * An entry already asking for the same state is therefore left to finish. A different state still restarts
     * it, since that is a change the body has to be told about and the reseat has to happen against the new
     * state anyway.
     *
     * The one-shot sites, a fresh spawn and a body coming back into sight, deliberately assign instead. Each
     * fires once per body and each needs the early SetWeaponDrawnEx passes to run again, which this would skip.
     */
    void QueueWeaponDrawUpdate(const uint32_t acFormId, const bool acDrawn) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    float m_cachedExperience = 0.f;

    // TODO: revamp this, read the local anim var like vampire lord?
    struct WeaponDrawData
    {
        WeaponDrawData() = default;
        WeaponDrawData(bool aDrawWeapon)
            : m_drawWeapon(aDrawWeapon)
        {
        }

        double m_timer = 0.0;
        bool m_drawWeapon = false;
        uint8_t m_pass = 0;

        // Carried from the pass that takes the hand items off to the one that puts them back, because they
        // stop being findable as equipped in between. Shield, right hand, left hand, in that order.
        uint32_t m_handItems[3]{};
    };

    Map<uint32_t, WeaponDrawData> m_weaponDrawUpdates{};

#if TP_SKYRIMVR
    /**
     * @brief An off hand weapon on a remote body, and whether we currently have it taken off.
     *
     * Vanilla has no left hip sheath, so a one handed weapon in the off hand is simply not drawn once it is
     * put away. On a body that plays its own animations the game handles that. A remote body does not play
     * them, so the weapon stays parented to the SHIELD node and is given the placement a shield would get,
     * which lays a sword flat against the waist.
     *
     * Taking the weapon off the body reproduces what the player sees on their own screen exactly, and putting
     * it back the moment they draw keeps the hand right in combat. Keyed by the body's form id.
     */
    struct OffHandWeapon
    {
        uint32_t ItemId = 0;
        bool Stowed = false;
    };

    Map<uint32_t, OffHandWeapon> m_offHandWeapons{};
#endif

    /**
     * @brief The weapon state the server last reported for a body, kept so it can be applied once it has 3D.
     *
     * m_weaponDrawUpdates gives each entry two attempts, at half a second and two seconds, then drops it. A
     * body whose 3D is being rebuilt after a cell change is unavailable for closer to three, so both attempts
     * miss and the correction is lost 700ms before the body exists to take it. Measured on 2026-08-19.
     *
     * The attempts themselves must keep happening regardless, because that call is what nudges the 3D back.
     * So rather than defer them, the server's value is remembered here and re-queued when the body reappears.
     * Keyed by form id and cleared on use.
     */
    Map<uint32_t, bool> m_desiredWeaponDrawn{};

    entt::scoped_connection m_referenceAddedConnection;
    entt::scoped_connection m_referenceRemovedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_factionsConnection;
    entt::scoped_connection m_ownershipTransferConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_assignCharacterConnection;
    entt::scoped_connection m_characterSpawnConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_remoteSpawnDataReceivedConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_notifyMountConnection;
    entt::scoped_connection m_initPackageConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_beastFormChangeConnection;
    entt::scoped_connection m_addExperienceEventConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_dialogueEventConnection;
    entt::scoped_connection m_dialogueSyncConnection;
    entt::scoped_connection m_subtitleEventConnection;
    entt::scoped_connection m_subtitleSyncConnection;
    entt::scoped_connection m_actorTeleportConnection;
    entt::scoped_connection m_relinquishConnection;
    entt::scoped_connection m_partyJoinedConnection;
};
