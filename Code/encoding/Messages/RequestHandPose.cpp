#include <Messages/RequestHandPose.h>

void RequestHandPose::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);

    LeftPalm.Serialize(aWriter);
    RightPalm.Serialize(aWriter);

    LeftPalmRotation.Serialize(aWriter);
    RightPalmRotation.Serialize(aWriter);

    Serialization::WriteBool(aWriter, HandsActive);
    Serialization::WriteBool(aWriter, HandsRotationValid);
    Serialization::WriteFloat(aWriter, EyeHeight);

    HeadRotation.Serialize(aWriter);
    Serialization::WriteBool(aWriter, HeadRotationValid);

    LeftGripRotation.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, LeftGripOffset.x);
    Serialization::WriteFloat(aWriter, LeftGripOffset.y);
    Serialization::WriteFloat(aWriter, LeftGripOffset.z);
    RightGripRotation.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, RightGripOffset.x);
    Serialization::WriteFloat(aWriter, RightGripOffset.y);
    Serialization::WriteFloat(aWriter, RightGripOffset.z);
    Serialization::WriteBool(aWriter, LeftGripValid);
    Serialization::WriteBool(aWriter, RightGripValid);
    Serialization::WriteBool(aWriter, Drawn);
}

void RequestHandPose::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    LeftPalm.Deserialize(aReader);
    RightPalm.Deserialize(aReader);

    LeftPalmRotation.Deserialize(aReader);
    RightPalmRotation.Deserialize(aReader);

    HandsActive = Serialization::ReadBool(aReader);
    HandsRotationValid = Serialization::ReadBool(aReader);
    EyeHeight = Serialization::ReadFloat(aReader);

    HeadRotation.Deserialize(aReader);
    HeadRotationValid = Serialization::ReadBool(aReader);

    LeftGripRotation.Deserialize(aReader);
    LeftGripOffset.x = Serialization::ReadFloat(aReader);
    LeftGripOffset.y = Serialization::ReadFloat(aReader);
    LeftGripOffset.z = Serialization::ReadFloat(aReader);
    RightGripRotation.Deserialize(aReader);
    RightGripOffset.x = Serialization::ReadFloat(aReader);
    RightGripOffset.y = Serialization::ReadFloat(aReader);
    RightGripOffset.z = Serialization::ReadFloat(aReader);
    LeftGripValid = Serialization::ReadBool(aReader);
    RightGripValid = Serialization::ReadBool(aReader);
    Drawn = Serialization::ReadBool(aReader);
}
