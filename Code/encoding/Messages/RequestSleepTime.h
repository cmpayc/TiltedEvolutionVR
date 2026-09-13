#pragma once

#include "Message.h"
#include "Structs/TimeModel.h"

/**
 * @brief Asks the server to move the shared clock to where sleeping has just left this client's own.
 *
 * The whole time model rather than an hour, because sleeping crosses midnight: eight hours from ten in the
 * evening lands on the following day, and a message that carried only the hour would leave the server a day
 * behind and hand the old date straight back on the next resync.
 *
 * TimeScale travels with it and is ignored on arrival. The server's configuration owns the rate at which time
 * passes and a client has no business changing it; it is in the struct because the struct is shared with
 * ServerTimeSettings, which does carry it.
 */
struct RequestSleepTime final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSleepTime;

    RequestSleepTime()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestSleepTime() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestSleepTime& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && timeModel == acRhs.timeModel; }

    TimeModel timeModel{};
};
