#include <Messages/RequestObjectRemove.h>

void RequestObjectRemove::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DropId);
    CellId.Serialize(aWriter);
}

void RequestObjectRemove::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    DropId = Serialization::ReadVarInt(aReader);
    CellId.Deserialize(aReader);
}
