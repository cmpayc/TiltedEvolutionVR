#include <Structs/BodyPose.h>
#include <bit>
#include <cmath>

namespace BodyTracking
{
namespace
{
bool ValidBone(const BonePose& aBone) noexcept
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(aBone.Position[i]) || std::abs(aBone.Position[i]) > kMaxTranslation)
            return false;
    float norm = 0.f;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::isfinite(aBone.Rotation[i])) return false;
        norm += aBone.Rotation[i] * aBone.Rotation[i];
    }
    return norm > 0.9f && norm < 1.1f;
}
void WriteBone(TiltedPhoques::Buffer::Writer& aWriter, const BonePose& aBone) noexcept
{
    for (int i = 0; i < 3; ++i)
        aWriter.WriteBits(std::bit_cast<uint32_t>(aBone.Position[i]), 32);
    aWriter.WriteBits(aBone.Rotation.Pack(), 64);
}
bool ReadBone(TiltedPhoques::Buffer::Reader& aReader, BonePose& aBone) noexcept
{
    uint64_t value{};
    for (int i = 0; i < 3; ++i)
    {
        if (!aReader.ReadBits(value, 32)) return false;
        aBone.Position[i] = std::bit_cast<float>(static_cast<uint32_t>(value));
    }
    if (!aReader.ReadBits(value, 64)) return false;
    aBone.Rotation.Unpack(value);
    return ValidBone(aBone);
}
}

bool Pose::IsValid() const noexcept
{
    if (!Decoded || !Sequence || !CaptureTick || !Generation || (Mask & ~kKnownMask) || (GripMask & ~3u))
        return false;
    if (Mask && !HasBody()) return false;
    if (!Mask && GripMask) return false;
    for (size_t i = 0; i < Bones.size(); ++i)
        if ((Mask & (1u << i)) && !ValidBone(Bones[i])) return false;
    for (size_t i = 0; i < Grips.size(); ++i)
        if ((GripMask & (1u << i)) && (!ValidBone(Grips[i]) || !GripItems[i])) return false;
    return true;
}

void Pose::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(2, 8); // world-axis body + exact grip item schema
    aWriter.WriteBits(Sequence, 64);
    aWriter.WriteBits(CaptureTick, 64);
    aWriter.WriteBits(Generation, 32);
    aWriter.WriteBits(Mask, 32);
    aWriter.WriteBits(GripMask, 8);
    // No variable count/allocation: the schema fixes all possible slots.
    for (size_t i = 0; i < Bones.size(); ++i)
        if (Mask & (1u << i)) WriteBone(aWriter, Bones[i]);
    for (size_t i = 0; i < Grips.size(); ++i)
        if (GripMask & (1u << i))
        {
            WriteBone(aWriter, Grips[i]);
            aWriter.WriteBits(GripItems[i].ModId, 32);
            aWriter.WriteBits(GripItems[i].BaseId, 32);
        }
}

void Pose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = Pose{};
    Decoded = false;
    uint64_t value{};
    if (!aReader.ReadBits(value, 8) || value != 2) return;
    if (!aReader.ReadBits(Sequence, 64) || !aReader.ReadBits(CaptureTick, 64)) return;
    if (!aReader.ReadBits(value, 32)) return;
    Generation = static_cast<uint32_t>(value);
    if (!aReader.ReadBits(value, 32)) return;
    Mask = static_cast<uint32_t>(value);
    if (Mask & ~kKnownMask) return;
    if (!aReader.ReadBits(value, 8) || (value & ~3u)) return;
    GripMask = static_cast<uint8_t>(value);
    for (size_t i = 0; i < Bones.size(); ++i)
        if ((Mask & (1u << i)) && !ReadBone(aReader, Bones[i])) return;
    for (size_t i = 0; i < Grips.size(); ++i)
        if (GripMask & (1u << i))
        {
            if (!ReadBone(aReader, Grips[i]) || !aReader.ReadBits(value, 32)) return;
            GripItems[i].ModId = static_cast<uint32_t>(value);
            if (!aReader.ReadBits(value, 32)) return;
            GripItems[i].BaseId = static_cast<uint32_t>(value);
        }
    Decoded = true;
    if (!IsValid()) Decoded = false;
}
}
