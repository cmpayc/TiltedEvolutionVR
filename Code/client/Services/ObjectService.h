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
struct DynamicObjectCreatedEvent;
struct ObjectPickedUpEvent;
struct NotifyObjectRemove;

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
    void OnDynamicObjectCreated(const DynamicObjectCreatedEvent&) noexcept;
    void OnObjectPickedUp(const ObjectPickedUpEvent&) noexcept;
    void OnObjectRemoveNotify(const NotifyObjectRemove&) noexcept;

    BSTEventResult OnEvent(const TESActivateEvent*, const EventDispatcher<TESActivateEvent>*) override;

    entt::entity CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept;

    void RunHeldObjectUpdates() noexcept;
    void SendObjectTransform(const uint32_t acFormId, const bool aIsReleased) noexcept;
    void StopSettling(const uint32_t acFormId) noexcept;

    // Both return zero when the object is not a known dropped item, which is the normal case for a static
    // world reference. Both drop entries whose reference has died or been reused.
    uint64_t GetDropId(const uint32_t acFormId) noexcept;
    uint32_t GetDynamicFormId(const uint64_t acDropId) noexcept;
    void ForgetDynamicObject(const uint64_t acDropId) noexcept;

    void MarkDriving(const uint32_t acFormId, const glm::vec3& acPosition, const glm::vec3& acRotation) noexcept;
    void StopDriving(const uint32_t acFormId) noexcept;
    void RunDrivenObjectTimeouts() noexcept;
    void RunDrivenObjectInterpolation(const double aDelta) noexcept;

    // Diagnostic only, nothing here writes to an object. See WatchForDrift.
    void WatchForDrift(const uint32_t acFormId, const char* acpReason) noexcept;
    void StopWatchingDrift(const uint32_t acFormId) noexcept;
    void RunDriftWatch(const double aDelta) noexcept;
    void ReportDriftGeometry(const glm::vec3& acPosition, const glm::vec3& acDirection) noexcept;
    void RunCellDriftSweep(const double aDelta) noexcept;

    // Hands a warp-driven object back to local physics in a state where it will actually move again.
    static void RestoreObjectPhysics(TESObjectREFR* apObject) noexcept;

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
    entt::scoped_connection m_dynamicObjectConnection;
    entt::scoped_connection m_objectPickedUpConnection;
    entt::scoped_connection m_objectRemoveConnection;

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

    /**
     * @brief Pairs one dropped item's local reference with the drop that created it everywhere.
     *
     * An item dropped from an inventory becomes a temporary reference, created separately on each client,
     * so its form id differs per machine. The drop id is the shared name; this is the local half.
     *
     * BaseFormId exists because temporary form ids are **recycled within a session**: the same
     * `FF000857` was observed holding two different items minutes apart. An entry whose reference no
     * longer carries the base form it was registered with is therefore stale, not merely old, and
     * trusting it would sync the wrong object.
     */
    struct DynamicObject
    {
        uint64_t DropId{};
        uint32_t FormId{};
        uint32_t BaseFormId{};
    };

    Vector<DynamicObject> m_dynamicObjects{};

    /**
     * @brief An object we are currently warping because a remote player is holding it.
     *
     * Warping detaches the rigid body, and the unwarped write on release is what puts it back. That makes
     * the release the only repair, and a repair that can go missing: the holder can disconnect, the message
     * can be lost, or our own settling can be cancelled because somebody else took the object. An object
     * left warped is stuck at its last network position for the rest of the session, and the next player to
     * pick it up streams a frozen position that nobody sees move.
     *
     * So a driven object is repaired either by its release or by simply going quiet. Silence is the reliable
     * signal, because it covers every way a release can fail to arrive.
     */
    struct DrivenObject
    {
        uint32_t FormId{};
        std::chrono::steady_clock::time_point LastSeen{};

        /**
         * @brief The move currently being played out, which is what stops the object stepping.
         *
         * The stream arrives at 30 Hz and the headset draws at 90, so a packet written where it lands leaves
         * the object still for three frames and then somewhere else. Next to a hand that is redrawn every
         * frame off an interpolated body, that staircase is the difference between arms that look smooth and
         * a carried object that looks like it is being dragged.
         *
         * So a packet is not a position to write, it is a destination. From is where the object was being
         * shown when the packet landed, To is where the packet wants it, and Duration is the gap that packet
         * represents, so the motion is played out over exactly the time it took to arrive. The cost is one
         * packet of latency, about 33 ms, against motion that is continuous rather than in steps.
         */
        glm::vec3 From{};
        glm::vec3 To{};

        // Euler, the same triple the reference carries, eased the short way round. See LerpAngles.
        glm::vec3 FromRotation{};
        glm::vec3 ToRotation{};

        double Elapsed{};
        double Duration{};
    };

    Vector<DrivenObject> m_driven{};

    /**
     * @brief An object we have just stopped controlling, watched for a few seconds to see whether it stops.
     *
     * Purely a diagnostic. `pReason` is always a string literal, so the entry owns nothing.
     */
    struct DriftWatch
    {
        uint32_t FormId{};
        const char* pReason{};
        glm::vec3 Start{};
        glm::vec3 Late{};
        double Elapsed{};
        uint32_t Windows{};
        bool LateTaken{};
    };

    Vector<DriftWatch> m_driftWatch{};

    /**
     * @brief Where one reference in the player's cell was a second ago.
     *
     * Purely a diagnostic, and deliberately every reference rather than only the ones we sync: a drift that
     * moves objects nobody has touched is a different fault from one that moves only the streamed ones, and
     * sampling only the latter could never tell the two apart. See RunCellDriftSweep.
     */
    struct CellSample
    {
        uint32_t FormId{};

        // The 3D node's world translate, which is where the object is drawn.
        glm::vec3 Position{};

        // The reference's own position field, which physics writes back. Sampled alongside the node because
        // the two disagreeing is the difference between a body that is really moving and a transform somebody
        // is writing behind physics's back, and those have nothing in common but the symptom.
        glm::vec3 Reference{};
    };

    Vector<CellSample> m_cellSamples{};
    double m_cellSweepElapsed{};
    uint32_t m_cellSweepId{};
    uint32_t m_cellSweepReports{};

    // The player's own position at the last sweep, so a room that appears to move while the player walks can
    // be told from one that moves while the player stands still.
    glm::vec3 m_cellSweepPlayerAt{};

    // Frames counted since the last sweep report, and the longest one among them. Skyrim's havok is tied to
    // the frame rate and misbehaves when it drops, which is the one thing a second player reliably changes
    // and the one thing never measured here. See RunCellDriftSweep.
    uint32_t m_cellSweepFrames{};
    double m_cellSweepWorstFrame{};

    /**
     * @brief Every object this session has ever warped or carried.
     *
     * The three live lists above answer "is anything writing this object right now", which is not the question
     * a drift asks. Warping detaches an object's rigid body and the release is what puts it back; an object
     * whose repair went wrong is left with a detached body and drifts for ever, and by then it is in none of
     * the lists and the sweep called it untouched. On 2026-08-22 that made a drift among objects we had warped
     * dozens of times read exactly like a drift among objects nobody had gone near.
     *
     * Never pruned. One id per object touched in a session is nothing, and forgetting one would put the
     * ambiguity straight back.
     */
    Set<uint32_t> m_everHandled{};
};
