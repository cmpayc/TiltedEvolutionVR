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

    // The last position the actor's collision was warped to, and whether that has happened at all yet.
    // See InterpolationSystem::Update: the warp is withheld while the body is standing still.
    glm::vec3 LastWarp{};
    bool HasWarped{};
};
