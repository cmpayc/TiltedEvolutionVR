#pragma once

#include <glm/glm.hpp>

namespace BodyTracking
{
// Shared by the existing movement sender and the body capture frame. Preserve
// the original HMD-minus-reference arithmetic and never add headset height.
inline glm::vec2 RoomscaleOffset(const glm::vec3& aReference, const glm::vec3& aHmdWorld) noexcept
{
    return {aHmdWorld.x - aReference.x, aHmdWorld.y - aReference.y};
}
}
