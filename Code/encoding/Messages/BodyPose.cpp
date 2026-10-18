#include <Messages/RequestBodyPose.h>
#include <Messages/NotifyBodyPose.h>

void RequestBodyPose::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Id, 32);
    Body.Serialize(aWriter);
}
void RequestBodyPose::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    uint64_t id{};
    const bool ok = aReader.ReadBits(id, 32);
    Id = static_cast<uint32_t>(id);
    Body.Deserialize(aReader);
    Body.Decoded = Body.Decoded && ok;
}
void NotifyBodyPose::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Id, 32);
    aWriter.WriteBits(ServerTick, 64);
    Body.Serialize(aWriter);
}
void NotifyBodyPose::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t id{};
    const bool ok = aReader.ReadBits(id, 32) && aReader.ReadBits(ServerTick, 64);
    Id = static_cast<uint32_t>(id);
    Body.Deserialize(aReader);
    Body.Decoded = Body.Decoded && ok && ServerTick;
}
