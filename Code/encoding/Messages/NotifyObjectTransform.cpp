#include <Messages/NotifyObjectTransform.h>

void NotifyObjectTransform::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, DropId);
    Position.Serialize(aWriter);

    Serialization::WriteFloat(aWriter, Rotation.x);
    Serialization::WriteFloat(aWriter, Rotation.y);
    Serialization::WriteFloat(aWriter, Rotation.z);

    Serialization::WriteBool(aWriter, IsReleased);
}

void NotifyObjectTransform::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    DropId = Serialization::ReadVarInt(aReader);
    Position.Deserialize(aReader);

    Rotation.x = Serialization::ReadFloat(aReader);
    Rotation.y = Serialization::ReadFloat(aReader);
    Rotation.z = Serialization::ReadFloat(aReader);

    IsReleased = Serialization::ReadBool(aReader);
}
