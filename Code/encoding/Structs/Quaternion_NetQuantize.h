#pragma once

#include <glm/gtc/quaternion.hpp>

using TiltedPhoques::Buffer;

/**
 * @brief Network optimized unit quaternion, four int16 components in 64 bits.
 *
 * A rotation matrix would be nine floats, and a euler triple needs a sender and receiver that agree on axis
 * order, which nothing in this codebase does consistently.
 *
 * Do not reach for Vector3_NetQuantize to carry a rotation. It truncates each component to a whole number, and
 * every quaternion component lives in [-1, 1], so all four would land on 0 or +-1.
 */
struct Quaternion_NetQuantize : glm::quat
{
    // Identity rather than = default, because a zero initialized quaternion is not a rotation: mat3_cast turns
    // it into a zero matrix, which collapses whatever it is applied to instead of leaving it alone.
    Quaternion_NetQuantize() noexcept
        : glm::quat(1.f, 0.f, 0.f, 0.f)
    {
    }

    ~Quaternion_NetQuantize() = default;

    /**
     * Equality of the packed wire value, which is what the other _NetQuantize types compare too.
     *
     * Note this is not equality of rotation to full float precision, and the type is lossy, so a quaternion
     * that has been through a round trip does not necessarily pack back to the value it came from.
     */
    bool operator==(const Quaternion_NetQuantize& acRhs) const noexcept;
    bool operator!=(const Quaternion_NetQuantize& acRhs) const noexcept;

    Quaternion_NetQuantize& operator=(const glm::quat& acRhs) noexcept;

    /**
     * Serialize to a buffer.
     * @param aWriter Writer wrapping the buffer.
     */
    void Serialize(Buffer::Writer& aWriter) const noexcept;
    /**
     * Deserialize from a buffer.
     * @param aReader Reader wrapping the buffer.
     */
    void Deserialize(Buffer::Reader& aReader) noexcept;
    /**
     * Packs the quaternion into a 64 bits representation of the network quaternion
     */
    [[nodiscard]] uint64_t Pack() const noexcept;
    /**
     * Unpack a 64 bits representation of a network quaternion
     * @param aValue The 64bits representation of a quaternion.
     */
    void Unpack(uint64_t aValue) noexcept;
};
