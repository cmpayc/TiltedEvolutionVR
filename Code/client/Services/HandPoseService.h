#pragma once

#include <Actor.h>

struct World;
struct TransportService;
struct ImguiService;

struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyHandPose;

struct NiAVObject;
struct NiNode;

/**
 * @brief Shows where other VR players have their hands.
 *
 * Two halves. A VR client reads its own controller nodes and sends them for its own character. Every client
 * near that player receives them and poses that character's arms.
 *
 * The posing is the difficult half and four separate things all have to be right for it to reach the screen.
 * PROGRESS.md session 9 records the measurements behind each; in short:
 *
 *  - the write has to happen after the skeleton is posed for the frame and on a single thread, which is what
 *    the game's render hook gives us and HIGGS's post VRIK callback does not,
 *  - it has to go to BSFlattenedBoneTree's transform array as well as the bone nodes, because the skinning
 *    reads the array,
 *  - descendants have to be collected from that array's parent indices, since most bones below the wrist have
 *    no node at all,
 *  - and a bone's rotation must never be built from scratch, only turned from where it points to where it
 *    should point, or its rest orientation and roll are lost.
 */
struct HandPoseService
{
    HandPoseService(entt::dispatcher& aDispatcher, World& aWorld, TransportService& aTransport, ImguiService& aImguiService);

    TP_NOCOPYMOVE(HandPoseService);

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnHandPoseNotify(const NotifyHandPose& acMessage) noexcept;

    // A bone to pose, with its slot in the flattened array. Public because the posing helpers take it.
    struct PosedNode
    {
        NiAVObject* pNode{nullptr};
        uint8_t* pFlatEntry{nullptr};

        /**
         * @brief pNode's vtable pointer as it was when this was resolved, or null if it could not be read.
         *
         * Identity check for the pointer, because the root compare in PoseActor cannot catch every rebuild:
         * a freed root is often handed straight back by the node pools, so the new 3D can land on the same
         * address and the compare passes with every child pointer dangling. A write through one then lands in
         * whatever now owns that memory. On 2026-08-18 that was a BSLightingShaderProperty: the world
         * transform went in at +0x7C, its render pass list head at +0x98 took two floats of the rotation, and
         * the game crashed in ClearRenderPassArrays walking the result.
         *
         * An object of a different class cannot have the same vtable, so comparing this catches reuse that the
         * address alone cannot.
         */
        const void* pVTable{nullptr};

        /**
         * @brief pFlatEntry's back pointer to its bone node, as it was when this was resolved.
         *
         * pFlatEntry points into the BSFlattenedBoneTree's bone array, which belongs to the 3D and dangles on a
         * rebuild exactly like pNode does. It cannot be covered by pVTable, because an array slot is raw bytes
         * with no vtable of its own, and it needs its own check rather than riding on pNode's: WriteBone writes
         * through pFlatEntry even when pNode is null, which ResolveChains produces on purpose for a bone whose
         * node could not be read.
         *
         * Each slot carries a pointer back to its own node at kBoneEntryRefNode, so comparing that is an
         * identity check on the slot using data already in it.
         */
        const void* pFlatRefNode{nullptr};
    };

    // Poses every remote player's arms. Driven from the game's render hook, which is the one point in the frame
    // where a bone write is both late enough to survive and on a single thread.
    void OnRenderPose() noexcept;

private:
    struct ArmChain
    {
        PosedNode UpperArm{};
        PosedNode Forearm{};
        PosedNode Hand{};

        // Descendants, from the flattened array rather than the node tree.
        std::vector<PosedNode> UpperSubtree;
        std::vector<PosedNode> ForeSubtree;
        std::vector<PosedNode> HandSubtree;

        bool HasCore() const noexcept { return UpperArm.pNode && Forearm.pNode && Hand.pNode; }
    };

    /**
     * @brief Everything needed to keep one remote player's arms posed.
     *
     * Resolved once per actor and kept, because resolving costs a name search per bone plus a walk of the bone
     * array, which is far too much to repeat every frame.
     */
    struct RemoteHands
    {
        uint32_t FormId{};

        // Palm goals, relative to the character's own root, as last received.
        glm::vec3 Palm[2]{};
        bool HasPose{false};

        // The sender's real eye height, and this character's own head height, both above the root. Their ratio
        // turns a real world hand position into the same position on a body of a different size.
        float SenderEyeHeight{0.f};

        /**
         * @brief This character's head height above its root, zero until a believable one has been measured.
         *
         * Measured lazily rather than captured when the chains resolve. A resolve can land while the actor is
         * mid animation or otherwise not standing, and a single bad reading then sets the scale for good: one
         * such capture put the head bone *below* the shoulder, giving a scale of 0.85 that dropped every hand
         * to chest height and never corrected itself.
         *
         * Nothing is trusted until the head reads clearly above the shoulder, and until then no scaling is
         * applied, which is a smaller error than scaling by a wrong number.
         */
        float HeadHeight{0.f};
        NiAVObject* pHead{nullptr};



        // Time since the last message. A sender that stops talking, because it left, crashed or went out of
        // range, must not leave an actor's arms held in its last pose for ever.
        double Age{0.0};

        ArmChain Chain[2]{};
        uint32_t ChainFor{};

        /**
         * @brief The actor's 3D root as it was when the chains were resolved.
         *
         * Everything cached in ArmChain is a raw pointer into that 3D: bone nodes and slots in the flattened
         * bone array. The game rebuilds an actor's 3D for all sorts of reasons, equipment changes and cell
         * loads among them, and the form id does not change when it does. Re-resolving only on a form id
         * change therefore leaves every cached pointer dangling, and the next frame writes a transform into
         * freed memory.
         *
         * Comparing the root each frame costs one pointer compare and catches it.
         */
        NiNode* Root{nullptr};

        // Time since the last resolve attempt that failed. Resolving is expensive, so a failure must not be
        // retried every frame: FindBoneCount alone does a VirtualQuery per bone entry.
        double SinceFailedResolve{0.0};
        bool ResolveFailed{false};

        // Reference orientation per bone, relative to the actor's root, captured once. Composing onto the live
        // rotation instead would inherit whatever roll the animation is applying and make the arms spin.
        glm::mat3 RestRotate[2][2]{};
        glm::vec3 RestDir[2][2]{};
        bool RestCaptured{false};

        bool LayoutConfirmed{false};
    };

    void SendLocalPose() noexcept;

    /**
     * @brief Whether a point is in front of the viewer, from the VR headset's own node.
     *
     * Arms nobody can see are not worth posing, and an actor behind the viewer is the case where writing its
     * bones shows as black bands across the screen: out of view it is still drawn into the shadow pass, which
     * reads bone transforms at a different point in the frame than the main pass and so overlaps the write.
     *
     * This narrows when the write happens rather than making it safe. The race is still there for anyone the
     * viewer is actually looking at.
     *
     * The cone is deliberately generous, rejecting only what is clearly behind. Cutting at the edge of the
     * real field of view would freeze the arms of somebody still visible in peripheral vision, which is a
     * worse artefact than the one being avoided.
     */
    bool IsInView(const glm::vec3& acWorldPosition) noexcept;
    bool ResolveChains(RemoteHands& aHands, Actor* apActor) noexcept;
    void PoseActor(RemoteHands& aHands) noexcept;

    /**
     * @brief Records or verifies the vtable of every cached bone node in both arms.
     *
     * @param aCapture true right after a resolve, to record what each pointer pointed at. False before a
     *                 write, to check they still point at the same objects.
     * @return when capturing, zero. Otherwise the number of cached pointers that no longer match, which is
     *         non-zero only when the actor's 3D was rebuilt without the root address changing.
     */
    size_t AuditPosedNodes(RemoteHands& aHands, bool aCapture) noexcept;

    World& m_world;
    TransportService& m_transport;

    bool m_connected = false;

    /**
     * @brief Runtime off switch, on F10.
     *
     * Posing writes into another actor's live skeleton from a HIGGS worker thread, so it is a credible cause
     * of a remote player rendering wrongly or not at all. It is also credible that such a fault has nothing to
     * do with it. Turning it off in place, without reconnecting or restarting, is the only way to tell those
     * apart, and reconnecting is not a fair test because it stops every other networked service too.
     *
     * Sending is unaffected: this only stops other players' arms being driven on this client.
     */
    bool m_posingEnabled = true;
    bool m_wasF10Down = false;

    /**
     * @brief Restrict posing to the first thread HIGGS ever called us on, toggled with F11.
     *
     * HIGGS calls its post VRIK callback from a worker pool, and a bone transform written there is read by the
     * renderer for skinning. A read that lands mid write sees half of one matrix and half of another, which
     * draws as a triangle stretched across the screen: the black bands, worst when the actor is only in the
     * shadow pass rather than in view.
     *
     * If the calls are one per frame from a rotating pool then pinning to a single thread costs nothing but
     * the frames that arrive on other threads, and the bands should stop. If they persist, the race is with
     * the renderer itself rather than between callbacks, and this write point is not usable as it stands.
     */

    /**
     * @brief Pose from the game's render hook instead of HIGGS's callback, toggled with F1.
     *
     * HIGGS calls its post VRIK callback from whichever job thread happens to run it. Writing a bone there
     * races the renderer reading it, which draws as black bands, and pinning to one thread only trades that
     * for posing on a fraction of frames.
     *
     * The client already hooks the game's frame end through BSGraphics::Hook_StopTimer, which calls
     * RenderSystemD3D11::OnRender on the render thread. That is one thread, every frame, at a fixed point,
     * which is what a bone write needs. Whether it is also late enough for the write to survive to the screen
     * is the open question, and the only way to find out is to try it.
     *
     * On by default. Measured: HIGGS calls this once per frame but spread over a job pool, with only about one
     * call in six landing on any given thread, and black bands still appear occasionally even when writes are
     * restricted to a single thread. So that callback is not a safe place to write a bone from at all, and
     * there is no reason to keep it as the default while a single threaded alternative exists.
     */


    // When PoseAll last ran, so it can age the received poses itself instead of the update thread taking the
    // same lock every frame and making the render thread wait for it.
    std::chrono::high_resolution_clock::time_point m_lastPoseTime{};

    entt::scoped_connection m_drawConnection;


    // Keyed on the sender's server id.
    std::unordered_map<uint32_t, RemoteHands> m_remotes;

    // Guards m_remotes. The posing runs on HIGGS's callback while notifies arrive on the network thread.
    std::mutex m_remotesMutex;

    double m_sinceSend = 0.0;
    glm::vec3 m_lastSent[2]{};
    bool m_hasSent = false;

    // Whether the local player's hands were being synced last tick, so the change can be logged and so a
    // message goes out immediately when it flips rather than waiting for a palm to move.
    bool m_wasActive = true;
    bool m_hasActiveState = false;

    double m_sinceKeepAlive = 0.0;

    /**
     * @brief Timing, because the frame cost of this has been guessed at twice and got it wrong both times.
     *
     * Posing runs on HIGGS's callback, which comes from a worker pool, so it is not visible in any ordinary
     * profile of the update thread. These are written there and read on the update thread once a second.
     */

    // The local character's server id, found once per connection. Looking it up per send means scanning every
    // entity that has a form id, and with a cell's worth of synced objects that is not free at 30 a second.
    uint32_t m_localServerId = 0;

    // Whether the wand node offsets have been checked against the node names. Worth doing, but once, not on
    // every send: the check walks the name a character at a time and each step is a VirtualQuery.
    bool m_wandOffsetsChecked = false;
    bool m_wandOffsetsValid = false;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_handPoseConnection;
};
