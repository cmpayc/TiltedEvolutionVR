#include <Messages/RequestSleepTime.h>

void RequestSleepTime::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    timeModel.Serialize(aWriter);
}

void RequestSleepTime::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    timeModel.Deserialize(aReader);
}
