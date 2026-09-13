#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>

struct InterpolationComponent
{
    struct TimePoint
    {
        uint64_t Tick{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        AnimationVariables Variables{};
        float Direction{};

        TimePoint() = default;
        TimePoint(const TimePoint&) = default;
        TimePoint& operator=(const TimePoint&) = default;
    };

    List<TimePoint> TimePoints;
    glm::vec3 Position;

    // Whether the actor's collision has been warped onto its streamed position at all yet. See
    // InterpolationSystem::Update: the first frame warps unconditionally, and every frame after it warps
    // only when the body is not already there.
    bool HasWarped{};

#if TP_SKYRIMVR
    // The character controller the capsule was last read through, and whether it is the rigid body kind that has
    // one to read. Checked by RTTI only when the pointer changes. See ReadCapsule in InterpolationSystem.
    const void* Controller{};
    bool ControllerHasCapsule{};

    // Where the capsule settles across the ground relative to the reference, taken on the frame after a warp, and
    // whether the last frame warped. Not zero for every body: an elk's rests 11.9 units off.
    glm::vec2 CapsuleRest{};
    bool WarpedLastFrame{};
#endif
};
