#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct ActivateEvent;
struct TransportService;
struct NotifyActivate;
struct LockChangeEvent;
struct NotifyLockChange;
struct CellChangeEvent;
struct ScriptAnimationEvent;
struct AssignObjectsResponse;
struct NotifyScriptAnimation;
struct UpdateEvent;
struct ObjectHoldEvent;
struct NotifyObjectTransform;

/**
 * @brief Handles objects in the environment.
 */
class ObjectService final : public BSTEventSink<TESActivateEvent>
{
public:
    ObjectService(World&, entt::dispatcher&, TransportService&);

private:
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnAssignObjectsResponse(const AssignObjectsResponse&) noexcept;
    void OnActivate(const ActivateEvent&) noexcept;
    void OnActivateNotify(const NotifyActivate&) noexcept;
    void OnLockChange(const LockChangeEvent&) noexcept;
    void OnLockChangeNotify(const NotifyLockChange&) noexcept;
    void OnScriptAnimationEvent(const ScriptAnimationEvent&) noexcept;
    void OnNotifyScriptAnimation(const NotifyScriptAnimation&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnObjectHold(const ObjectHoldEvent&) noexcept;
    void OnObjectTransformNotify(const NotifyObjectTransform&) noexcept;

    BSTEventResult OnEvent(const TESActivateEvent*, const EventDispatcher<TESActivateEvent>*) override;

    entt::entity CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept;

    void RunHeldObjectUpdates() noexcept;
    void SendObjectTransform(const uint32_t acFormId, const bool aIsReleased) noexcept;
    void StopSettling(const uint32_t acFormId) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_onActivateConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_lockChangeNotifyConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_scriptAnimationNotifyConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_objectHoldConnection;
    entt::scoped_connection m_objectTransformConnection;

    // What each VR hand is holding, index 0 right and 1 left, zero meaning empty. Two hands can hold
    // the same object, so this is a slot per hand rather than a set: releasing one hand must not stop
    // the stream while the other is still holding on.
    uint32_t m_heldByHand[2]{};

    /**
     * @brief An object that has been let go of but is still moving.
     *
     * The hand opening is not the end of the motion. HIGGS hands the object to physics with the throw
     * velocity, so it flies and bounces before it lands, and we have no way to send a velocity: the
     * client has no rigid body access. A receiver given only the release point would drop the object
     * straight down from there and end up somewhere else entirely.
     *
     * So the stream does not stop at the release. It keeps running until the object stops moving, and
     * only then is the final no-warp message sent. Both clients watch the same flight and agree on
     * where it came to rest, which leaves nothing to diverge.
     */
    struct SettlingObject
    {
        uint32_t FormId{};
        glm::vec3 LastPosition{};
        double Elapsed{};
        uint32_t StillTicks{};
    };

    Vector<SettlingObject> m_settling{};
};
