#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>

#include <Components.h>
#include <World.h>
#include <Utils.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

extern thread_local const char* g_animErrorCode;

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

    const auto it = std::begin(actions);
    if (it != std::end(actions) && it->Tick <= aTick)
    {
        // Check if animation graph is ready before attempting to play animations
        if (!apActor->animationGraphHolder.IsReady())
        {
            // Animation graph not ready, keep the action in queue and try again later
            return;
        }
        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = *it;

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        const auto pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));

        apActor->actorState.flags1 = first.State1;
        apActor->actorState.flags2 = first.State2;

        apActor->LoadAnimationVariables(first.Variables);

        aAnimationComponent.LastRanAction = first;

        // Play the animation
        TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
        actionData.eventName = BSFixedString(first.EventName.c_str());
        actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
        actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

        const auto result = ActorMediator::Get()->ForceAction(&actionData);

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::AddActionsForReplay(RemoteAnimationComponent& aAnimationComponent,
                                          const ActionReplayChain& acReplay) noexcept
{
    aAnimationComponent.TimePoints.insert(aAnimationComponent.TimePoints.end(), acReplay.Actions.begin(),
                                          acReplay.Actions.end());
    aAnimationComponent.ReplayCount = acReplay.Actions.size();
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);
}

#if TP_SKYRIMVR
namespace
{
/**
 * @brief Tell the receiving graph the player is walking when the only thing moving them is their own feet.
 *
 * A player crossing their room moves because SkyrimVR follows their headset, not because the game's movement
 * system ran. The behaviour graph therefore reports a standing player: speed zero, no move state. Other clients
 * get a position that travels and an animation that does not, which is a body gliding along in an idle pose.
 *
 * So the locomotion variables are synthesised from how fast the position is actually changing, and only for the
 * local player, whose position is a headset rather than the game's own movement.
 *
 * Positions in the variable tables rather than graph variable ids, because that is what SaveAnimationVariables
 * fills. The player's graph is Master_Behavior, whose float list begins
 * {kTurnDelta, kDirection, kSpeedSampled, kweapAdj, kSpeed, kCastBlend, kPitchOffset, kSpeedDamped, ...},
 * giving the four below. Actor.cpp derives its hand type indices the same way and its answer is confirmed in
 * play, so the method is sound; the size guard covers a descriptor that turned out to be something else.
 *
 * What this does not do is fire moveStart. Skyrim's locomotion is a state machine driven by events as well as by
 * these variables, and if the receiving graph will not leave idle without one then the events have to be
 * synthesised too. Whether that is needed is a question for looking at it, not for guessing, so this is the half
 * that can be written without assuming anything.
 */
void SynthesiseRoomscaleLocomotion(const Actor* acpActor, const glm::vec2& acPosition, ReferenceUpdate& aUpdate, const uint64_t aTick) noexcept
{
    Movement& aMovement = aUpdate.UpdatedMovement;

    constexpr size_t kDirectionIndex = 1;
    constexpr size_t kSpeedSampledIndex = 2;
    constexpr size_t kSpeedIndex = 4;
    constexpr size_t kSpeedDampedIndex = 7;

    // kbInMoveState is the twenty seventh entry of the same descriptor's boolean list.
    constexpr size_t kInMoveStateIndex = 26;

    // Below this the player is shuffling rather than walking, in units per second, about fifteen centimetres a
    // second. Keeps tracking jitter from reading as movement.
    constexpr float kStillSpeed = 10.f;

    // Above this the game's own movement is already running, so the graph is doing the right thing on its own
    // and must not be second guessed. Stick locomotion lands well above it.
    constexpr float kGameMoving = 5.f;

    // How much of the new sample to take. Position arrives every 100ms and a difference over one interval is
    // noisy enough to make the animation stutter between walk and idle. Room walking comes in bursts of two or
    // three steps, so this cannot be heavy either: at 0.45 a burst was over before the average caught up.
    constexpr float kSmoothing = 0.6f;

    /**
     * @brief What the graph is told, over what was measured.
     *
     * Skyrim's locomotion runs on a different scale from a person walking around a room. Measured on
     * 2026-08-24: crossing a room reads 18 to 33 units per second, while the same log's stick locomotion sits at
     * 183 to 370. Handing the graph the real number therefore asks for the very bottom of its walk, which plays
     * as a crawl and reads as the body still gliding.
     *
     * Five puts a room walk into the range the game itself uses for walking.
     *
     * The cost is honest: the feet now cycle faster than the body actually travels, so they scuff rather than
     * plant. That is not fixable by choosing a better number, because the two speed scales genuinely differ. It
     * is a choice between feet that slide and feet that barely move, and this is the direction that was asked
     * for. Only the animation is scaled; the position is already correct and is not touched.
     */
    constexpr float kAnimationSpeedScale = 5.f;

    // Never fast enough to read as a sprint. The top of the range the game's own movement was observed using, so
    // a lunge across the room still animates as running rather than as something no walk cycle covers.
    constexpr float kAnimationSpeedMax = 370.f;

    auto& floats = aMovement.Variables.Floats;
    auto& booleans = aMovement.Variables.Booleans;

    static glm::vec2 s_lastPosition(acPosition);
    static std::chrono::steady_clock::time_point s_lastSample = std::chrono::steady_clock::now();
    static float s_speed = 0.f;
    static bool s_hasSample = false;
    static bool s_moving = false;

    const auto cNow = std::chrono::steady_clock::now();

    /**
     * No usable variable tables means there is nothing to write locomotion into.
     *
     * SaveAnimationVariables sizes these from the actor's graph descriptor and returns without touching them
     * when it cannot find one, which leaves them empty. That is worth recognising if this ever stops working: it
     * would mean the player's animation is not being synced at all, the floating body is not a roomscale problem,
     * and no amount of synthesised speed can fix it because there is no channel to send it down. PROGRESS.md has
     * the last occurrence, where BSAnimationGraphManager's offsets were eight bytes out on VR.
     *
     * Master_Behavior gives thirteen floats and fifty nine booleans, so anything smaller is that case.
     */
    if (floats.size() <= kSpeedDampedIndex || booleans.size() <= kInMoveStateIndex)
        return;

    const float cDelta = std::chrono::duration<float>(cNow - s_lastSample).count();

    const glm::vec2 cStep = acPosition - s_lastPosition;

    s_lastPosition = acPosition;
    s_lastSample = cNow;

    /**
     * @brief Starts and stops the walk, once each, as behaviour graph events.
     *
     * Setting the variables is not enough on its own: Skyrim's locomotion is a state machine and it leaves idle
     * on a moveStart event, not on a speed. The form ids are the graph's own, recorded in the commented Actions
     * enum at the foot of AnimationGraphDescriptor_Master_Behavior.cpp, and they travel raw because a vanilla
     * Skyrim.esm form has the same id on every client.
     *
     * Edge triggered. Repeating moveStart every hundred milliseconds restarts the animation from its first frame
     * each time, which reads as marching on the spot.
     */
    const auto cEmit = [&](const uint32_t acActionId, const char* acpEventName)
    {
        ActionEvent action{};

        action.Tick = aTick;
        action.ActorId = acpActor->formID;
        action.ActionId = acActionId;
        action.State1 = acpActor->actorState.flags1;
        action.State2 = acpActor->actorState.flags2;
        action.EventName = acpEventName;
        action.Variables = aMovement.Variables;

        aUpdate.ActionEvents.push_back(action);
    };

    constexpr uint32_t kMoveStart = 0x959F8;
    constexpr uint32_t kMoveStop = 0x959F9;

    // A cell load or a fast travel moves the player a very long way in one interval. Treated as a discontinuity
    // rather than as running at two thousand units a second.
    constexpr float kTeleportStep = 400.f;

    if (!s_hasSample || cDelta <= 0.f || glm::length(cStep) > kTeleportStep)
    {
        s_hasSample = true;
        s_speed = 0.f;

        return;
    }

    s_speed += (glm::length(cStep) / cDelta - s_speed) * kSmoothing;

    const bool cGameMoving = floats[kSpeedIndex] > kGameMoving;
    const bool cWalking = s_speed >= kStillSpeed && !cGameMoving;

    // Stopping, or handing back to the game's own movement, both need the walk ended or the receiving graph is
    // left in a move state nothing will take it out of.
    if (!cWalking)
    {
        if (s_moving)
        {
            s_moving = false;
            cEmit(kMoveStop, "moveStop");
        }

        return;
    }

    /**
     * The heading, relative to the way the character faces, zero straight ahead.
     *
     * Skyrim measures yaw clockwise from +Y, so forward is (sin, cos) and right is (cos, -sin).
     *
     * The sign convention the graph wants for Direction is not verified. If a sidestep animates as a sidestep in
     * the wrong direction, negate this and nothing else; walking straight ahead is unaffected either way, which
     * is the case that matters most and the one this was written for.
     */
    const float cYaw = acpActor->rotation.z;
    const glm::vec2 cUnit = cStep / glm::length(cStep);

    const float cForward = cUnit.x * std::sin(cYaw) + cUnit.y * std::cos(cYaw);
    const float cRight = cUnit.x * std::cos(cYaw) - cUnit.y * std::sin(cYaw);

    const float cDirection = std::atan2(cRight, cForward);

    const float cAnimationSpeed = std::min(s_speed * kAnimationSpeedScale, kAnimationSpeedMax);

    floats[kSpeedIndex] = cAnimationSpeed;
    floats[kSpeedSampledIndex] = cAnimationSpeed;
    floats[kSpeedDampedIndex] = cAnimationSpeed;
    floats[kDirectionIndex] = cDirection;

    booleans[kInMoveStateIndex] = true;

    // The AI movement direction, which the receiver writes straight onto middleProcess. Same heading, so the two
    // cannot disagree about which way the body is going.
    aMovement.Direction = cDirection;

    // Last, so the event carries the walking variables rather than the standing ones it was sampled with. The
    // receiver loads an action's variables and then fires it, so a moveStart holding a speed of zero starts the
    // graph moving and immediately tells it to stand still.
    if (!s_moving)
    {
        s_moving = true;
        cEmit(kMoveStart, "moveStart");
    }
}
} // namespace
#endif

void AnimationSystem::Serialize(World& aWorld, ClientReferencesMoveRequest& aMovementSnapshot, LocalComponent& localComponent, LocalAnimationComponent& animationComponent, FormIdComponent& formIdComponent)
{
    const auto pForm = TESForm::GetById(formIdComponent.Id);
    const auto pActor = Cast<Actor>(pForm);
    if (!pActor)
        return;

    auto& update = aMovementSnapshot.Updates[localComponent.Id];
    auto& movement = update.UpdatedMovement;

    if (const auto pCell = pActor->parentCell)
        World::Get().GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = pActor->GetWorldSpace())
        World::Get().GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = pActor->position;

    movement.Rotation.x = pActor->rotation.x;
    movement.Rotation.y = pActor->rotation.z;

#if TP_SKYRIMVR
    /**
     * The local player is sent where its headset is, not where its reference is.
     *
     * Nothing in this pipeline filters position: this snapshot goes out every 100ms whatever changed and the
     * server relays it at 50Hz without comparing anything. A step that never reached another player therefore
     * never reached `pActor->position` either, and measurement on 2026-08-24 showed why. The reference holds
     * completely still while the headset drifts about twenty units away, then snaps twenty to forty units
     * forward and the drift resets. The 3D root sits exactly on the reference the whole time, to the tenth of a
     * unit, so it is the reference itself that moves in steps.
     *
     * Adding the drift back gives the headset's horizontal position, which is continuous: the snap forward and
     * the drop in drift are the same distance and cancel.
     *
     * Only the local player, because this is the only actor whose position is a VR headset rather than the
     * game's own movement. Height is left alone: the drift is horizontal and a reference lifted off the floor
     * would be a different problem.
     *
     * HandPoseService::SendLocalPose adds the same offset to its palm origin. They have to agree, or a body
     * moved by this with palms still measured from the reference puts the hands a foot off the chest.
     */
    if (formIdComponent.Id == 0x14)
    {
        const glm::vec2 cRoomscale = Utils::GetRoomscaleOffset();

        movement.Position.x += cRoomscale.x;
        movement.Position.y += cRoomscale.y;
    }
#endif

    pActor->SaveAnimationVariables(movement.Variables);

#if TP_SKYRIMVR
    // After SaveAnimationVariables, which is what fills the tables this writes into. Called before it, the
    // tables are still empty, the size guard inside rejects every call, and the whole thing does nothing without
    // a word about it.
    if (formIdComponent.Id == 0x14)
        SynthesiseRoomscaleLocomotion(pActor, glm::vec2(movement.Position.x, movement.Position.y), update, aMovementSnapshot.Tick);
#endif

    if (pActor->currentProcess && pActor->currentProcess->middleProcess)
    {
        movement.Direction = pActor->currentProcess->middleProcess->direction;
    }

    for (auto& entry : animationComponent.Actions)
    {
        update.ActionEvents.push_back(entry);
    }

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}
