#include <Structs/Quaternion_NetQuantize.h>
#include <TiltedCore/Serialization.hpp>

#include <algorithm>
#include <cmath>

using TiltedPhoques::Serialization;

namespace
{
/**
 * @brief int16 full scale, so one component resolves to about 3e-5 and the worst case angle error is well
 *        under a hundredth of a degree.
 *
 * Symmetric on purpose: 0x8000 is left unused rather than special cased, which keeps Pack and Unpack inverses
 * of each other to within a rounding step.
 */
constexpr float cScale = 32767.f;

constexpr glm::quat cIdentity(1.f, 0.f, 0.f, 0.f);

int16_t Quantize(const float aValue) noexcept
{
    return static_cast<int16_t>(std::lroundf(std::clamp(aValue, -1.f, 1.f) * cScale));
}

float Dequantize(const uint64_t aPacked, const int aShift) noexcept
{
    return static_cast<float>(static_cast<int16_t>((aPacked >> aShift) & 0xFFFF)) / cScale;
}

// A quaternion that is not a rotation cannot be sent as one, and identity is the honest substitute: it arrives
// as "no rotation applied" rather than as a matrix that collapses whatever it is applied to.
glm::quat Normalized(const glm::quat& acValue) noexcept
{
    const float cLength = glm::length(acValue);

    return cLength > 0.0001f ? acValue / cLength : cIdentity;
}
} // namespace

bool Quaternion_NetQuantize::operator==(const Quaternion_NetQuantize& acRhs) const noexcept
{
    return Pack() == acRhs.Pack();
}

bool Quaternion_NetQuantize::operator!=(const Quaternion_NetQuantize& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

Quaternion_NetQuantize& Quaternion_NetQuantize::operator=(const glm::quat& acRhs) noexcept
{
    glm::quat::operator=(acRhs);
    return *this;
}

void Quaternion_NetQuantize::Serialize(Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Pack(), 64);
}

void Quaternion_NetQuantize::Deserialize(Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 64);

    Unpack(data);
}

uint64_t Quaternion_NetQuantize::Pack() const noexcept
{
    glm::quat value = Normalized(*this);

    // q and -q are the same rotation, so one of the two is chosen. Without this the same pose packs to two
    // different values and every comparison against a previously sent one, operator== included, is unreliable.
    if (value.w < 0.f)
        value = -value;

    uint64_t data = 0;

    data |= static_cast<uint64_t>(static_cast<uint16_t>(Quantize(value.w)));
    data |= static_cast<uint64_t>(static_cast<uint16_t>(Quantize(value.x))) << 16;
    data |= static_cast<uint64_t>(static_cast<uint16_t>(Quantize(value.y))) << 32;
    data |= static_cast<uint64_t>(static_cast<uint16_t>(Quantize(value.z))) << 48;

    return data;
}

void Quaternion_NetQuantize::Unpack(const uint64_t aValue) noexcept
{
    const glm::quat cRead(Dequantize(aValue, 0), Dequantize(aValue, 16), Dequantize(aValue, 32), Dequantize(aValue, 48));

    // Quantizing the four components independently takes the quaternion slightly off the unit sphere, and
    // mat3_cast of one that is not unit length scales as well as rotates.
    glm::quat::operator=(Normalized(cRead));
}
