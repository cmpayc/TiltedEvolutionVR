#pragma once

#include <Actor.h>

struct World;
struct ImguiService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NiAVObject;

/**
 * @brief Throwaway diagnostic for the VR hand sync feature. Answers the questions that decide whether
 * that feature is cheap or expensive, before any of it is built.
 *
 * Three questions, in the order they have to be answered:
 *
 *  1. Where are the VR controller nodes? They are not reachable from the player's actor 3D on VR, so
 *     the probe finds them by scanning PlayerCharacter for a pointer to a node with a matching name and
 *     reports the offset it found. Nothing here hardcodes an offset.
 *  2. Can a bone on *another* actor be written at all? The probe copies a source transform onto a
 *     target actor's hand bone every tick.
 *  3. Does the write survive to the next frame? This is the real question. The animation graph rewrites
 *     the whole skeleton during the actor's update, so a write placed at the wrong point in the frame
 *     is silently discarded. The probe reads the bone back at the start of the next tick and reports
 *     the drift between what it wrote and what it found.
 *
 * A non-zero drift means the write is being overwritten and the hand sync needs a later frame hook than
 * UpdateEvent. A zero drift means everything after this is ordinary plumbing.
 *
 * None of this is networked and none of it is meant to ship. Delete the whole service once the
 * questions are answered.
 */
struct SkeletonProbeService
{
    SkeletonProbeService(entt::dispatcher& aDispatcher, World& aWorld, ImguiService& aImguiService);
    ~SkeletonProbeService() noexcept = default;

    TP_NOCOPYMOVE(SkeletonProbeService);

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

    /**
     * @brief A bone to pose, paired with its slot in BSFlattenedBoneTree's transform array.
     *
     * Both have to be written. The node is what the pose maths reads back and what other code sees; the array
     * entry is what the skinning actually renders from. Writing only the node produces a correct skeleton that
     * never appears on screen, which is what it did for many iterations.
     *
     * pFlatEntry is null when the bone has no entry, which is legitimate for attachment nodes. Public because
     * the posing helpers in the implementation take it.
     */
    struct PosedNode
    {
        NiAVObject* pNode{nullptr};
        uint8_t* pFlatEntry{nullptr};
    };

protected:
    void OnDraw() noexcept;

private:
    // One end of the copy. Resolved fresh every tick, because 3D comes and goes with cell loads.
    struct NodeRef
    {
        NiAVObject* pNode{nullptr};
        String Name{};
        glm::vec3 World{};
        bool Resolved{false};
    };

    /**
     * @brief The three bones of one arm, each with every node hanging off it.
     *
     * Writing a bone's world transform moves that bone and nothing else, because the game only derives children
     * from locals during the downward pass, which has already run by the time the write happens. So every
     * descendant has to be carried across by hand, which is why whole subtrees are collected rather than a list
     * of bone names.
     *
     * Measured on an NPC: an upper arm has seven nodes below it, being the forearm, the hand, two upper arm
     * twist bones and three attachment nodes. There are no finger bones and no forearm twist bones on an NPC
     * skeleton, unlike the player's.
     *
     * Cached per target: resolving by name every frame means a BSFixedString per bone per frame, and this
     * client's BSFixedString destructor does not release its interned reference. The subtree walk is far too
     * expensive to repeat per frame as well.
     *
     * std::vector rather than TiltedPhoques::Vector, following HiggsService: this is filled from HIGGS's
     * callback and the TiltedPhoques allocators are per thread.
     */
    struct ArmChain
    {
        PosedNode UpperArm{};
        PosedNode Forearm{};
        PosedNode Hand{};

        // Descendants only. The bone itself is written directly.
        std::vector<PosedNode> UpperSubtree;
        std::vector<PosedNode> ForeSubtree;
        std::vector<PosedNode> HandSubtree;

        bool HasCore() const noexcept { return UpperArm.pNode && Forearm.pNode && Hand.pNode; }
    };

    void DumpPlayerTree() noexcept;
    void DumpTargetTree() noexcept;
    void ScanPlayerForWandNodes() noexcept;
    void PickNearestTarget() noexcept;

    void RunDiscovery() noexcept;
    void LogVerdict() noexcept;

    // One read back plus one write. Called from whichever point in the frame is being tested.
    void Tick() noexcept;
    void InstallHiggsHook() noexcept;

    // Index 0 is left, 1 is right. Prefers the PlayerCharacter offset the scan found, and falls back to a
    // name search from the actor's 3D.
    NiAVObject* ResolveSource(size_t aHand) noexcept;

    // Resolves and caches both arm chains against the current target, logging exactly which bones were found
    // so a missing one is visible instead of silently doing nothing.
    void ResolveArmChains(NiNode* apTargetRoot) noexcept;

    // True once the flattened array's layout has been confirmed against live data for this target. Writes to
    // the array are gated on it, because a wrong offset there corrupts a skeleton silently rather than crashing.
    bool m_flatLayoutConfirmed = false;

    void Apply() noexcept;
    void CheckDrift() noexcept;

    World& m_world;

    // On by default. The probe exists to produce one answer, so it should not need a keypress to do it.
    // F4 turns it off if the sight of a driven NPC gets in the way.
    bool m_enabled = true;

    // On now that the write point is confirmed to reach the screen. Writing only the hand bone leaves the
    // elbow and shoulder on the target's own pose, and the forearm mesh stretches across the gap, which is
    // the "very long arms" the first working run produced. F5 toggles it to see the difference.
    bool m_solveArmChain = true;

    // Which node names to copy from. Index 0 is left, index 1 is right. Filled from the wand scan, or
    // left at the player's own hand bones when no wand node is found, which is what an SE build gets.
    String m_sourceName[2]{};

    // Byte offset into PlayerCharacter where the scan found the source node's pointer. Zero means it was
    // not found there and the source has to be looked up by name from the actor's 3D instead. The VR
    // controller nodes are not in the actor's 3D at all, so for them this is the only way in.
    size_t m_sourceOffset[2]{};

    // Index into the candidate name list that the current pick came from, lower being more wanted. The
    // player's own hand bones are cached in PlayerCharacter at a *higher* offset than the wand nodes, so a
    // scan that walks offsets upward and keeps the last match ends up preferring exactly the wrong one.
    int m_sourcePriority[2]{};

    uint32_t m_targetFormId = 0;

    NodeRef m_source[2]{};
    NodeRef m_target[2]{};

    ArmChain m_chain[2]{};

    /**
     * @brief A fixed reference orientation per bone, so the posed roll does not come from the animation.
     *
     * Turning a bone from where it points to where it should point preserves its rest orientation, which is
     * what stopped the shoulder facing backwards. It also preserves whatever roll the animation happens to
     * have applied that frame, so a walking actor's arms spin about their own length as the walk cycle plays.
     *
     * Composing onto a captured reference instead of the live rotation makes the roll a constant. It is stored
     * relative to the actor's root so it stays correct when the actor turns, and captured once per target
     * rather than on every re-resolve, which would put the animation straight back in.
     *
     * Index is [hand][0 = upper arm, 1 = forearm].
     */
    glm::mat3 m_restRotate[2][2]{};
    glm::vec3 m_restDir[2][2]{};
    bool m_restCaptured = false;

    // Which actor m_chain was resolved against. Zero forces a re-resolve.
    uint32_t m_chainFor = 0;

    // The chain is re-resolved on a timer, not just once per target. An actor's skeleton is not fully built
    // the instant it becomes targetable, and resolving once meant caching a nearly empty subtree for good.
    double m_sinceChainResolve = 0.0;

    // Subtree sizes at the last resolve, so a change can be reported without logging every second.
    size_t m_lastSubtreeCount[2]{};

    /**
     * @brief The geometry of the last solve, so it can be checked as numbers rather than guessed at from a
     * screenshot.
     *
     * Three rounds of diagnosing this by eye produced three wrong answers. What matters is whether the posed
     * bones are mutually consistent: the upper arm and forearm should each still be their original length
     * after posing, and the hand should sit exactly one forearm from the elbow. Any of those being off is a
     * maths bug. All of them holding while the mesh still stretches means the problem is in how the pose
     * reaches the skin, not in the pose.
     */
    struct SolveDebug
    {
        glm::vec3 Shoulder{};
        glm::vec3 Elbow{};
        glm::vec3 Hand{};
        float UpperLen{};
        float LowerLen{};
        float GoalDistance{};
        bool Clamped{};
        bool Solved{};

        /**
         * @brief Orthonormality of the rotation written to the hand bone.
         *
         * The arm joints get rotations from AimRotation, which builds them from normalized orthogonal vectors,
         * so they cannot be anything but orthonormal. The hand instead gets the controller's rotation passed
         * through a change of basis, and none of those matrices has ever been checked. A matrix with a column
         * length other than 1, or a negative determinant, is a scale or a mirror rather than a rotation, and
         * writing one to a bone stretches or inverts the mesh skinned to it. That is the exact symptom.
         */
        float HandColumnLength[3]{};
        float HandDeterminant{};
    };

    SolveDebug m_solve[2]{};

    // What Apply() last wrote, and what CheckDrift() found there one tick later.
    glm::vec3 m_written[2]{};
    glm::vec3 m_readBack[2]{};
    float m_drift[2]{};
    float m_worstDrift = 0.f;
    bool m_hasWritten[2]{};

    uint32_t m_writes = 0;

    // Bone was re-posed by the animation before the next frame's write. Expected, and not a failure: a bone
    // override is written every frame by design. Kept only to show the mechanism in the log.
    uint32_t m_overwritten = 0;

    // The write did not reach the memory the game reads. This one is a real failure.
    uint32_t m_writeFailures = 0;

    // Nothing at all happens until the client is connected to a server. In the main menu the player's 3D
    // and the VR node block are in a half set up state that is not worth reading, and a probe reporting on
    // it would only produce noise. A reconnect re-runs discovery from scratch.
    bool m_connected = false;

    // Discovery then runs itself on the first update after connecting, so a plain session produces the
    // answer in tp_client.log without anyone having to click an overlay with a headset on.
    bool m_discovered = false;
    double m_sinceLog = 0.0;

    // Nearby actors are not loaded yet when discovery runs, so target selection retries until one turns up.
    double m_sinceTargetTry = 0.0;

    // Own edge detection. GetAsyncKeyState's low "pressed since last call" bit is shared system wide and
    // gets eaten by whatever else polls the same key, so the high bit is read and the transition tracked here.
    bool m_wasF4Down = false;
    bool m_wasF5Down = false;
    bool m_wasF6Down = false;

    /**
     * @brief Pose the arms straight up at a fixed point instead of tracking the controllers.
     *
     * This separates two failure modes that every previous test conflated. Driving from a moving controller
     * goal makes a broken write pipeline and a wrong goal look identical, both showing up as "strange
     * stretching". A static, unmistakable target cannot be confused for either: the arms either point straight
     * up and stay there, or the pose is not reaching the screen.
     *
     * Should have been the first test rather than the seventh.
     */
    bool m_staticPose = false;

    /**
     * @brief Which single bone of the chain to pose, for mapping bones to mesh regions.
     *
     * 0 poses the whole chain, 1 the hand only, 2 the forearm only, 3 the upper arm only.
     *
     * The hand bone and its array slot both demonstrably hold the posed position while the hand mesh renders
     * where the animation left it, so the mesh is skinned to something that is not being written. Reading the
     * skin instance's bone list would need offsets that are not verified. Moving one bone at a time and watching
     * which part of the body follows establishes the same mapping using only what is already known to work.
     */
    int m_isolate = 0;
    bool m_wasF12Down = false;

    // Which point in the frame the write is being made from. UpdateEvent is the cheap one and is known not
    // to survive; HIGGS's post VRIK callback runs after the skeleton has been posed, which is the next
    // candidate. Installed once and never removed, because HIGGS cannot unregister a callback.
    bool m_higgsHookInstalled = false;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_drawConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
};
