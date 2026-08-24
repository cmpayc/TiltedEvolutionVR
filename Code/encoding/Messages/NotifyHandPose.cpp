#include <Messages/NotifyHandPose.h>

void NotifyHandPose::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);

    LeftPalm.Serialize(aWriter);
    RightPalm.Serialize(aWriter);

    LeftPalmRotation.Serialize(aWriter);
    RightPalmRotation.Serialize(aWriter);

    Serialization::WriteBool(aWriter, HandsActive);
    Serialization::WriteBool(aWriter, HandsRotationValid);
    Serialization::WriteFloat(aWriter, EyeHeight);
}

void NotifyHandPose::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    LeftPalm.Deserialize(aReader);
    RightPalm.Deserialize(aReader);

    LeftPalmRotation.Deserialize(aReader);
    RightPalmRotation.Deserialize(aReader);

    HandsActive = Serialization::ReadBool(aReader);
    HandsRotationValid = Serialization::ReadBool(aReader);
    EyeHeight = Serialization::ReadFloat(aReader);
}
