#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <World.h>

void InterpolationSystem::Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, const uint64_t aTick) noexcept
{
    auto& movements = aInterpolationComponent.TimePoints;

    if (movements.size() < 2)
        return;

    while (movements.size() > 2)
    {
        const auto second = *(++movements.begin());
        if (aTick > second.Tick)
            movements.pop_front();
        else
            break;
    }

    const auto& first = *(movements.begin());
    const auto& second = *(++movements.begin());

    // Calculate delta movement since last update
    auto delta = 0.0001f;
    const auto tickDelta = static_cast<float>(second.Tick - first.Tick);
    if (tickDelta > 0.f)
    {
        delta = 1.f / tickDelta * static_cast<float>(aTick - first.Tick);
    }

    delta = TiltedPhoques::Min(delta, 1.0f);

    const NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};

    aInterpolationComponent.Position = position;

    // Don't try to move a null actor
    if (!apActor)
        return;

#if TP_SKYRIMVR
    /**
     * A remote actor that moves on its own, measured before the write that would hide it.
     *
     * Only when the stream has run dry: both remaining points are in the past, so `position` is the same
     * value we forced last frame and the actor should be exactly where we left it. Any gap is something
     * else moving it, which is the reported symptom of an NPC drifting slowly through the world.
     *
     * A gap here says the drift is local and our writes are being overridden, the way they already are on a
     * ragdoll, where havok writes the reference back every frame (see PROGRESS.md). No gap on either client
     * says the actor really is walking away in the owner's game and the sync is faithfully relaying it.
     * Those are the two halves of the question and this line separates them.
     *
     * Dead actors are skipped because they fail this by design: `ForcePosition` provably cannot move a
     * ragdoll, so a corpse would report a gap every frame and drown the signal.
     */
    if (aTick > second.Tick && !apActor->IsDead())
    {
        constexpr float kSlipUnits = 5.f;

        const float cSlip = glm::length(glm::vec3(apActor->position) - glm::vec3(position));

        if (cSlip > kSlipUnits)
        {
            static std::chrono::steady_clock::time_point lastSlipWarn;

            const auto now = std::chrono::steady_clock::now();
            if (now - lastSlipWarn >= std::chrono::seconds(1))
            {
                lastSlipWarn = now;

                spdlog::warn("Remote actor {:X} moved {:.1f} units on its own while its stream was pinned: at ({:.1f}, {:.1f}, {:.1f}), we last put it at ({:.1f}, {:.1f}, {:.1f})", apActor->formID, cSlip, apActor->position.x, apActor->position.y, apActor->position.z, position.x, position.y, position.z);
            }
        }
    }
#endif

#if TP_SKYRIMVR
    /**
     * Teleport the collision whenever the body is not already standing where the stream puts it.
     *
     * `ForcePosition` is `SetPosition(position, aSyncHavok)`, and the havok half of that is a warp: the
     * character controller is picked up and put down, not moved. Withholding it sends the reference and the 3D
     * to the streamed position and leaves the collision behind, and a body whose collision is somewhere else
     * cannot be hit, because the game's melee detection sweeps against the character controller rather than
     * against what is drawn.
     *
     * **The test has to be against where the actor is, not against where we last warped it.** That is what
     * this had wrong between 2026-08-23 and 2026-08-26. Comparing the incoming position against the previous
     * incoming position asks whether the *stream* moved, which is a different question: a remote body still
     * runs its own AI and its own havok, so a standing NPC whose stream repeats the same whole-unit position
     * walks its collision away from its reference and the old gate never warped it back. The slip warning
     * above is that same gap, measured, and it fires.
     *
     * Measured on 2026-08-26: an NPC owned by one client could not be hit by the other for half a minute,
     * while its owner hit it freely, and hits started landing on the second its streamed position began
     * changing again, which is the second the old gate resumed warping.
     *
     * The withholding was never the fix for objects drifting either, and the day of 2026-08-23 was spent
     * proving that: rate capping the warps changed nothing, and clearing the remote body's collision layer
     * took the same two minutes of play from twenty three drift events to none. Clearing a body's own layer was
     * then dropped altogether, because it also made the body unhittable; what stops the drift is the layer
     * table, in `DisableCharacterClutterCollision` in CharacterService.
     */
    constexpr float kWarpEpsilon = 0.1f;

    const float cGap = glm::distance(glm::vec3(position), glm::vec3(apActor->position));

    const bool cWarp = !aInterpolationComponent.HasWarped || cGap > kWarpEpsilon;

    apActor->ForcePosition(position, cWarp);

    if (cWarp)
        aInterpolationComponent.HasWarped = true;
#else
    apActor->ForcePosition(position);
#endif

    apActor->LoadAnimationVariables(second.Variables);

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        apActor->currentProcess->middleProcess->direction = second.Direction;
    }

    auto rotA = first.Rotation;
    auto rotB = second.Rotation;

    const auto deltaX = TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta;
    const auto deltaY = TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta;
    const auto deltaZ = TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta;

    auto finalX = TiltedPhoques::Mod(rotA.x + deltaX, float(TiltedPhoques::Pi * 2));
    if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
        finalX -= TiltedPhoques::Pi * 2;

    const auto finalY = TiltedPhoques::Mod(rotA.y + deltaY, float(TiltedPhoques::Pi * 2));
    const auto finalZ = TiltedPhoques::Mod(rotA.z + deltaZ, float(TiltedPhoques::Pi * 2));

    apActor->SetRotation(finalX, finalY, finalZ);
}

void InterpolationSystem::AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept
{
    auto itor = std::begin(aInterpolationComponent.TimePoints);
    const auto end = std::cend(aInterpolationComponent.TimePoints);

    while (itor != end)
    {
        if (itor->Tick > acPoint.Tick)
        {
            aInterpolationComponent.TimePoints.insert(itor, acPoint);

            return;
        }

        ++itor;
    }

    aInterpolationComponent.TimePoints.push_back(acPoint);
}

InterpolationComponent& InterpolationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    return aWorld.emplace_or_replace<InterpolationComponent>(aEntity);
}

void InterpolationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<InterpolationComponent>(aEntity))
        aWorld.remove<InterpolationComponent>(aEntity);
}
