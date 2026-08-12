#include <Messages/NotifyObjectRemove.h>

void NotifyObjectRemove::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DropId);
}

void NotifyObjectRemove::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    DropId = Serialization::ReadVarInt(aReader);
}
